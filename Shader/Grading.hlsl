#ifndef GRADING_HLSL
#define GRADING_HLSL

#include "ColorSpace.hlsl" 

static const float GRADING_MID_GREY = 0.18f;
static const float WB_TINT_SCALE = 0.05f;

float3 WhiteBalanceScale(float temp, float tint)
{
    float t = clamp(temp, 1500.0f, 15000.0f);
    float u = (0.860117757f + 1.54118254e-4f * t + 1.28641212e-7f * t * t)
            / (1.0f + 8.42420235e-4f * t + 7.08145163e-7f * t * t);
    float v = (0.317398726f + 4.22806245e-5f * t + 4.20481691e-8f * t * t)
            / (1.0f - 2.89741816e-5f * t + 1.61456053e-7f * t * t);
    float x = 3.0f * u / (2.0f * u - 8.0f * v + 4.0f);
    float y = 2.0f * v / (2.0f * u - 8.0f * v + 4.0f);
    y += tint * WB_TINT_SCALE;

    float Y = 1.0f;
    float X = Y / max(y, 1e-4f) * x;
    float Z = Y / max(y, 1e-4f) * (1.0f - x - y);


    float3 rgb = float3(
         3.2404542f * X - 1.5371385f * Y - 0.4985314f * Z,
        -0.9692660f * X + 1.8760108f * Y + 0.0415560f * Z,
         0.0556434f * X - 0.2040259f * Y + 1.0572252f * Z);
    rgb = max(rgb, 1e-3f);

    
    float3 ref = float3(0.9505f, 1.0000f, 1.0890f);
    float3 refRGB = float3(
         3.2404542f * ref.x - 1.5371385f * ref.y - 0.4985314f * ref.z,
        -0.9692660f * ref.x + 1.8760108f * ref.y + 0.0415560f * ref.z,
         0.0556434f * ref.x - 0.2040259f * ref.y + 1.0572252f * ref.z);

    return refRGB / rgb;
}


float3 ColorGradeApply(
    float3 color,
    float4 saturation,
    float4 contrast,
    float4 gamma,
    float4 gain,
    float4 offset)
{
    // Saturation
    float3 sat = saturation.rgb * saturation.a;
    float grey = Luminance(color);
    color = lerp(grey.xxx, color, sat);

    // Contrast
    float3 con = contrast.rgb * contrast.a;
    color = pow(max(color / GRADING_MID_GREY, 1e-5f), con) * GRADING_MID_GREY;

    // Gain
    float3 g = gain.rgb * gain.a;
    color *= g;

    // Offset
    color += offset.rgb + (offset.a - 1.0f);

    // Gamma
    float3 gm = gamma.rgb * gamma.a;
    color = pow(max(color, 1e-5f), 1.0f / max(gm, 1e-3f));

    return max(color, 0.0f);
}

#endif 
