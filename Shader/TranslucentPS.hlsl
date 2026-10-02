#include "PBR_Utility.hlsl"
#include "ForwardLightingCommon.hlsl"	// DeferredLightingCommon / SubstrateEvaluation / ShadowFilteringCommon / LightGridCommon
#include "RefractionCommon.hlsl"
#include "HeightFogCommon.hlsl"
#include "BasePassCommon.hlsl"

// =============================================================
//  TranslucentPS
//  トランスルーセンシーパス (BLEND_Translucent / BLEND_Additive) の
//  フォワードシェーディング PS。BasePassPixelShader (translucent
//  マテリアル) 相当。
//
//  デファードライティング (DeferredPS) と同一のライティング入力
//  (b0 View / b3 ForwardLightData / b5 Shadow, t6-t8 IBL,
//   t13 ライトバッファ, t14-t18 シャドウ, t19-t20 ライトグリッド,
//   t37-t38 LTC) をサーフェス位置で直接評価し、確定済み SceneColor へ
//  ハードウェアブレンドで合成する。
//  ディレクショナルライトは UE のフォワードシェーディングと同じく
//  「選択された 1 灯」(b3 の DirectionalLight*) だけを照らし、
//  bAffectTranslucentLighting が偽のライトは照らさない:
//    BLEND_Translucent : SrcAlpha / InvSrcAlpha
//    BLEND_Additive    : SrcAlpha / One
//  シェーダ本体は両モード共通 (ブレンドステートのみ異なる)。
//
//  Substrate (bUseSubstrate) のときは Slab BSDF をフォワードで
//  構築・評価する (SubstrateEvaluation.hlsl。デファードと同じ数式)。
//
//  Refraction (RefractionMethod != NONE, BLEND_Translucent のみ):
//  シーンカラーコピー (t21) を屈折オフセット付きでサンプルし、
//    rgb = Lighting + 屈折背景 x 透過色
//    a   = Opacity
//  を出力してカバレッジ合成はハードウェアブレンド
//  (SrcAlpha/InvSrcAlpha) に任せる (RefractionCommon.hlsl)。
//  ブレンド宛先はライブな SceneColor なので、先に描かれた半透明面
//  (同一メッシュの手前向き三角形や奥の半透明オブジェクト) を
//  消さない。屈折背景そのものは半透明パス開始前のシーン
//  (SceneColor 参照と同じ制約)。Additive は対象外。
//
//  Height Fog / Volumetric Fog (BasePassPixelShader の Fogging 相当):
//  サーフェス位置で HeightFogCommon.hlsl のフォグ (b7 + t33/t34) を
//  直接評価し、出力色へ合成する (ApplyTranslucencyFog):
//    BLEND_Translucent : Color * Fog.a + Fog.rgb   (α = Opacity で
//                        ブレンドされるので、透けた分の背景はフォグ
//                        パス側の結果がそのまま残る)
//    BLEND_Additive    : Color * Fog.a             (減衰のみ。加算光は
//                        インスキャッタを持ち込まない)
//  Unlit / 屈折経路にも同様に掛かる (UE のマテリアル既定
//  "Apply Fogging" = true 相当。屈折背景は既にフォグ済みの
//  SceneColor なので薄く二重に掛かるが UE も同じ挙動)。
//
//  深度は不透明結果に対するテストのみ (PSO: DepthRead、書き込みなし)。
//  α = BaseColor テクスチャ α x 頂点カラー α x Material.Opacity。
//  ※ 深度 SRV (t3) はこのパスでは DSV としてバインド中のため
//    参照しないこと。LinearDepth (t4) は別リソースで SRV のまま
//    なので参照可 (屈折の深度棄却に使用)。SceneColorCopy (t21) と
//    b4 (PostProcess: SceneTexelSize) はこのパス直前に確定する。
// =============================================================

