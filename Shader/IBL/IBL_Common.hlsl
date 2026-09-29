// ============================================================
//  IBL_Common.hlsl
//  IBL ベイク用コンピュートシェーダ (IBL_*.hlsl) の共通定義
//    - BakeParams 定数バッファ (b0)
//    - キューブ面 UV -> 方向ベクトル変換
//    - Hammersley 列 / GGX importance sampling
// ============================================================

#ifndef IBL_COMMON_HLSL
#define IBL_COMMON_HLSL

#include "../Constant.hlsl"

// C++ 側 IBL_BAKE_PARAMS (IBLBaker.cpp) と一致 (16byte)
cbuffer BakeParams : register(b0)
{
    uint  FaceSize;     // 出力の一辺 (キューブ面 / 対象 mip / LUT の解像度)
    uint  MipLevel;     // 現在の mip (C++ 側で設定。シェーダでは未参照)
    float Roughness;    // この mip に対応する roughness (Prefilter のみ使用)
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

// Hammersley 低食い違い列
float RadicalInverse_VdC(uint bits)
{
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return float(bits) * 2.3283064365386963e-10f;
}

float2 Hammersley(uint i, uint N)
{
    return float2(float(i) / float(N), RadicalInverse_VdC(i));
}

// GGX importance sampling -> 半球上のハーフベクトル
float3 ImportanceSampleGGX(float2 Xi, float3 N, float roughness)
{
    float a = roughness * roughness;

    float phi = 2.0f * PI * Xi.x;
    float cosTheta = sqrt((1.0f - Xi.y) / (1.0f + (a * a - 1.0f) * Xi.y));
    float sinTheta = sqrt(1.0f - cosTheta * cosTheta);

    float3 H;
    H.x = cos(phi) * sinTheta;
    H.y = sin(phi) * sinTheta;
    H.z = cosTheta;

    float3 up = abs(N.z) < 0.999f ? float3(0, 0, 1) : float3(1, 0, 0);
    float3 tangent = normalize(cross(up, N));
    float3 bitangent = cross(N, tangent);

    return normalize(tangent * H.x + bitangent * H.y + N * H.z);
}

#endif
