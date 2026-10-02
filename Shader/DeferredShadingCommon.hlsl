#ifndef DEFERRED_SHADING_COMMON_HLSL
#define DEFERRED_SHADING_COMMON_HLSL

#include "PBR_Utility.hlsl"

// =============================================================
//  DeferredShadingCommon
//  DeferredShadingCommon.ush の FGBufferData 相当。
//  ライト 1 灯の評価 (GetDynamicLighting -> IntegrateBxDF) が受け取る
//  サーフェスのパラメータ。デファードは G-Buffer から、フォワード
//  (半透明) はマテリアルから同じ構造体を組み立てる。
//
//  SpecularColor (F0) は既存のマテリアルモデルのまま
//  lerp(0.04, BaseColor, Metallic) で、Specular 入力は使わない [PORT]。
// =============================================================

struct FGBufferData
{
    float3 WorldNormal;
    float3 BaseColor;
    float Metallic;
    float Specular; // G-Buffer の Specular (ライティングでは未使用)
    float Roughness;
    float GBufferAO;
    float3 DiffuseColor; // BaseColor * (1 - Metallic)
    float3 SpecularColor; // F0
    float Depth; // ビュー空間 Z [m]
};

FGBufferData MakeGBufferData(float3 WorldNormal, float3 BaseColor, float Metallic, float Specular, float Roughness, float AmbientOcclusion, float ViewDepth)
{
    FGBufferData GBuffer;
    GBuffer.WorldNormal = WorldNormal;
    GBuffer.BaseColor = BaseColor;
    GBuffer.Metallic = Metallic;
    GBuffer.Specular = Specular;
    GBuffer.Roughness = Roughness;
    GBuffer.GBufferAO = AmbientOcclusion;
    GBuffer.DiffuseColor = BaseColor * (1.0f - Metallic);
    GBuffer.SpecularColor = lerp(DIELECTRIC_F0, BaseColor, Metallic);
    GBuffer.Depth = ViewDepth;
    return GBuffer;
}

#endif
