#include "PostProcess_Utility.hlsl"

// bloom bright-pass with a soft knee. Input: HDR post-process input (t0), read by UV.
// b4 SceneTexelSize = 1 / mip0 size (the chain is fixed at O/2). The 4-tap box at a quarter mip0 texel
// prefilters the input: P = O -> exact 2x2 box (same as one bilinear tap at the texel corner),
// P = 2*O -> each tap is a bilinear 2x2, together the full 4x4 footprint; P < O -> plain magnifying read.
PS_OUTPUT main(PS_INPUT input)
{
    PS_OUTPUT output;
    float2 t = 0.25f * float2(PostProcess.SceneTexelSizeX, PostProcess.SceneTexelSizeY);
    float3 c = 0.25f * ( TextureBaseColor.Sample(Sampler2, input.TexCoord + t * float2(-1.0f, -1.0f)).rgb
                       + TextureBaseColor.Sample(Sampler2, input.TexCoord + t * float2( 1.0f, -1.0f)).rgb
                       + TextureBaseColor.Sample(Sampler2, input.TexCoord + t * float2(-1.0f,  1.0f)).rgb
                       + TextureBaseColor.Sample(Sampler2, input.TexCoord + t * float2( 1.0f,  1.0f)).rgb );
    c *= PostProcess.Exposure;

    float br = max(c.r, max(c.g, c.b));
    float knee = PostProcess.BloomThreshold * 0.5f + 1e-4f;
    float soft = clamp(br - PostProcess.BloomThreshold + knee, 0.0f, 2.0f * knee);
    soft = soft * soft / (4.0f * knee + 1e-5f);
    float contrib = max(soft, br - PostProcess.BloomThreshold) / max(br, 1e-5f);

    output.Color = float4(c * contrib, 1.0f);
    return output;
}
