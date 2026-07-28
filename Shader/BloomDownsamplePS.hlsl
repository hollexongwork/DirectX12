#include "PostProcess_Utility.hlsl"

// Jimenez "Next Generation Post Processing" 13-tap downsample.
PS_OUTPUT main(PS_INPUT input)
{
    PS_OUTPUT output;
    float2 uv = input.TexCoord;
    float2 t = float2(PostProcess.SceneTexelSizeX, PostProcess.SceneTexelSizeY);

    float3 a = TextureBaseColor.Sample(Sampler2, uv + t * float2(-2,-2)).rgb;
    float3 b = TextureBaseColor.Sample(Sampler2, uv + t * float2( 0,-2)).rgb;
    float3 c = TextureBaseColor.Sample(Sampler2, uv + t * float2( 2,-2)).rgb;
    float3 d = TextureBaseColor.Sample(Sampler2, uv + t * float2(-2, 0)).rgb;
    float3 e = TextureBaseColor.Sample(Sampler2, uv).rgb;
    float3 f = TextureBaseColor.Sample(Sampler2, uv + t * float2( 2, 0)).rgb;
    float3 g = TextureBaseColor.Sample(Sampler2, uv + t * float2(-2, 2)).rgb;
    float3 h = TextureBaseColor.Sample(Sampler2, uv + t * float2( 0, 2)).rgb;
    float3 i = TextureBaseColor.Sample(Sampler2, uv + t * float2( 2, 2)).rgb;
    float3 j = TextureBaseColor.Sample(Sampler2, uv + t * float2(-1,-1)).rgb;
    float3 k = TextureBaseColor.Sample(Sampler2, uv + t * float2( 1,-1)).rgb;
    float3 l = TextureBaseColor.Sample(Sampler2, uv + t * float2(-1, 1)).rgb;
    float3 m = TextureBaseColor.Sample(Sampler2, uv + t * float2( 1, 1)).rgb;

    float3 result = e * 0.125f;
    result += (a + c + g + i) * 0.03125f;
    result += (b + d + f + h) * 0.0625f;
    result += (j + k + l + m) * 0.125f;
    output.Color = float4(result, 1.0f);
    return output;
}
