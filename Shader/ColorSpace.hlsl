#ifndef COLOR_SPACE_HLSL
#define COLOR_SPACE_HLSL

// Rec.709 ãPìxåWêî
static const float3 LUM_WEIGHT = float3(0.2126f, 0.7152f, 0.0722f);

float Luminance(float3 c)
{
    return dot(c, LUM_WEIGHT);
}

// ---- sRGB <-> linear (IEC 61966-2-1) ----
float3 LinearToSRGB(float3 c)
{
    float3 lo = c * 12.92f;
    float3 hi = 1.055f * pow(max(c, 1e-5f), 1.0f / 2.4f) - 0.055f;
    return (c <= 0.0031308f) ? lo : hi;
}

float3 SRGBToLinear(float3 c)
{
    float3 lo = c / 12.92f;
    float3 hi = pow(max((c + 0.055f) / 1.055f, 1e-5f), 2.4f);
    return (c <= 0.04045f) ? lo : hi;
}

#endif 