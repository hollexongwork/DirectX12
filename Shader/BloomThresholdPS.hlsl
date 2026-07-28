#include "PostProcess_Utility.hlsl"

// bloom bright-pass with a soft knee. Input: HDR SceneColor (t0).
PS_OUTPUT main(PS_INPUT input)
{
    PS_OUTPUT output;
    float3 c = TextureBaseColor.Sample(Sampler2, input.TexCoord).rgb;
    c *= PostProcess.Exposure;

    float br = max(c.r, max(c.g, c.b));
    float knee = PostProcess.BloomThreshold * 0.5f + 1e-4f;
    float soft = clamp(br - PostProcess.BloomThreshold + knee, 0.0f, 2.0f * knee);
    soft = soft * soft / (4.0f * knee + 1e-5f);
    float contrib = max(soft, br - PostProcess.BloomThreshold) / max(br, 1e-5f);

    output.Color = float4(c * contrib, 1.0f);
    return output;
}
