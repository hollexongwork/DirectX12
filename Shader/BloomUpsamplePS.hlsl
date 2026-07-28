#include "PostProcess_Utility.hlsl"

// 3x3 tent upsample of t0 (lower-res blurred mip), added to t10
// (higher-res mip already accumulated). SceneTexelSize = source mip texel.
PS_OUTPUT main(PS_INPUT input)
{
    PS_OUTPUT output;
    float2 uv = input.TexCoord;
    float2 t = float2(PostProcess.SceneTexelSizeX, PostProcess.SceneTexelSizeY);

    float3 s;
    s  = TextureBaseColor.Sample(Sampler2, uv + t * float2(-1,-1)).rgb * 1.0f;
    s += TextureBaseColor.Sample(Sampler2, uv + t * float2( 0,-1)).rgb * 2.0f;
    s += TextureBaseColor.Sample(Sampler2, uv + t * float2( 1,-1)).rgb * 1.0f;
    s += TextureBaseColor.Sample(Sampler2, uv + t * float2(-1, 0)).rgb * 2.0f;
    s += TextureBaseColor.Sample(Sampler2, uv).rgb                     * 4.0f;
    s += TextureBaseColor.Sample(Sampler2, uv + t * float2( 1, 0)).rgb * 2.0f;
    s += TextureBaseColor.Sample(Sampler2, uv + t * float2(-1, 1)).rgb * 1.0f;
    s += TextureBaseColor.Sample(Sampler2, uv + t * float2( 0, 1)).rgb * 2.0f;
    s += TextureBaseColor.Sample(Sampler2, uv + t * float2( 1, 1)).rgb * 1.0f;
    s *= (1.0f / 16.0f);

    float3 hi = TextureBloom.Sample(Sampler2, uv).rgb; // higher-res accumulator
    output.Color = float4(hi + s, 1.0f);
    return output;
}
