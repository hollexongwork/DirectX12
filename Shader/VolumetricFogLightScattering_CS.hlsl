#include "VolumetricFogCommon.hlsl"

// =============================================================
//  VolumetricFogLightScattering_CS
//  VolumetricFog.usf の LightScatteringCS 相当 - Pass 2/3。
//  各 froxel (ジッタ付きサンプル位置) に届く光を位相関数で
//  カメラ方向へ散乱させ、媒質属性 (VBufferA/B) と合成する:
//    L_in   = Σ (ライト色 x 遮蔽 x 減衰 x HG(g, cosθ))
//           + スカイ (IBL irradiance を視線方向で採光 / π) x 強度
//    出力   = (L_in * 散乱係数 + 発光, 消散係数)
//  最後に前フレームの LightScattering を「セル中心 (ジッタなし) を
//  前フレームのクリップ空間へ再投影」して採光し、HistoryWeight で
//  指数移動平均する (テンポラル再投影)。
//
//  ライト:
//    - ディレクショナル : CSM (ShadowProjectionCommon) で遮蔽
//    - ローカル (Point/Spot/Rect) : ライトグリッド (t2/t3) で
//      froxel のセルに影響するライトだけ巡回。シャドウマップの
//      ライトはアトラス (t5) で遮蔽
//
//  Dispatch: ceil(GridSize / 4) の 3D
// =============================================================

[numthreads(VOLUMETRIC_FOG_THREADGROUP_SIZE, VOLUMETRIC_FOG_THREADGROUP_SIZE, VOLUMETRIC_FOG_THREADGROUP_SIZE)]
void main(uint3 DispatchThreadId : SV_DispatchThreadID)
{
    const uint3 gridCoordinate = DispatchThreadId;
    if (any(gridCoordinate >= (uint3) GridSize.xyz))
    {
        return;
    }

    // ---- サンプル位置 (ジッタ付き) ----
    float sceneDepth;
    float3 worldPos = ComputeCellWorldPosition(gridCoordinate, FrameJitter.xyz, sceneDepth);
    float3 cameraVector = normalize(worldPos - CameraOrigin.xyz);

    // froxel のおおよその大きさ (逆二乗ライトの距離バイアス用)
    float neighborDepth;
    float3 neighborPos = ComputeCellWorldPosition(gridCoordinate + uint3(1, 1, 1), FrameJitter.xyz, neighborDepth);
    float cellRadius = length(worldPos - neighborPos);

    const float phaseG = FogDensityParams1.w;

    float3 lightScattering = float3(0.0f, 0.0f, 0.0f);

    // ---- ディレクショナルライト (CSM 遮蔽 + HG 位相) ----
    [branch]
    if (DirectionalLightDirection.w > 0.5f)
    {
        float shadow = ComputeDirectionalLightVolumetricShadow(worldPos, sceneDepth);

        float3 directionalColor = DirectionalLightColor.rgb;
        [flatten]
        if (FogInscatteringColor.w > 0.5f)
        {
            // bOverrideLightColorsWithFogInscatteringColors:
            // 太陽光の色を DirectionalInscatteringLuminance で置き換える
            float luminance = Luminance(DirectionalLightColor.rgb);
            directionalColor = DirectionalInscatteringColor.rgb * luminance;
        }

        float phase = HenyeyGreensteinPhase(phaseG, dot(DirectionalLightDirection.xyz, cameraVector));
        lightScattering += directionalColor * (shadow * phase);
    }

    // ---- スカイ (IBL irradiance を「視線方向の空」として採光) ----
    // 等方散乱の ∫ L(ω) p(ω) dω ≒ 視線方向まわりのコサイン平均 (E(v) / π)。
    // 強度は VolumetricFogStaticLightingScatteringIntensity (静的 /
    // 環境ライティングの散乱強度) を充てる。
    [branch]
    if (DirectionalInscatteringColor.w > 0.0f)
    {
        float3 skyRadiance = SkyIrradiance.SampleLevel(LinearClampSampler, cameraVector, 0.0f).rgb / PI;
        lightScattering += skyRadiance * DirectionalInscatteringColor.w;
    }

    // ---- ローカルライト (ライトグリッドのセルを巡回) ----
    {
        // froxel 中心のスクリーンピクセル座標でライトグリッドのセルを引く
        uint2 pixelPos = (uint2) ((float2(gridCoordinate.xy) + 0.5f) * GridSize.w);

        [branch]
        if (bUseLightGrid != 0u)
        {
            uint gridIndex = VF_ComputeLightGridCellIndex(pixelPos, sceneDepth);
            uint numLights = min(NumCulledLightsGrid[gridIndex * 2u + 0u], MaxCulledLightsPerCell);
            uint dataStart = NumCulledLightsGrid[gridIndex * 2u + 1u];

            [loop]
            for (uint i = 0; i < numLights; ++i)
            {
                uint lightIndex = CulledLightDataGrid[dataStart + i];
                lightScattering += ComputeLocalLightVolumetricScattering(
                    ForwardLocalLights[lightIndex], LocalShadowParams[lightIndex],
                    worldPos, cameraVector, cellRadius, phaseG);
            }
        }
        else
        {
            [loop]
            for (uint i = 0; i < NumLocalLights; ++i)
            {
                lightScattering += ComputeLocalLightVolumetricScattering(
                    ForwardLocalLights[i], LocalShadowParams[i],
                    worldPos, cameraVector, cellRadius, phaseG);
            }
        }
    }

    // ---- 媒質属性との合成 ----
    float4 vbufferA = VBufferA[gridCoordinate];
    float4 vbufferB = VBufferB[gridCoordinate];
    float3 scattering = vbufferA.rgb;
    float absorption = vbufferA.a;
    float3 emissive = vbufferB.rgb;

    // 消散係数 = 散乱 (平均) + 吸収 (= 密度 x ExtinctionScale)
    float extinction = dot(scattering, float3(1.0f, 1.0f, 1.0f) / 3.0f) + absorption;

    float4 newScatteringAndExtinction = float4(lightScattering * scattering + emissive, extinction);

    // ---- テンポラル再投影 ----
    // セル中心 (ジッタなし) を前フレームのクリップ空間へ写し、
    // 履歴ボリュームをトライリニアで採光して指数移動平均する
    [branch]
    if (TemporalParams.x > 0.5f && TemporalParams.y > 0.5f)
    {
        float centerDepth;
        float3 centerPos = ComputeCellWorldPosition(gridCoordinate, float3(0.5f, 0.5f, 0.5f), centerDepth);
        float4 historyUV = ComputeVolumeUVFromWorld(centerPos, PrevWorldToClip);

        [branch]
        if (historyUV.w > 0.0f && all(historyUV.xyz >= 0.0f) && all(historyUV.xyz <= 1.0f))
        {
            float4 history = LightScatteringHistory.SampleLevel(LinearClampSampler, historyUV.xyz, 0.0f);
            newScatteringAndExtinction = lerp(newScatteringAndExtinction, history, FrameJitter.w);
        }
    }

    RWLightScattering[gridCoordinate] = newScatteringAndExtinction;
}
