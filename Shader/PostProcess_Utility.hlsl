#ifndef POSTPROCESS_UTILITY_HLSL
#define POSTPROCESS_UTILITY_HLSL

#include "Common.hlsl"   // ConstantBuffers / Resources / ColorSpace を含む
#include "Grading.hlsl"  // WhiteBalanceScale / ColorGradeApply (純粋関数)

// =============================================================
//  Tonemap operators
// =============================================================

// ---- ACES Narkowicz (Mobile) ----
float3 ACES_Narkowicz(float3 x)
{
    const float a = 2.51f, b = 0.03f, c = 2.43f, d = 0.59f, e = 0.14f;
    return saturate((x * (a * x + b)) / (x * (c * x + d) + e));
}

// ---- ACES Hill (RRT+ODT フィット, Desktop) ----
static const float3x3 ACESInputMat = float3x3(
     0.59719f, 0.35458f, 0.04823f,
     0.07600f, 0.90834f, 0.01566f,
     0.02840f, 0.13383f, 0.83777f);
static const float3x3 ACESOutputMat = float3x3(
     1.60475f, -0.53108f, -0.07367f,
    -0.10208f, 1.10813f, -0.00605f,
    -0.00327f, -0.07276f, 1.07602f);

float3 RRTAndODTFit(float3 v)
{
    float3 a = v * (v + 0.0245786f) - 0.000090537f;
    float3 b = v * (0.983729f * v + 0.4329510f) + 0.238081f;
    return a / b;
}

float3 ACES_Hill(float3 color)
{
    color = mul(ACESInputMat, color);
    color = RRTAndODTFit(color);
    color = mul(ACESOutputMat, color);
    return saturate(color);
}

// モード分岐: 0=Narkowicz, 1=Hill, それ以外=クランプのみ
float3 ApplyTonemap(float3 hdr, uint mode)
{
    if (mode == 0u)
        return ACES_Narkowicz(hdr);
    if (mode == 1u)
        return ACES_Hill(hdr);
    return saturate(hdr); // None
}

float3 ColorGrade(float3 color)
{
    return ColorGradeApply(
        color,
        PostProcess.ColorSaturation,
        PostProcess.ColorContrast,
        PostProcess.ColorGamma,
        PostProcess.ColorGain,
        PostProcess.ColorOffset);
}

// =============================================================
//  Helpers
// =============================================================

// フィルムグレイン用ハッシュ
float Hash21(float2 p)
{
    p = frac(p * float2(123.34f, 456.21f));
    p += dot(p, p + 45.32f);
    return frac(p.x * p.y);
}

// 3D カラーグレーディング LUT サンプル (33^3, 半テクセルオフセット補正)
float3 SampleColorGradingLUT(float3 color)
{
    const float lutSize = 33.0f;
    float3 c = saturate(color);
    float3 uvw = c * ((lutSize - 1.0f) / lutSize) + (0.5f / lutSize);
    return ColorGradingLUT.SampleLevel(Sampler2, uvw, 0).rgb;
}

#endif // POSTPROCESS_UTILITY_HLSL
