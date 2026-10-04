#include "VolumetricFogCommon.hlsl"

// =============================================================
//  VolumetricFogAttributes_CS
//  InitializeVolumeAttributesCS 相当 - Pass 1/3。
//  各 froxel (ジッタ付きサンプル位置) で Exponential Height Fog の
//  2 層密度を評価し、Volumetric Fog の媒質属性へ変換する:
//    σt (消散)  = (Density0(y) + Density1(y)) * ExtinctionScale
//    散乱 (rgb) = σt * Albedo
//    吸収       = σt * (1 - mean(Albedo))
//    発光 (rgb) = VolumetricFogEmissive [/m]
//  VolumetricFogStartDistance / NearFadeInDistance による
//  手前のフェードもここで密度に掛ける。
//
//  出力: RWVBufferA (u0) = (散乱 rgb, 吸収), RWVBufferB (u1) = (発光 rgb, 0)
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

    float sceneDepth;
    float3 worldPos = ComputeCellWorldPosition(gridCoordinate, FrameJitter.xyz, sceneDepth);

    // ---- 指数高さフォグの密度 (2 層)。高さ軸は Y ----
    // exp2 の引数は -127 でクランプ (発散防止)
    float exponent0 = max(-127.0f, FogDensityParams0.y * (worldPos.y - FogDensityParams0.z));
    float exponent1 = max(-127.0f, FogDensityParams1.y * (worldPos.y - FogDensityParams1.z));
    float globalDensityFirst = FogDensityParams0.x * exp2(-exponent0);
    float globalDensitySecond = FogDensityParams1.x * exp2(-exponent1);
    float globalDensity = (globalDensityFirst + globalDensitySecond) * FogDensityParams0.w;

    // ---- 手前のフェード (StartDistance / NearFadeInDistance) ----
    const float startDistance = FogAlbedo.w;
    const float nearFadeIn = FogEmissive.w;
    float nearFade = (nearFadeIn > 0.0f)
        ? saturate((sceneDepth - startDistance) / nearFadeIn)
        : ((sceneDepth >= startDistance) ? 1.0f : 0.0f);
    globalDensity *= nearFade;

    // ---- 媒質属性 ----
    float3 scattering = globalDensity * FogAlbedo.rgb;
    float absorption = globalDensity * (1.0f - dot(FogAlbedo.rgb, float3(1.0f, 1.0f, 1.0f) / 3.0f));

    RWVBufferA[gridCoordinate] = float4(scattering, absorption);
    RWVBufferB[gridCoordinate] = float4(FogEmissive.rgb * nearFade, 0.0f);
}
