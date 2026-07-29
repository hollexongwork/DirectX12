#ifndef PBR_UTILITY_HLSL
#define PBR_UTILITY_HLSL

#include "Common.hlsl"

// 誘電体の既定 F0 (4%)
static const float3 DIELECTRIC_F0 = float3(0.04f, 0.04f, 0.04f);

// =============================================================
//  Direct lighting BRDF (Cook-Torrance)
// =============================================================

// GGX 法線分布関数
float GGX_NDF(float NdotH, float roughness)
{
    float a = roughness * roughness;
    float a2 = a * a;
    float d = NdotH * NdotH * (a2 - 1.0f) + 1.0f;
    return a2 / max(PI * d * d, EPSILON);
}

// Schlick フレネル
float3 SchlickFresnel(float3 F0, float VdotH)
{
    float f = pow(1.0f - max(VdotH, 0.0f), 5.0f);
    return F0 + (1.0f - F0) * f;
}

// Smith-GGX ジオメトリ (Schlick-Beckmann, 直接光 k)
float SchlickGGX(float NdotV, float roughness)
{
    float r = roughness + 1.0f;
    float k = (r * r) / 8.0f;
    return NdotV / (NdotV * (1.0f - k) + k + EPSILON);
}

float SmithGeometry(float NdotV, float NdotL, float roughness)
{
    return SchlickGGX(max(NdotV, 0.0f), roughness)
         * SchlickGGX(max(NdotL, 0.0f), roughness);
}

// Cook-Torrance BRDF (単一方向光). 戻り値は radiance 寄与。
float3 CookTorrance(
    float3 Normal, float3 LightDir, float3 ViewDir,
    float3 albedo, float roughness, float metallic,
    float3 lightColor)
{
    float3 HalfVec = normalize(LightDir + ViewDir);

    float NdotL = max(dot(Normal, LightDir), 0.0f);
    float NdotV = max(dot(Normal, ViewDir), 1e-5f);
    float NdotH = max(dot(Normal, HalfVec), 0.0f);
    float VdotH = max(dot(ViewDir, HalfVec), 0.0f);

    // F0: 誘電体=4%, 金属=アルベド
    float3 F0 = lerp(DIELECTRIC_F0, albedo, metallic);

    float D = GGX_NDF(NdotH, roughness);
    float G = SmithGeometry(NdotV, NdotL, roughness);
    float3 F = SchlickFresnel(F0, VdotH);

    // 鏡面反射
    float3 specular = (D * G * F) / max(4.0f * NdotV * NdotL, EPSILON);

    // 拡散反射 (Lambert, 金属=0)
    float3 kD = (1.0f - F) * (1.0f - metallic);
    float3 diffuse = kD * albedo * INV_PI;

    return (diffuse + specular) * lightColor * NdotL;
}

// =============================================================
//  IBL (Image-Based Lighting)
//  起動時に IBLBaker が以下を一度きりベイク:
//    IrradianceCube : 拡散 irradiance (畳み込み済キューブ)
//    PrefilterCube  : roughness 別 specular (ミップ畳み込み済キューブ)
//    BRDFLut        : (scale, bias) 統合 LUT
//  ランタイムは Irradiance 1 + Prefilter 1 + LUT 1 = 3 サンプルのみ。
// =============================================================

// Prefilter キューブの最大ミップ (C++ 側 PREFILTER_MIP_COUNT と一致必須)
static const float PREFILTER_MAX_MIP = 4.0f; // mip 0..4 (5段)

// Fresnel (roughness 減衰付き, Lagarde)
float3 FresnelSchlickRoughness(float cosTheta, float3 F0, float roughness)
{
    float3 maxF = max((1.0f - roughness).xxx, F0);
    return F0 + (maxF - F0) * pow(clamp(1.0f - cosTheta, 0.0f, 1.0f), 5.0f);
}

// 完全な IBL アンビエント項 (3 タップ)
float3 IBL_Ambient(
    float3 albedo,
    float metallic,
    float roughness,
    float occlusion,
    float3 N,
    float3 V)
{
    float NdotV = max(dot(N, V), 1e-4f);
    float3 R = reflect(-V, N);

    float3 F0 = lerp(DIELECTRIC_F0, albedo, metallic);
    float3 F = FresnelSchlickRoughness(NdotV, F0, roughness);

    float3 kS = F;
    float3 kD = (1.0f - kS) * (1.0f - metallic);

    // --- Diffuse (1 tap) ---
    float3 irradiance = IrradianceCube.Sample(Sampler2, N).rgb;
    float3 diffuse = irradiance * albedo;

    // --- Specular split-sum (2 taps) ---
    float mip = roughness * PREFILTER_MAX_MIP;
    float3 prefiltered = PrefilterCube.SampleLevel(Sampler2, R, mip).rgb;
    float2 envBRDF = BRDFLut.Sample(Sampler2, float2(NdotV, roughness)).rg;
    float3 specular = prefiltered * (F * envBRDF.x + envBRDF.y);

    return (kD * diffuse + specular) * occlusion;
}

#endif
