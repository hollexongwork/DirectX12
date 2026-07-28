// ============================================================
//  IBL_Irradiance.hlsl
//  環境キューブマップ -> 拡散irradianceキューブマップ畳み込み
//  半球コサイン重み積分（ランタイムではなく起動時に一度きり）
// ============================================================

#include "../Constant.hlsl"

TextureCube<float4>       EnvCube     : register(t0);
RWTexture2DArray<float4>  IrradianceOut : register(u0);

SamplerState LinearClamp : register(s0);

cbuffer BakeParams : register(b0)
{
    uint  FaceSize;     // irradianceキューブの一辺 (例: 32)
    uint  MipLevel;
    float Roughness;
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
