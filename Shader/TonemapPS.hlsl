#include "PostProcess_Utility.hlsl"

// =============================================================
//  PostProcess Tonemapper (final composite pass).
//
//  Order :
//    1. Chromatic aberration (lateral, scales toward edges)
//    2. + Bloom (additive, intensity-weighted)
//    3. * Exposure (AutoExposureBuffer[0] when PP_FLAG_AUTO_EXPOSURE,
//       otherwise manual PostProcess.Exposure)
//    4. White balance (Temp/Tint) - only when the grading LUT is disabled;
//       the LUT baked by ColorGradingLUT_CS already contains WB
//    5. Tonemap operator (ACES Narkowicz / ACES Hill / None)
//    6. Linear -> sRGB
//    7. 3D color-grading LUT (display space, baked by ColorGradingLUT_CS)
//    8. Vignette (display space)
//    9. Film grain (display space)
//
//  Inputs: t0 = HDR SceneColor, t9 = bloom (accumulated),
//          t10 = color grading LUT (33^3), t11 = auto exposure buffer
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

    // ---- 4. White balance (LUT 無効時のみ。LUT は WB をベイク済み) ----
    if ((flags & PP_FLAG_WHITE_BALANCE) && !(flags & PP_FLAG_COLOR_GRADING))
    {
        hdr *= WhiteBalanceScale(PostProcess.WhiteTemp, PostProcess.WhiteTint);
    }
    
    // ---- 5. Tonemap ----
    float3 color = ApplyTonemap(hdr, PostProcess.TonemapperMode);

    // ---- 6/7. sRGB encode + Color grading LUT (display space) ----
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

    // ---- 8. Vignette (display space) ----
    if (flags & PP_FLAG_VIGNETTE)
    {
        float2 c = uv - 0.5f;
        float v = smoothstep(0.8f, 0.2f, length(c) * 1.4142f);
        sdr *= lerp(1.0f, v, saturate(PostProcess.VignetteIntensity));
    }

    // ---- 9. Film grain (display space) ----
    if (flags & PP_FLAG_GRAIN)
    {
        float n = Hash21(uv * float2(2.0f, 2.0f) + PostProcess.FilmGrainTime);
        sdr += (n - 0.5f) * PostProcess.FilmGrainIntensity;
    }

    sdr = saturate(sdr);
    output.Color = float4(sdr, 1.0f);
    return output;
}