// -------------------------------------------------------------
//  屈折面色の計算 (BLEND_Translucent + RefractionMethod != NONE)
//    SurfaceLighting  : 面のライティング (エミッシブ込み)
//    ViewTransmittance: 背景に乗る透過色 (Substrate SSS 由来 / 1)
//    RoughnessForBlur : 粗い屈折のブラー量 (レガシーは 0)
//  戻り値は「面そのものの色」= Lighting + 屈折背景 x 透過色。
//  呼び出し側が α = Opacity で出力し、カバレッジ合成 (面の外側に
//  ライブな背景を通す) はハードウェアブレンドが行う。
//  ※ かつては Coverage で古い背景コピーと lerp して α = 1 で
//    置換していたが、その方式は先に描かれた半透明面を消してしまう
//    (深度書き込みが無いため、後から描かれた奥向き三角形の置換を
//     深度テストで防げない)。
// -------------------------------------------------------------
float3 ComputeRefractedSurfaceColor(
    float3 SurfaceLighting,
    float3 ViewTransmittance,
    float RoughnessForBlur,
    float2 SvPositionXY,
    float3 WorldNormal,
    float3 WorldVertexNormal,
    float SurfaceViewDepth,
    FMaterialRefractionData RefractionData)
{
    float2 SceneTexelSize = float2(PostProcess.SceneTexelSizeX, PostProcess.SceneTexelSizeY);
    float2 SceneUV = SvPositionXY * SceneTexelSize;

    float2 OffsetUV = ComputeRefractionOffsetUV(
        Material.RefractionMethod,
        RefractionData,
        WorldNormal,
        WorldVertexNormal,
        SceneTexelSize);

    float2 RefractedUV = ResolveRefractedSceneUV(
        SceneUV, OffsetUV,
        SurfaceViewDepth,
        RefractionData.RefractionDepthBias,
        SceneTexelSize);

    float3 RefractedBackground = SampleRefractedSceneColor(
        RefractedUV, RoughnessForBlur, SceneTexelSize);

    return SurfaceLighting + RefractedBackground * ViewTransmittance;
}

// -------------------------------------------------------------
//  Lumen Radiance Cache 拡散 GI (半透明用)
//  カメラ周囲のワールドプローブ SH L1 ボリューム (t30-t32,
//  トロイダルアドレッシング) を WRAP サンプラのトライリニアで
//  採光し、法線 N の平均入射ラディアンスを返す。
//  半透明は G-Buffer に乗らずスクリーンプローブを持たないため、
//  Translucency Volume Lighting に相当する経路。
// -------------------------------------------------------------
float3 SampleLumenRadianceCacheGI(float3 WorldPos, float3 N)
{
    [branch]
    if (bLumenTranslucencyGI == 0u || LumenRadianceCacheParams1.y < 0.5f)
    {
        return float3(0.0f, 0.0f, 0.0f);
    }

    const float spacing = LumenRadianceCacheParams0.w;
    const float numProbes = LumenRadianceCacheParams1.x;
    const float3 volumeMin = LumenRadianceCacheParams0.xyz;
    const float volumeSize = spacing * numProbes;

    // ボリューム範囲外は採光しない。外周の半セル帯も棄却する
    // (WRAP トライリニアがトロイダルの反対面テクセルと補間して
    //  遠方のラディアンスが漏れるため)
    const float halfCell = 0.5f * spacing;
    float3 rel = WorldPos - volumeMin;
    if (any(rel < halfCell) || any(rel > volumeSize - halfCell))
    {
        return float3(0.0f, 0.0f, 0.0f);
    }

    // トロイダル: worldPos / (spacing * N) を WRAP サンプラで読む
    // (プローブ中心 (W + 0.5) * spacing がテクセル中心に一致する)
    float3 uvw = WorldPos / volumeSize;

    float4 shR = LumenRCSH_R.SampleLevel(Sampler, uvw, 0.0f);
    float4 shG = LumenRCSH_G.SampleLevel(Sampler, uvw, 0.0f);
    float4 shB = LumenRCSH_B.SampleLevel(Sampler, uvw, 0.0f);

    // SH L1 の半球コサイン畳み込み -> 平均入射ラディアンス
    // (LumenProbeCommon.hlsl の LumenSH1EvaluateMeanRadiance と同一式)
    float4 eval = float4(0.282095f,
        0.488603f * N.y, 0.488603f * N.z, 0.488603f * N.x);
    eval.yzw *= (2.0f / 3.0f);

    return max(float3(dot(shR, eval), dot(shG, eval), dot(shB, eval)),
        float3(0.0f, 0.0f, 0.0f));
}

// -------------------------------------------------------------
//  フォグ合成 (BasePassPixelShader の MATERIALBLENDING_* 分岐相当)
//    Color        : 面の最終色 (ライティング / エミッシブ / 屈折込み)
//    WorldPos     : 受光点 (ワールド)
//    SvPositionXY : ピクセル座標 (Volumetric Fog のボリューム UV 用)
//    ViewDepth    : ビュー空間 Z
// -------------------------------------------------------------
float3 ApplyTranslucencyFog(float3 Color, float3 WorldPos, float2 SvPositionXY, float ViewDepth)
{
    float4 Fogging = ComputeFogInscatteringAndOpacity(WorldPos, SvPositionXY, ViewDepth);

    [flatten]
    if (Material.BlendMode == BLEND_ADDITIVE)
    {
        // 加算合成はフォグの透過率で減衰するだけ (インスキャッタは加えない)
        return Color * Fogging.a;
    }

    return Color * Fogging.a + Fogging.rgb;
}

