#include "VolumetricFogCommon.hlsl"

// =============================================================
//  VolumetricFogIntegration_CS
//  FinalIntegrationCS 相当 - Pass 3/3。
//  XY 列ごとに 1 スレッドを割り当て、Z スライスを手前から奥へ
//  積分してカメラから各 froxel までの
//    rgb = 累積インスキャッタ (透過率で重み付け),
//    a   = 累積透過率
//  を書き出す (IntegratedLightScattering, グラフィックス側 t34)。
//
//  スライスごとの積分は "Physically Based and Unified Volumetric
//  Rendering in Frostbite" のエネルギー保存形:
//    S_int = (S - S * T) / σt,  T = exp(-σt * Δs)
//  (σt ≒ 0 のときは S * Δs にフォールバック。発光のみの媒質で
//   0 除算により光が消えるのを防ぐ)
//
//  Dispatch: (ceil(GridSizeX / 8), ceil(GridSizeY / 8), 1)
// =============================================================

[numthreads(VOLUMETRIC_FOG_INTEGRATION_GROUP_SIZE, VOLUMETRIC_FOG_INTEGRATION_GROUP_SIZE, 1)]
void main(uint3 DispatchThreadId : SV_DispatchThreadID)
{
    const uint2 gridCoordinate = DispatchThreadId.xy;
    if (any(gridCoordinate >= (uint2) GridSize.xy))
    {
        return;
    }

    const uint gridSizeZ = (uint) GridSize.z;

    float3 accumulatedLighting = float3(0.0f, 0.0f, 0.0f);
    float accumulatedTransmittance = 1.0f;
    float3 previousSliceWorldPosition = CameraOrigin.xyz;

    [loop]
    for (uint layerIndex = 0; layerIndex < gridSizeZ; ++layerIndex)
    {
        uint3 layerCoordinate = uint3(gridCoordinate, layerIndex);
        float4 scatteringAndExtinction = LightScatteringTexture[layerCoordinate];

        // セル中心 (ジッタなし) 間の距離をステップ長にする
        float layerDepth;
        float3 layerWorldPosition = ComputeCellWorldPosition(layerCoordinate, float3(0.5f, 0.5f, 0.5f), layerDepth);
        float stepLength = length(layerWorldPosition - previousSliceWorldPosition);
        previousSliceWorldPosition = layerWorldPosition;

        float extinction = max(scatteringAndExtinction.w, 0.0f);
        float transmittance = exp(-extinction * stepLength);

        // エネルギー保存積分 (Frostbite)。σt ≒ 0 は S * Δs
        float3 scatteringIntegratedOverSlice = (extinction > 0.00001f)
            ? (scatteringAndExtinction.rgb - scatteringAndExtinction.rgb * transmittance) / extinction
            : scatteringAndExtinction.rgb * stepLength;

        accumulatedLighting += scatteringIntegratedOverSlice * accumulatedTransmittance;
        accumulatedTransmittance *= transmittance;

        RWIntegratedLightScattering[layerCoordinate] = float4(accumulatedLighting, accumulatedTransmittance);
    }
}
