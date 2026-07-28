#include "Common.hlsl"


PS_INPUT main(VS_INPUT input)
{
    PS_INPUT output;
    
    output.Position = float4(input.Position.xyz,1.0f);
        
    float4 normal = float4(input.Normal, 1.0f);
    
    output.TexCoord = input.TexCoord;
    
    output.Color = input.Color;
    
    return output;
}
