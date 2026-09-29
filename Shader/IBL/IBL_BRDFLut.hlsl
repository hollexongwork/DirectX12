// ============================================================
//  IBL_BRDFLut.hlsl
//  split-sum の 2nd 項: BRDF統合LUT を 2Dテクスチャに焼き込む
//  X軸 = NdotV, Y軸 = roughness, 出力 = (scale, bias)
//  起動時に一度だけ計算 -> ランタイムは 1タップで参照
// ============================================================

#include "IBL_Common.hlsl"

RWTexture2D<float2> BRDFLutOut : register(u0);

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
