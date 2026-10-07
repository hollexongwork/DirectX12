#ifndef DYNAMIC_LIGHTING_COMMON_HLSL
#define DYNAMIC_LIGHTING_COMMON_HLSL

#include "BRDF.hlsl"

// =============================================================
//  DynamicLightingCommon
//  ローカルライトの減衰関数。
//  レジスタを宣言しない (グラフィックス / コンピュート共用)。
// =============================================================

// -------------------------------------------------------------
//  半径で正規化した距離のマスク (RadialAttenuationMask)
//    WorldLightVector = (受光点 -> ライト) * InvRadius
// -------------------------------------------------------------
float RadialAttenuationMask(float3 WorldLightVector)
{
    float NormalizeDistanceSquared = dot(WorldLightVector, WorldLightVector);
    return 1.0f - saturate(NormalizeDistanceSquared);
}

// -------------------------------------------------------------
//  指数フォールオフ (RadialAttenuation)
//  bUseInverseSquaredFalloff = false のとき距離応答の全体を担う。
// -------------------------------------------------------------
float RadialAttenuation(float3 WorldLightVector, float FalloffExponent)
{
    return pow(RadialAttenuationMask(WorldLightVector), FalloffExponent);
}

// -------------------------------------------------------------
//  スポットコーンのマスク (SpotAttenuationMask)
//    L             : 受光点 -> ライト方向 (正規化)
//    SpotDirection : スポットライトの発光方向
//    SpotAngles    : x = cos(Outer), y = 1 / (cos(Inner) - cos(Outer))
// -------------------------------------------------------------
float SpotAttenuationMask(float3 L, float3 SpotDirection, float2 SpotAngles)
{
    return saturate((dot(L, -SpotDirection) - SpotAngles.x) * SpotAngles.y);
}

// スポットコーンの減衰 (SpotAttenuation)
float SpotAttenuation(float3 L, float3 SpotDirection, float2 SpotAngles)
{
    float ConeAngleFalloff = Square(SpotAttenuationMask(L, SpotDirection, SpotAngles));
    return ConeAngleFalloff;
}

#endif
