#include "Common.hlsl"


PS_INPUT main(VS_INPUT input)
{
    PS_INPUT output;
    float4x4 wvp;
    wvp = mul(LocalToWorld, View);
    wvp = mul(wvp, Projection);
    
    float4 position = float4(input.Position, 1.0f);
    output.Position = mul(position, wvp);
    output.WorldPosition = mul(position, LocalToWorld);
       
    float4 normal = float4(input.Normal, 0.0f);
    output.Normal = mul(normal, LocalToWorld);
    
    float4 tangent = float4(input.Tangent, 0.0f);
    output.Tangent = mul(tangent, LocalToWorld).xyz;
    
    output.TexCoord = input.TexCoord;
    
    output.Color = input.Color;
    
    return output;
}
