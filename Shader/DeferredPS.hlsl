#include "PBR_Utility.hlsl"
#include "DeferredLightingCommon.hlsl"
#include "SubstrateEvaluation.hlsl"
#include "ShadowFilteringCommon.hlsl"
#include "LightGridCommon.hlsl"

// =============================================================
//  DeferredPS
//  ディファードライティングパス。G-Buffer と Substrate バッファ
//  (t22/t23) を読み、ピクセルごとに 2 経路へ分岐する:
//    - SubstrateMaterial0.x != 0 : Substrate Slab BSDF
//      (SubstrateEvaluation.hlsl。SSS / F0-F90 / 第2ローブ / ファズ)
//    - それ以外                  : レガシー Metallic/Specular
//      (CookTorrance + IBL_Ambient)
//  ローカルライトの減衰 / MRP セットアップ (FAreaLight) は両経路で
//  共有 (DeferredLightingCommon.hlsl)。
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
    float3 lightDir = normalize(DirectionalLightDirection.xyz);

    // ---- ディレクショナルシャドウ (CSM) ----
    // カスケード選択はカメラビュー空間の深度で行う
    float viewDepth = mul(float4(worldPos.xyz, 1.0f), View).z;
    float directionalShadow = GetDirectionalShadow(worldPos.xyz, normal, viewDepth);

    // ---- Substrate ピクセル判定 (RT3.x = ヘッダ, 0 = レガシー) ----
    uint4 substrateData0 = SubstrateMaterial0.Load(int3(input.Position.xy, 0));

    // ---- ライトグリッドセル (両経路共通) ----
    uint3 GridCoordinate = ComputeLightGridCellCoordinate(uint2(input.Position.xy), viewDepth);
    uint GridIndex = ComputeLightGridCellIndex(GridCoordinate);

    float3 sceneLighting;

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

        // ---- ディレクショナルライト ----
        float3 light = SubstrateEvaluateSlabDirect(
            SlabBSDF, normal, viewDir,
            lightDir, lightDir,
            0.0f, 0.0f, 1.0f, 1.0f, 1.0f)
            * DirectionalLightColor.rgb * directionalShadow;

        // ---- ローカルライト (Point / Spot / Rect) ----
        float3 localLight = float3(0.0f, 0.0f, 0.0f);

        [branch]
        if (bUseLightGrid != 0u)
        {
            FCulledLightsGridHeader GridHeader = GetCulledLightsGridHeader(GridIndex);

            for (uint i = 0; i < GridHeader.NumLights; ++i)
            {
                uint LightIndex = GetCulledLightDataGrid(GridHeader.DataStartIndex + i);
                FLightShaderParameters localLightParams = ForwardLocalLights[LightIndex];

                float localShadow = GetLocalLightShadow(
                    localLightParams, LocalShadowParams[LightIndex], worldPos.xyz, normal);

                localLight += IntegrateLocalLightSubstrate(
                    localLightParams,
                    worldPos.xyz, normal, viewDir,
                    SlabBSDF) * localShadow;
            }
        }
        else
        {
            for (uint i = 0; i < NumLocalLights; ++i)
            {
                FLightShaderParameters localLightParams = ForwardLocalLights[i];

                float localShadow = GetLocalLightShadow(
                    localLightParams, LocalShadowParams[i], worldPos.xyz, normal);

                localLight += IntegrateLocalLightSubstrate(
                    localLightParams,
                    worldPos.xyz, normal, viewDir,
                    SlabBSDF) * localShadow;
            }
        }

        // ---- IBL + エミッシブ ----
        float3 iblAmbient = SubstrateEnvLighting(SlabBSDF, normal, viewDir, occlusion);

        sceneLighting = light + localLight + iblAmbient + SlabBSDF.Emissive;
    }
    else
    {
        // ============================================================
        //  レガシー Metallic/Specular 経路 (従来と同一)
        // ============================================================
        float3 light = CookTorrance(
            normal, lightDir, viewDir,
            baseColor.rgb, roughness, metallic,
            DirectionalLightColor.rgb) * directionalShadow;

        // ---- ローカルライト (Point / Spot / Rect) ----
        // タイルドライトカリング: LightGridInjection_CS / LightGridCompact_CS
        // が毎フレーム構築したライトグリッド (t19/t20) から、このピクセルの
        // 属するセルのライトだけを巡回する (LightGridCommon.hlsl)。
        // グリッドが持つインデックスはライトバッファ (t13) / ローカル
        // シャドウパラメータ (t16) と共通なので 1:1 対応はそのまま。
        float3 localLight = float3(0.0f, 0.0f, 0.0f);

        [branch]
        if (bUseLightGrid != 0u)
        {
            FCulledLightsGridHeader GridHeader = GetCulledLightsGridHeader(GridIndex);

            for (uint i = 0; i < GridHeader.NumLights; ++i)
            {
                uint LightIndex = GetCulledLightDataGrid(GridHeader.DataStartIndex + i);
                FLightShaderParameters localLightParams = ForwardLocalLights[LightIndex];

                // ローカルシャドウ (t16 はライトバッファ t13 と同じインデックスで 1:1)
                float localShadow = GetLocalLightShadow(
                    localLightParams, LocalShadowParams[LightIndex], worldPos.xyz, normal);

                localLight += IntegrateLocalLight(
                    localLightParams,
                    worldPos.xyz, normal, viewDir,
                    baseColor.rgb, roughness, metallic) * localShadow;
            }
        }
        else
        {
            // フォールバック: 全灯ループ (グリッド無効時の従来経路)。
            // 有効数は b3 (ForwardLightData) の NumLocalLights。
            for (uint i = 0; i < NumLocalLights; ++i)
            {
                FLightShaderParameters localLightParams = ForwardLocalLights[i];

                float localShadow = GetLocalLightShadow(
                    localLightParams, LocalShadowParams[i], worldPos.xyz, normal);

                localLight += IntegrateLocalLight(
                    localLightParams,
                    worldPos.xyz, normal, viewDir,
                    baseColor.rgb, roughness, metallic) * localShadow;
            }
        }

        // --- IBL Ambient (diffuse irradiance + specular split-sum) ---
        float3 iblAmbient = IBL_Ambient(
            baseColor.rgb,
            metallic, roughness, occlusion,
            normal, viewDir);

        sceneLighting = light + localLight + iblAmbient;
    }

    output.Color = float4(sceneLighting, 1.0f);

    // ---- ライトグリッドデバッグ可視化 (ImGui: Light Grid ウィンドウ) ----
    //   1 = セルのライト数ヒートマップ (緑 -> 黄 -> 赤)
    //   2 = Z スライス可視化
    [branch]
    if (LightGridDebugMode != 0u)
    {
        FCulledLightsGridHeader DebugHeader = GetCulledLightsGridHeader(GridIndex);
        float3 debugColor = (LightGridDebugMode == 1u)
            ? GetLightGridComplexityColor(DebugHeader.NumLights)
            : GetLightGridZSliceColor(GridCoordinate.z);

        // 64px タイル境界に細線を引いてセルを見やすくする
        uint2 pixelInTile = uint2(input.Position.xy) & ((1u << LightGridPixelSizeShift) - 1u);
        float border = (pixelInTile.x == 0u || pixelInTile.y == 0u) ? 0.35f : 1.0f;

        output.Color.rgb = lerp(output.Color.rgb, debugColor * border, 0.6f);
    }

    return output;
}
