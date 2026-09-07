#include "LumenSceneLightingCommon.hlsl"
#include "LumenProbeCommon.hlsl"

// =============================================================
//  LumenRadianceCacheSH_CS
//  Radiance Cache の octahedral ラディアンスをプローブごとに
//  SH L1 へ射影し、3D ボリューム (N^3) へ書く。半透明パス
//  (TranslucentPS) が WRAP サンプラのトライリニアで採光する。
//    t19 = RC ラディアンスアトラス
//    u4..u6 = SH ボリューム R/G/B (Texture3D<float4>)
//  Dispatch: (N/8, N/8, N) - 1 スレッド = 1 プローブ
// =============================================================

Texture2D<float4> RadianceCacheAtlas : register(t19);

RWTexture3D<float4> RWRCSH_R : register(u4);
RWTexture3D<float4> RWRCSH_G : register(u5);
RWTexture3D<float4> RWRCSH_B : register(u6);

[numthreads(8, 8, 1)]
void main(uint3 DTid : SV_DispatchThreadID)
{
    const int probesPerAxis = (int) PassRCParams1.x;
    if ((int) DTid.x >= probesPerAxis || (int) DTid.y >= probesPerAxis ||
        (int) DTid.z >= probesPerAxis)
    {
        return;
    }

    const uint probeLinear = DTid.x
        + DTid.y * (uint) probesPerAxis
        + DTid.z * (uint) (probesPerAxis * probesPerAxis);

    uint2 tileOrigin = uint2(probeLinear % 64u, probeLinear / 64u) * 8u;

    // ---- SH L1 射影 (全球 = 立体角 4pi を 64 テクセルで分割) ----
    float4 shR = float4(0.0f, 0.0f, 0.0f, 0.0f);
    float4 shG = float4(0.0f, 0.0f, 0.0f, 0.0f);
    float4 shB = float4(0.0f, 0.0f, 0.0f, 0.0f);

    const float texelWeight = (4.0f * LUMEN_PI) / 64.0f;

    [loop]
    for (uint y = 0; y < 8; ++y)
    {
        [loop]
        for (uint x = 0; x < 8; ++x)
        {
            float2 octaUV = (float2((float) x, (float) y) + 0.5f) / 8.0f;
            float3 dir = LumenOctahedronToDirection(octaUV);

            float3 radiance = RadianceCacheAtlas.Load(
                int3(tileOrigin + uint2(x, y), 0)).rgb;

            LumenSH1Project(radiance, dir, texelWeight, shR, shG, shB);
        }
    }

    RWRCSH_R[DTid] = shR;
    RWRCSH_G[DTid] = shG;
    RWRCSH_B[DTid] = shB;
}
