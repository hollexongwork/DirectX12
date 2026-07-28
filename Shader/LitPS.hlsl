
#include "common.hlsl"

PS_OUTPUT main(PS_INPUT input)
{
    PS_OUTPUT output;
    
    output.Color = TextureBaseColor.Sample(Sampler, input.TexCoord)*input.Color;
    
    float3 normal = normalize(input.Normal.xyz);
    
    float3 lightDir = normalize(DirectionalLightDirection.xyz);
    
    float diffuse = saturate(dot(normal, lightDir));
    
    output.Color.rgb *= diffuse;
    
    
    return output;
}
