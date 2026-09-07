#include "LumenSceneLightingCommon.hlsl"
#include "LumenProbeCommon.hlsl"

// =============================================================
//  LumenScreenProbeFilter_CS
//  ScreenProbeFilterGatherTraces 相当。トレースラディアンスを
//  3x3 の近傍プローブの「同じ octa テクセル」同士で空間フィルタ
//  する (プローブ深度 / 法線の近いものだけをブレンド)。
//    t19 = ProbeGeo / t20 = TraceRadiance / u4 = FilteredRadiance
//  Dispatch: (PW, PH, 1) - 1 グループ = 1 プローブ (8x8 スレッド)
// =============================================================

Texture2D<float4> ProbeGeoTexture : register(t19);
Texture2D<float4> TraceRadianceTexture : register(t20);
RWTexture2D<float4> RWFilteredRadiance : register(u4);

[numthreads(8, 8, 1)]
void main(uint3 GroupID : SV_GroupID, uint3 GroupThreadID : SV_GroupThreadID)
{
    const uint2 probe = GroupID.xy;
    const uint2 octaTexel = GroupThreadID.xy;
    const uint2 atlasTexel = probe * 8u + octaTexel;
    const int2 probeCount = (int2) PassProbeParams0.xy;

    float4 centerGeo = ProbeGeoTexture.Load(int3(probe, 0));
    float4 centerRadiance = TraceRadianceTexture.Load(int3(atlasTexel, 0));

    [branch]
    if (centerGeo.w <= 0.0f)
    {
        RWFilteredRadiance[atlasTexel] = centerRadiance;
        return;
    }

    float4 accum = centerRadiance;
    float totalWeight = 1.0f;

    [unroll]
    for (int dy = -1; dy <= 1; ++dy)
    {
        [unroll]
        for (int dx = -1; dx <= 1; ++dx)
        {
            if (dx == 0 && dy == 0)
            {
                continue;
            }

            int2 neighbor = (int2) probe + int2(dx, dy);
            if (neighbor.x < 0 || neighbor.y < 0 ||
                neighbor.x >= probeCount.x || neighbor.y >= probeCount.y)
            {
                continue;
            }

            float4 neighborGeo = ProbeGeoTexture.Load(int3(neighbor, 0));
            if (neighborGeo.w <= 0.0f)
            {
                continue;
            }

            // 深度 / 法線の一致度で重み付け (エッジ越しのにじみ防止)
            float depthDelta = abs(neighborGeo.w - centerGeo.w);
            float depthWeight = saturate(1.0f - depthDelta / max(0.1f * centerGeo.w, 0.05f));
            float normalWeight = saturate(dot(neighborGeo.xyz, centerGeo.xyz));

            float weight = depthWeight * normalWeight * normalWeight;
            if (weight < 0.01f)
            {
                continue;
            }

            int2 neighborTexel = neighbor * 8 + (int2) octaTexel;
            accum += TraceRadianceTexture.Load(int3(neighborTexel, 0)) * weight;
            totalWeight += weight;
        }
    }

    RWFilteredRadiance[atlasTexel] = accum / totalWeight;
}
