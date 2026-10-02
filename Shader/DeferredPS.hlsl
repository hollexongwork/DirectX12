// コンタクトシャドウ (GetShadowTerms -> ShadowRayCast) はシーン深度 (t3) を読むので、
// デファードライティングパスだけが有効にする
#define SUPPORT_CONTACT_SHADOWS 1

#include "PBR_Utility.hlsl"
#include "ForwardLightingCommon.hlsl"	// DeferredLightingCommon / SubstrateEvaluation / ShadowFilteringCommon / LightGridCommon
#include "TemporalAACommon.hlsl"	// InterleavedGradientNoise (自己テスト Out[36] と同じ関数。レジスタ宣言無し)

// ---- Lumen スクリーン GI (半球コーントレース + Surface Cache 採光) ----
// SDF アトラス / サンプラーは既存リソースを共通名へエイリアスして
// LumenTracingCommon を取り込む (t24-t27 は Resources.hlsl で宣言済み)。
#define LumenDistanceFieldAtlas DistanceFieldAtlasTexture
#define LumenTraceSampler Sampler2
#include "LumenTracingCommon.hlsl"

// -------------------------------------------------------------
//  LumenScreenGather
//  G-Buffer ピクセルから半球コーンを Lumen シーン (メッシュ SDF) に
//  トレースし、ヒット先の Surface Cache FinalLighting (Emissive
//  合成済み) を採光する (Final Gather のピクセル毎簡易版)。
//    OutRadiance      : 半球コサイン平均の入射ラディアンス
//                       (拡散反射は albedo * OutRadiance)
//    OutSkyVisibility : ミス方向の割合 (IBL の遮蔽 = DFAO 相当)
// -------------------------------------------------------------
void LumenScreenGather(float3 WorldPos, float3 Normal, uint2 PixelPos,
    out float3 OutRadiance, out float OutSkyVisibility)
{
    OutRadiance = float3(0.0f, 0.0f, 0.0f);
    OutSkyVisibility = 1.0f;

    const uint numCones = clamp(LumenNumScreenCones, 1u, 8u);

    // Interleaved Gradient Noise でピクセルごとにコーンリングを回転。
    // UE InterleavedGradientNoise と同じフレーム項 (View.StateFrameIndexMod8) で 8 フレーム周期に回し、
    // 静止ノイズを TAA に積分させる。AA 無効時は StateFrameIndexMod8 = 0 なので従来の画像と同一
    float ign = InterleavedGradientNoise(float2(PixelPos), (float)StateFrameIndexMod8);
    float randomRotation = ign * 6.2831853f;

    float3 rayStart = WorldPos + Normal * LumenSurfaceBias;

    float visibilitySum = 0.0f;

    [loop]
    for (uint c = 0; c < numCones; ++c)
    {
        float3 rayDir = GetLumenHemisphereRay(Normal, c, numCones, randomRotation);

        FLumenTraceResult trace = TraceLumenScene(
            rayStart, rayDir, LumenMaxTraceDistance,
            LumenConeTanAngle, NumLumenObjects);

        [branch]
        if (trace.bHit)
        {
            FLumenSceneObject hitObj = LumenSceneObjects[trace.HitObject];
            float3 hitPos = rayStart + rayDir * trace.HitT;
            float3 hitNormal = ComputeLumenHitNormal(hitObj, hitPos, rayStart);

            OutRadiance += SampleLumenSurfaceCache(hitObj, hitPos, hitNormal);
        }

        visibilitySum += trace.Visibility;
    }

    OutRadiance /= (float) numCones;
    OutSkyVisibility = visibilitySum / (float) numCones;
}

// =============================================================
//  DeferredPS
//  ディファードライティングパス。G-Buffer と Substrate バッファ
//  (t22/t23) を読み、ピクセルごとに 2 経路へ分岐する:
//    - SubstrateMaterial0.x != 0 : Substrate Slab BSDF
//      (SubstrateEvaluation.hlsl。SSS / F0-F90 / 第2ローブ / ファズ)
//    - それ以外                  : レガシー Metallic/Specular
//      (CookTorrance + IBL_Ambient)
//  直接光は両経路とも ForwardLightingCommon.hlsl の
//  GetForwardDirectLighting / GetForwardDirectLightingSubstrate
//  (全ディレクショナルライト + ライトグリッドのローカルライト)。
//  ライト 1 灯の減衰 / シャドウ / 面光源の積分は UE と同じ
//  GetDynamicLighting (DeferredLightingCommon.hlsl)。
// =============================================================

