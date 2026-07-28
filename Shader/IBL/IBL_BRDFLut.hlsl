// ============================================================
//  IBL_BRDFLut.hlsl
//  split-sum の 2nd 項: BRDF統合LUT を 2Dテクスチャに焼き込む
//  X軸 = NdotV, Y軸 = roughness, 出力 = (scale, bias)
//  起動時に一度だけ計算 -> ランタイムは 1タップで参照
// ============================================================

#include "../Constant.hlsl"

RWTexture2D<float2> BRDFLutOut : register(u0);

cbuffer BakeParams : register(b0)
{
    uint  FaceSize;     // LUT解像度 (例: 512)
    uint  MipLevel;
    float Roughness;
    float _pad;
};

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

// IBL用 Smith Geometry（k = a^2 / 2）
float GeometrySchlickGGX_IBL(float NdotV, float roughness)
{
    float a = roughness;
    float k = (a * a) / 2.0f;
    return NdotV / (NdotV * (1.0f - k) + k);
}

float GeometrySmith_IBL(float NdotV, float NdotL, float roughness)
{
    return GeometrySchlickGGX_IBL(NdotV, roughness)
         * GeometrySchlickGGX_IBL(NdotL, roughness);
}

float2 IntegrateBRDF(float NdotV, float roughness)
{
    float3 V;
    V.x = sqrt(1.0f - NdotV * NdotV);
    V.y = 0.0f;
    V.z = NdotV;

    float A = 0.0f;
    float B = 0.0f;
    float3 N = float3(0.0f, 0.0f, 1.0f);

    const uint SAMPLE_COUNT = 1024u;
    for (uint i = 0u; i < SAMPLE_COUNT; ++i)
    {
        float2 Xi = Hammersley(i, SAMPLE_COUNT);
        float3 H = ImportanceSampleGGX(Xi, N, roughness);
        float3 L = normalize(2.0f * dot(V, H) * H - V);

        float NdotL = max(L.z, 0.0f);
        float NdotH = max(H.z, 0.0f);
        float VdotH = max(dot(V, H), 0.0f);

        if (NdotL > 0.0f)
        {
            float G = GeometrySmith_IBL(NdotV, NdotL, roughness);
            float G_Vis = (G * VdotH) / max(NdotH * NdotV, 1e-7f);
            float Fc = pow(1.0f - VdotH, 5.0f);

            A += (1.0f - Fc) * G_Vis;
            B += Fc * G_Vis;
        }
    }
    return float2(A, B) / float(SAMPLE_COUNT);
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= FaceSize || id.y >= FaceSize)
        return;

    // ピクセル中心
    float NdotV     = (float(id.x) + 0.5f) / float(FaceSize);
    float roughness = (float(id.y) + 0.5f) / float(FaceSize);

    NdotV = max(NdotV, 1e-3f);

    float2 result = IntegrateBRDF(NdotV, roughness);

    BRDFLutOut[id.xy] = result;
}
