// ============================================================
//  IBL_EquirectToCube.hlsl
//  Equirectangular HDR (Texture2D) -> Cubemap (6 faces) 変換
//  Compute Shader: 8x8 thread group, 1 thread = 1 cubemap texel
// ============================================================

#include "IBL_Common.hlsl"

Texture2D<float4>   EquirectMap : register(t0);
RWTexture2DArray<float4> CubeFaceOut : register(u0);

SamplerState LinearWrap : register(s0);


float2 DirToEquirect(float3 dir)
{
    float phi = atan2(dir.z, dir.x);
    float theta = acos(clamp(dir.y, -1.0f, 1.0f));
    float u = (phi / (2.0f * PI)) + 0.5f;
    float v = theta / PI;
    return float2(u, v);
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= FaceSize || id.y >= FaceSize)
        return;

    uint face = id.z;

    // ピクセル中心 -> -1..1
    float2 uv = (float2(id.xy) + 0.5f) / float(FaceSize);
    uv = uv * 2.0f - 1.0f;

    float3 dir = CubeFaceUVToDir(face, uv);
    float2 eq = DirToEquirect(dir);

    float3 color = EquirectMap.SampleLevel(LinearWrap, eq, 0).rgb;

    CubeFaceOut[uint3(id.xy, face)] = float4(color, 1.0f);
}