PS_OUTPUT main(PS_INPUT input)
{
    PS_OUTPUT output;

    // ---- GBufferからデータ取得 ----
    float4 baseColor = TextureBaseColor.Sample(Sampler2, input.TexCoord) * input.Color;
    float4 normalSample = TextureNormal.Sample(Sampler2, input.TexCoord);
    float4 msra = TextureMSRA.Sample(Sampler2, input.TexCoord);

    // ---- Unlitマーカー判定: Normal.w == 0.0 ならライティングをスキップ ----
    // GeometryPSでUnlitオブジェクトはNormal.w=0に書き込んでいる
    if (normalSample.w < 0.5f)
    {
        // EmissionColor + BaseColor がそのまま入っているので素通し
        output.Color = float4(baseColor.rgb, 1.0f);
        return output;
    }

    float3 normal = normalize(normalSample.xyz);

    // MSRA GBuffer: R=Metallic, G=Specular, B=Roughness, A=Occlusion

    float metallic = msra.r;
    float roughness = msra.b;
    float occlusion = msra.a;

    // ---- ワールド座標の再構築 ----
    float4 worldPos;
    {
        float2 uv = input.TexCoord;
        float depth = TextureDepth.Sample(Sampler2, uv).r;

        float4 ndcPos = float4(
             uv.x * 2.0f - 1.0f,
            (1.0f - uv.y) * 2.0f - 1.0f,
             depth,
             1.0f);

        worldPos = mul(ndcPos, InvViewProjection);
        worldPos /= worldPos.w;
    }

    // ---- ライティング ----
    float3 viewDir = normalize(WorldCameraOrigin.xyz - worldPos.xyz);

    // ---- ディレクショナルシャドウ (CSM + DF) ----
    // 選択されたフォワードディレクショナルライトの影。
    // カスケード選択はカメラビュー空間の深度で行う
    float viewDepth = mul(float4(worldPos.xyz, 1.0f), View).z;
    float directionalShadow = GetDirectionalShadow(worldPos.xyz, normal, viewDepth);

    // ---- Substrate ピクセル判定 (RT3.x = ヘッダ, 0 = レガシー) ----
    uint4 substrateData0 = SubstrateMaterial0.Load(int3(input.Position.xy, 0));

    // ---- ライトグリッドセル (両経路共通) ----
    uint3 GridCoordinate = ComputeLightGridCellCoordinate(uint2(input.Position.xy), viewDepth);
    uint GridIndex = ComputeLightGridCellIndex(GridCoordinate);

    // ---- Lumen スクリーン GI (両経路共通の前計算) ----
    // lumenRadiance     : 半球コサイン平均の入射ラディアンス
    //                     (Surface Cache 経由 = Emissive 面の光を含む)
    // lumenSkyVisibility: スカイ可視率。IBL をこの係数で減衰することで
    //                     GI との二重計上を抑えつつ DFAO としても機能する
    // GatherMode 2 = Screen Probe Gather (LumenScreenProbeIntegrate_CS の
    //                積分結果 t28 を読むだけ)
    // GatherMode 1 = ピクセル毎コーントレース (フォールバック経路)
    float3 lumenRadiance = float3(0.0f, 0.0f, 0.0f);
    float lumenSkyVisibility = 1.0f;

    [branch]
    if (LumenGatherMode == 2u)
    {
        float4 diffuseIndirect = LumenDiffuseIndirectTexture.Load(int3(input.Position.xy, 0));
        lumenRadiance = diffuseIndirect.rgb;
        lumenSkyVisibility = diffuseIndirect.a;
    }
    else if (bLumenScreenGI != 0u)
    {
        LumenScreenGather(worldPos.xyz, normal, uint2(input.Position.xy),
            lumenRadiance, lumenSkyVisibility);
    }

    float skyOcclusion = lerp(1.0f, lumenSkyVisibility, LumenSkyOcclusionStrength);

    // ---- Lumen Reflections (プレフィルタ差し替え値, t29) ----
    // rgb = トレース済みラディアンス, a = 差し替え率。
    // IBL スペキュラの prefiltered サンプルをこの値で lerp する。
    float4 lumenReflection = float4(0.0f, 0.0f, 0.0f, 0.0f);

    [branch]
    if (bLumenReflections != 0u)
    {
        lumenReflection = LumenReflectionTexture.Load(int3(input.Position.xy, 0));
    }

    float3 sceneLighting;
    float3 lumenDiffuseAlbedo; // GI の拡散反射に使うアルベド (経路ごとに解決)

    // ---- 直接光 (ディレクショナル全灯 + ライトグリッドのローカルライト) ----
    // ライト 1 灯の評価は UE と同じ GetDynamicLighting / SubstrateDeferredLighting
    // (ForwardLightingCommon.hlsl)。コンタクトシャドウのディザは 8 フレーム周期で回す
    // (AA 無効時は StateFrameIndexMod8 = 0 で固定パターン)
    const float3 cameraVector = -viewDir;
    const float contactShadowDither = InterleavedGradientNoise(input.Position.xy, (float) StateFrameIndexMod8);

    [branch]
    if (SubstrateIsSubstrateMaterial(substrateData0.x))
    {
        // ============================================================
        //  Substrate Slab 経路
        // ============================================================
        uint4 substrateData1 = SubstrateMaterial1.Load(int3(input.Position.xy, 0));
        FSubstrateBSDF SlabBSDF = SubstrateUnpackSlabData(substrateData0, substrateData1);

        // G-Buffer 側が持つ値で上書き (パック規約)
        SlabBSDF.Normal = normal;
        SlabBSDF.DiffuseAlbedo = baseColor.rgb;
        SlabBSDF.Roughness = max(roughness, 0.001f);

        float3 directLighting = GetForwardDirectLightingSubstrate(
            GridIndex, worldPos.xyz, cameraVector, SlabBSDF, normal, viewDepth,
            directionalShadow, contactShadowDither, false);

        // ---- IBL + エミッシブ ----
        // IBL はスカイ可視率で減衰 (Lumen GI との二重計上防止 + DFAO)。
        // スペキュラは Lumen Reflections で prefiltered を差し替える。
        float3 iblAmbient = SubstrateEnvLightingWithReflection(
            SlabBSDF, normal, viewDir, occlusion,
            lumenReflection.rgb, lumenReflection.a) * skyOcclusion;

        sceneLighting = directLighting + iblAmbient + SlabBSDF.Emissive;
        lumenDiffuseAlbedo = SlabBSDF.DiffuseAlbedo;
    }
    else
    {
        // ============================================================
        //  レガシー Metallic/Specular 経路
        // ============================================================
        FGBufferData GBuffer = MakeGBufferData(normal, baseColor.rgb, metallic, msra.g, roughness, occlusion, viewDepth);

        // タイルドライトカリング: LightGridInjection_CS / LightGridCompact_CS
        // が毎フレーム構築したライトグリッド (t19/t20) から、このピクセルの
        // 属するセルのライトだけを巡回する (LightGridCommon.hlsl)。
        float3 directLighting = GetForwardDirectLighting(
            GridIndex, worldPos.xyz, cameraVector, GBuffer,
            directionalShadow, contactShadowDither, false);

        // --- IBL Ambient (diffuse irradiance + specular split-sum) ---
        // IBL はスカイ可視率で減衰 (Lumen GI との二重計上防止 + DFAO)。
        // スペキュラは Lumen Reflections で prefiltered を差し替える。
        float3 iblAmbient = IBL_AmbientWithReflection(
            baseColor.rgb,
            metallic, roughness, occlusion,
            normal, viewDir,
            lumenReflection.rgb, lumenReflection.a) * skyOcclusion;

        sceneLighting = directLighting + iblAmbient;
        lumenDiffuseAlbedo = baseColor.rgb * (1.0f - metallic);
    }

    // ---- Lumen スクリーン GI の拡散合成 ----
    // コサイン重点サンプルの拡散反射: Lo = albedo * mean(L_i)
    // (Surface Cache 経由なのでエミッシブ面 / 多バウンス光を含む)
    [branch]
    if (LumenGatherMode != 0u)
    {
        sceneLighting += lumenRadiance * lumenDiffuseAlbedo * LumenGIIntensity;
    }

    output.Color = float4(sceneLighting, 1.0f);

    // ---- Lumen デバッグ可視化 (ImGui: Lumen ウィンドウ) ----
    //   1 = GI 入射ラディアンスのみ / 2 = スカイ可視率 / 3 = GI 拡散寄与のみ
    //   4 = Short Range AO (Integrate_CS がデバッグ用に rgb へ AO を書く)
    [branch]
    if (LumenGatherMode != 0u && LumenDebugMode != 0u)
    {
        if (LumenDebugMode == 1u || LumenDebugMode == 4u)
        {
            // 4 (Short Range AO): Integrate_CS が rgb に AO を書いている
            // (GatherMode 2 のみ。ピクセル毎経路では GI ラディアンスのまま)
            output.Color.rgb = lumenRadiance;
        }
        else if (LumenDebugMode == 2u)
        {
            output.Color.rgb = lumenSkyVisibility.xxx;
        }
        else
        {
            output.Color.rgb = lumenRadiance * lumenDiffuseAlbedo * LumenGIIntensity;
        }
    }

    // ---- ライトグリッドデバッグ可視化 (ImGui: Light Grid ウィンドウ) ----
    //   1 = セルのライト数ヒートマップ (緑 -> 黄 -> 赤)
    //   2 = Z スライス可視化
    [branch]
    if (ForwardLightData.LightGridDebugMode != 0u)
    {
        FCulledLightsGridHeader DebugHeader = GetCulledLightsGridHeader(GridIndex);
        float3 debugColor = (ForwardLightData.LightGridDebugMode == 1u)
            ? GetLightGridComplexityColor(DebugHeader.NumLights)
            : GetLightGridZSliceColor(GridCoordinate.z);

        // 64px タイル境界に細線を引いてセルを見やすくする
        uint2 pixelInTile = uint2(input.Position.xy) & ((1u << ForwardLightData.LightGridPixelSizeShift) - 1u);
        float border = (pixelInTile.x == 0u || pixelInTile.y == 0u) ? 0.35f : 1.0f;

        output.Color.rgb = lerp(output.Color.rgb, debugColor * border, 0.6f);
    }

    return output;
}
