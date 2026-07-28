// ============================================================
//  IBL_EquirectToCube.hlsl
//  Equirectangular HDR (Texture2D) -> Cubemap (6 faces) 変換
//  Compute Shader: 8x8 thread group, 1 thread = 1 cubemap texel
// ============================================================

#include "../Constant.hlsl"

TextureCube<float4> dummyCube : register(t100); // unused (slot占有回避)
Texture2D<float4>   EquirectMap : register(t0);
RWTexture2DArray<float4> CubeFaceOut : register(u0);

SamplerState LinearWrap : register(s0);

cbuffer BakeParams : register(b0)
{
    uint  FaceSize;     // 出力キューブの一辺 (例: 512)
    uint  MipLevel;     // 未使用 (mip0生成のみ)
    float Roughness;    // 未使用
    float _pad;
};


// cubemap face index + uv(-1..1) -> world方向ベクトル (D3D左手系, +Z前)
float3 CubeFaceUVToDir(uint face, float2 uv)
{
    // uv: -1..1
    float3 dir;
    switch (face)
    {
        case 0: dir = float3( 1.0f, -uv.y, -uv.x); break; // +X
        case 1: dir = float3(-1.0f, -uv.y,  uv.x); break; // -X
        case 2: dir = float3( uv.x,  1.0f,  uv.y); break; // +Y
        case 3: dir = float3( uv.x, -1.0f, -uv.y); break; // -Y
        case 4: dir = float3( uv.x, -uv.y,  1.0f); break; // +Z
        case 5: dir = float3(-uv.x, -uv.y, -1.0f); break; // -Z
        default: dir = float3(0, 0, 1); break;
    }
    return normalize(dir);
}

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
