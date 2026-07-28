// ============================================================
//  IBL_Prefilter.hlsl
//  環境キューブマップ -> roughness別 prefiltered specular キューブマップ
//  各 mip = 特定 roughness の GGX importance sampling 畳み込み
//  ディスパッチ単位で MipLevel/Roughness を切り替えて呼ぶ
// ============================================================

#include "../Constant.hlsl"

TextureCube<float4>       EnvCube      : register(t0);
RWTexture2DArray<float4>  PrefilterOut : register(u0);

SamplerState LinearClamp : register(s0);

cbuffer BakeParams : register(b0)
{
    uint  FaceSize;     // この mip の一辺サイズ
    uint  MipLevel;     // 現在の mip
    float Roughness;    // この mip に対応する roughness
    float _pad;
};


float3 CubeFaceUVToDir(uint face, float2 uv)
{
    float3 dir;
    switch (face)
    {
        case 0: dir = float3( 1.0f, -uv.y, -uv.x); break;
        case 1: dir = float3(-1.0f, -uv.y,  uv.x); break;
        case 2: dir = float3( uv.x,  1.0f,  uv.y); break;
        case 3: dir = float3( uv.x, -1.0f, -uv.y); break;
        case 4: dir = float3( uv.x, -uv.y,  1.0f); break;
        case 5: dir = float3(-uv.x, -uv.y, -1.0f); break;
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

float DistributionGGX(float NdotH, float roughness)
{
    float a = roughness * roughness;
    float a2 = a * a;
    float d = NdotH * NdotH * (a2 - 1.0f) + 1.0f;
    return a2 / max(PI * d * d, 1e-7f);
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= FaceSize || id.y >= FaceSize)
        return;

    uint face = id.z;
    float2 uv = (float2(id.xy) + 0.5f) / float(FaceSize);
    uv = uv * 2.0f - 1.0f;

    float3 N = CubeFaceUVToDir(face, uv);
    float3 R = N;
    float3 V = N; // split-sum 近似: V = R = N

    const uint SAMPLE_COUNT = 1024u;
    float3 prefilteredColor = float3(0, 0, 0);
    float  totalWeight = 0.0f;

    // 環境キューブのmip0解像度 (prefilter sample用の mip 選択に使用)
    float envSize = 512.0f;

    for (uint i = 0u; i < SAMPLE_COUNT; ++i)
    {
        float2 Xi = Hammersley(i, SAMPLE_COUNT);
        float3 H = ImportanceSampleGGX(Xi, N, Roughness);
        float3 L = normalize(2.0f * dot(V, H) * H - V);

        float NdotL = max(dot(N, L), 0.0f);
        if (NdotL > 0.0f)
        {
            // 解像度依存のmipバイアス（ファイアフライ抑制, Karis）
            float NdotH = max(dot(N, H), 0.0f);
            float D = DistributionGGX(NdotH, Roughness);
            float pdf = (D * NdotH / (4.0f * max(dot(H, V), 1e-4f))) + 1e-4f;

            float saTexel = 4.0f * PI / (6.0f * envSize * envSize);
            float saSample = 1.0f / (float(SAMPLE_COUNT) * pdf + 1e-4f);
            float mip = (Roughness == 0.0f) ? 0.0f
                      : 0.5f * log2(saSample / saTexel);

            prefilteredColor += EnvCube.SampleLevel(LinearClamp, L, mip).rgb * NdotL;
            totalWeight += NdotL;
        }
    }

    prefilteredColor = prefilteredColor / max(totalWeight, 1e-4f);

    PrefilterOut[uint3(id.xy, face)] = float4(prefilteredColor, 1.0f);
}
