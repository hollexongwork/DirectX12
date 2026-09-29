// ============================================================
//  IBL_Irradiance.hlsl
//  環境キューブマップ -> 拡散irradianceキューブマップ畳み込み
//  半球コサイン重み積分（ランタイムではなく起動時に一度きり）
// ============================================================

#include "IBL_Common.hlsl"

TextureCube<float4>       EnvCube     : register(t0);
RWTexture2DArray<float4>  IrradianceOut : register(u0);

SamplerState LinearClamp : register(s0);

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= FaceSize || id.y >= FaceSize)
        return;

    uint face = id.z;
    float2 uv = (float2(id.xy) + 0.5f) / float(FaceSize);
    uv = uv * 2.0f - 1.0f;

    float3 N = CubeFaceUVToDir(face, uv);

    // 接空間基底
    float3 up = abs(N.y) < 0.999f ? float3(0, 1, 0) : float3(1, 0, 0);
    float3 right = normalize(cross(up, N));
    up = normalize(cross(N, right));

    float3 irradiance = float3(0, 0, 0);
    float  sampleCount = 0.0f;

    const float deltaPhi   = (2.0f * PI) / 64.0f;
    const float deltaTheta = (0.5f * PI) / 32.0f;

    for (float phi = 0.0f; phi < 2.0f * PI; phi += deltaPhi)
    {
        float sinPhi = sin(phi);
        float cosPhi = cos(phi);
        for (float theta = 0.0f; theta < 0.5f * PI; theta += deltaTheta)
        {
            float sinTheta = sin(theta);
            float cosTheta = cos(theta);

            // 接空間サンプル方向
            float3 tangentSample = float3(sinTheta * cosPhi, sinTheta * sinPhi, cosTheta);
            float3 sampleVec = tangentSample.x * right
                             + tangentSample.y * up
                             + tangentSample.z * N;

            // cos重み (Lambert) + 立体角補正 sinTheta
            irradiance += EnvCube.SampleLevel(LinearClamp, sampleVec, 0).rgb
                        * cosTheta * sinTheta;
            sampleCount += 1.0f;
        }
    }

    irradiance = PI * irradiance / sampleCount;

    IrradianceOut[uint3(id.xy, face)] = float4(irradiance, 1.0f);
}
