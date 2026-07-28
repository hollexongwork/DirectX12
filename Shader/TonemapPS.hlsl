#include "PostProcess_Utility.hlsl"

// =============================================================
//  PostProcess Tonemapper (final composite pass).
//
//  Order :
//    1. Chromatic aberration (lateral, scales toward edges)
//    2. + Bloom (additive, intensity-weighted)
//    3. * Exposure
//    4. White balance (Temp/Tint)
//    5. Tonemap operator (ACES Narkowicz / ACES Hill / None)
//    6. Color grading (Sat / Contrast / Gamma / Gain / Offset)
//    7. Vignette
//    8. Film grain
//    9. Linear -> sRGB
//
//  Inputs: t0 = HDR SceneColor, t9 = bloom (full-res accumulated)
// =============================================================

PS_OUTPUT main(PS_INPUT input)
{
    PS_OUTPUT output;
    float2 uv = input.TexCoord;
    uint flags = PostProcess.Flags;

    // ---- 1. Chromatic aberration (sample R/G/B with radial offset) ----
    float3 hdr;
    if (flags & PP_FLAG_CHROMATIC)
    {
        float2 center = uv - 0.5f;
        float dist = dot(center, center); // squared radius
        float2 dir = center * dist * PostProcess.ChromaticAberration * 0.1f;
        hdr.r = TextureBaseColor.Sample(Sampler2, uv - dir).r;
        hdr.g = TextureBaseColor.Sample(Sampler2, uv).g;
        hdr.b = TextureBaseColor.Sample(Sampler2, uv + dir).b;
    }
    else
    {
        hdr = TextureBaseColor.Sample(Sampler2, uv).rgb;
    }

    // ---- 2. Bloom additive ----
    if (flags & PP_FLAG_BLOOM)
    {
        float3 bloom = TextureBloom.Sample(Sampler2, uv).rgb;
        hdr += bloom * PostProcess.BloomIntensity;
    }

    // ---- 3. Exposure ----
    float exposure;
    if (flags & PP_FLAG_AUTO_EXPOSURE)
    {
        exposure = AutoExposureBuffer[0];
    }
    else
    {
        exposure = PostProcess.Exposure;
    }
    hdr *= exposure;

    // ---- 4. White balance ----
    if ((flags & PP_FLAG_WHITE_BALANCE) && !(flags & PP_FLAG_COLOR_GRADING))
    {
        hdr *= WhiteBalanceScale(PostProcess.WhiteTemp, PostProcess.WhiteTint);
    }
    
    // ---- 5. Tonemap ----
    float3 color = ApplyTonemap(hdr, PostProcess.TonemapperMode);

    // ---- 6/9. Color grading LUT + sRGB encode (color-space correct) ----
    float3 sdr;
    if (flags & PP_FLAG_COLOR_GRADING)
    {
        float3 display = LinearToSRGB(saturate(color));
        sdr = SampleColorGradingLUT(display);
    }
    else
    {
        sdr = LinearToSRGB(saturate(color));
    }

    // ---- 7. Vignette (display space) ----
    if (flags & PP_FLAG_VIGNETTE)
    {
        float2 c = uv - 0.5f;
        float v = smoothstep(0.8f, 0.2f, length(c) * 1.4142f);
        sdr *= lerp(1.0f, v, saturate(PostProcess.VignetteIntensity));
    }

    // ---- 8. Film grain (display space) ----
    if (flags & PP_FLAG_GRAIN)
    {
        float n = Hash21(uv * float2(2.0f, 2.0f) + PostProcess.FilmGrainTime);
        sdr += (n - 0.5f) * PostProcess.FilmGrainIntensity;
    }

    sdr = saturate(sdr);
    output.Color = float4(sdr, 1.0f);
    return output;
}
