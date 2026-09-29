// ============================================================
//  IBL_Prefilter.hlsl
//  環境キューブマップ -> roughness別 prefiltered specular キューブマップ
//  各 mip = 特定 roughness の GGX importance sampling 畳み込み
//  ディスパッチ単位で MipLevel/Roughness を切り替えて呼ぶ
// ============================================================

#include "IBL_Common.hlsl"

TextureCube<float4>       EnvCube      : register(t0);
RWTexture2DArray<float4>  PrefilterOut : register(u0);

SamplerState LinearClamp : register(s0);


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