PS_OUTPUT main(PS_INPUT input, bool bIsFrontFace : SV_IsFrontFace)
{
    PS_OUTPUT output;

    // ---- BaseColor + Opacity ----
    // Automatic View Mip Bias (b0 MaterialTextureMipBias。TemporalUpscale 時のみ非 0、それ以外は 0 で Sample と同値)
    float4 baseColor = TextureBaseColor.SampleBias(Sampler, input.TexCoord, MaterialTextureMipBias) * input.Color;
    float opacity = saturate(baseColor.a * Material.Opacity);

    // 屈折を自前合成するか (BLEND_Translucent のみ。Additive は対象外)
    bool bUseRefraction = (Material.BlendMode == BLEND_TRANSLUCENT)
                       && (Material.RefractionMethod != REFRACTION_METHOD_NONE);

    // ---- ワールド座標 / ビュー深度 (両経路共通) ----
    // ワールド座標は深度再構築ではなく VS 補間値 (input.WorldPosition)。
    float3 worldPos = input.WorldPosition.xyz;
    float3 viewDir = normalize(WorldCameraOrigin.xyz - worldPos);
    float viewDepth = mul(float4(worldPos, 1.0f), View).z;

    // ---- Unlit: エミッシブのみ (ベースパスの Unlit 経路と同じ合成) ----
    if (Material.Unlit)
    {
        float3 emissive = Material.EmissionColor.rgb + baseColor.rgb * Material.BaseColor.rgb;

        [branch]
        if (bUseRefraction)
        {
            // Unlit ガラス: ライティングなしで背景屈折のみ 。透過色は 1。
            float3 vertexNormalUnlit = normalize(input.Normal.xyz);
            float3 refracted = ComputeRefractedSurfaceColor(
                emissive,
                float3(1.0f, 1.0f, 1.0f), 0.0f,
                input.Position.xy,
                vertexNormalUnlit, vertexNormalUnlit,
                viewDepth,
                GetMaterialRefraction());
            // カバレッジ合成はハードウェアブレンドに任せる
            output.Color = float4(
                ApplyTranslucencyFog(refracted, worldPos, input.Position.xy, viewDepth), opacity);
            return output;
        }

        output.Color = float4(
            ApplyTranslucencyFog(emissive, worldPos, input.Position.xy, viewDepth), opacity);
        return output;
    }

    // ---- 法線 (TBN + Two Sided 反転) ----
    float3 vertexNormal = normalize(input.Normal.xyz);

    // Two Sided: 裏面は幾何法線を反転する (TwoSidedSign 相当)
    if (Material.TwoSided && !bIsFrontFace)
    {
        vertexNormal = -vertexNormal;
    }

    float3x3 TBN = BuildTBN(vertexNormal, input.Tangent);
    float3 normalSample = TextureNormal.SampleBias(Sampler, input.TexCoord, MaterialTextureMipBias).xyz * 2.0f - 1.0f;
    float3 mappedNormal = normalize(mul(normalSample, TBN));
    float3 normal = normalize(lerp(vertexNormal, mappedNormal, Material.NormalWeight));

    // ---- ARM (GeometryPS と同じフォールバック規約) ----
    float4 ARM = TextureMSRA.SampleBias(Sampler, input.TexCoord, MaterialTextureMipBias);
    float occlusion = (ARM.r == 0.0f) ? 1.0f : ARM.r;
    float roughness = (ARM.g == 0.0f) ? Material.Roughness : ARM.g;
    float metallic = (ARM.b == 0.0f) ? Material.Metallic : ARM.b;

    // ---- ディレクショナルシャドウ (CSM + DF) ----
    // 選択されたフォワードディレクショナルライトの影
    float directionalShadow = GetDirectionalShadow(worldPos, normal, viewDepth);

    // ---- ライトグリッドセル (両経路共通) ----
    uint3 GridCoordinate = ComputeLightGridCellCoordinate(uint2(input.Position.xy), viewDepth);
    uint GridIndex = ComputeLightGridCellIndex(GridCoordinate);

    // ---- 直接光 (フォワードシェーディング) ----
    // ディレクショナルライトは選択された 1 灯、ローカルライトはライトグリッドのセル。
    // bAffectTranslucentLighting が偽のライトは照らさない (ForwardLightingCommon.hlsl)。
    // コンタクトシャドウはデファードのみなのでディザは使わない
    const float3 cameraVector = -viewDir;

    float3 surfaceLighting;
    float3 viewTransmittance = float3(1.0f, 1.0f, 1.0f); // 屈折背景に乗る透過色
    float refractionRoughness = 0.0f; // 粗い屈折のブラー量
    float3 translucentGIAlbedo = float3(0.0f, 0.0f, 0.0f); // Lumen GI 用拡散アルベド
    FMaterialRefractionData refractionData = GetMaterialRefraction();

    [branch]
    if (Material.bUseSubstrate)
    {
        // ============================================================
        //  Substrate Slab 経路 (フォワード)
        //  GeometryPS と共通のヘルパ (BasePassCommon.hlsl) で Slab を
        //  構築し、デファードと同じ数式 (SubstrateEvaluation.hlsl) で
        //  直接評価する。
        // ============================================================
        FSubstrateBSDF SlabBSDF = GetMaterialSubstrateSlabBSDF(normal, baseColor.rgb, roughness);

        float3 directLighting = GetForwardDirectLightingSubstrate(
            GridIndex, worldPos, cameraVector, SlabBSDF, normal, viewDepth,
            directionalShadow, 0.0f, true);

        // ---- IBL + エミッシブ ----
        float3 iblAmbient = SubstrateEnvLighting(SlabBSDF, normal, viewDir, occlusion);

        surfaceLighting = directLighting + iblAmbient + SlabBSDF.Emissive;
        translucentGIAlbedo = SlabBSDF.DiffuseAlbedo;

        // 屈折背景の着色 (Colored Transmittance): SSS 有効時は
        // 視線方向のスラブ透過。粗い屈折は Slab のラフネス。
        [branch]
        if (bUseRefraction && SlabBSDF.SSSType != SUBSTRATE_SSS_TYPE_NONE)
        {
            viewTransmittance = SubstrateViewTransmittance(
                SlabBSDF, max(dot(normal, viewDir), 1e-4f));
        }
        refractionRoughness = SlabBSDF.Roughness;

        // ---- Index Of Refraction From F0 ----
        // Substrate では界面を F0 が定義するため、屈折 IOR も
        // 同じ F0 から導出できる (誘電体逆変換)。手入力 IOR と
        // F0 由来のスペキュラが食い違わないのが利点。
        [branch]
        if (bUseRefraction
            && Material.RefractionMethod == REFRACTION_METHOD_INDEX_OF_REFRACTION
            && Material.bRefractionUseF0)
        {
            refractionData.Data.x = DielectricF0ToIor(F0RGBToF0(SlabBSDF.F0));
        }
    }
    else
    {
        // ============================================================
        //  レガシー Metallic/Specular 経路
        // ============================================================
        FGBufferData GBuffer = MakeGBufferData(normal, baseColor.rgb, metallic, Material.Specular, roughness, occlusion, viewDepth);

        // タイルドライトカリング (ライトグリッド) はデファードと共通。
        float3 directLighting = GetForwardDirectLighting(
            GridIndex, worldPos, cameraVector, GBuffer,
            directionalShadow, 0.0f, true);

        // --- IBL Ambient (diffuse irradiance + specular split-sum) ---
        float3 iblAmbient = IBL_Ambient(
            baseColor.rgb,
            metallic, roughness, occlusion,
            normal, viewDir);

        // Lit 経路の合成はデファードと同一 (エミッシブは Unlit 経路のみ、
        // GeometryPS / DeferredPS の挙動と一致させる)
        surfaceLighting = directLighting + iblAmbient;
        translucentGIAlbedo = baseColor.rgb * (1.0f - metallic);
    }

    // ---- Lumen Radiance Cache GI (半透明拡散) ----
    surfaceLighting += SampleLumenRadianceCacheGI(worldPos, normal)
        * translucentGIAlbedo * LumenTranslucencyGIIntensity;

    // ---- Refraction 合成 / 通常のハードウェアブレンド ----
    [branch]
    if (bUseRefraction)
    {
        // IOR は refractionData に確定済み (bRefractionUseF0 のとき
        // Substrate 分岐内で F0 から導出済み。それ以外は b2 の手入力値)。
        float3 refracted = ComputeRefractedSurfaceColor(
            surfaceLighting,
            viewTransmittance, refractionRoughness,
            input.Position.xy,
            normal, vertexNormal,
            viewDepth,
            refractionData);

        // α = Opacity: カバレッジ合成 (面の外側にライブな背景を通す)
        // は SrcAlpha/InvSrcAlpha ブレンドが行う。宛先はライブな
        // SceneColor なので、先に描かれた半透明面 (同一メッシュの
        // 手前向き三角形や奥の半透明オブジェクト) を消さない。
        output.Color = float4(
            ApplyTranslucencyFog(refracted, worldPos, input.Position.xy, viewDepth), opacity);
    }
    else
    {
        output.Color = float4(
            ApplyTranslucencyFog(surfaceLighting, worldPos, input.Position.xy, viewDepth), opacity);
    }

    return output;
}
