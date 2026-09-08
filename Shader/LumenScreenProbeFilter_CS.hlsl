#include "LumenSceneLightingCommon.hlsl"
#include "LumenProbeCommon.hlsl"

// =============================================================
//  LumenScreenProbeFilter_CS
//  ScreenProbeFilterGatherTraces 相当。トレースラディアンスを
//  3x3 の近傍プローブの「同じ octa テクセル」同士で空間フィルタ
//  する (プローブ接平面 / 法線の近いものだけをブレンド)。
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

    // 中心プローブのワールド位置 (アンカーから再構築。平面距離重み用)
    const uint downsample = (uint) PassProbeParams0.z;
    const uint2 screenSize = (uint2) PassProbeParams1.xy;
    uint2 centerAnchor = LumenGetProbeAnchor(probe);
    float3 centerWorldPos = LumenReconstructWorldPosition(
        centerAnchor, LumenSceneDepth.Load(int3(centerAnchor, 0)));

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

            // 平面距離 / 法線の一致度で重み付け (エッジ越しのにじみ防止)。
            // 視距離差だと斜めの床で隣接プローブが棄却され、フィルタが
            // 効かず (= 1 プローブ 64 レイの生ノイズ) 明部が震えるため、
            // 「隣プローブの接平面から中心プローブまでの距離」で判定する
            uint2 neighborAnchor = LumenGetProbeAnchor((uint2) neighbor);
            float3 neighborWorldPos = LumenReconstructWorldPosition(
                neighborAnchor, LumenSceneDepth.Load(int3(neighborAnchor, 0)));
            float3 toCenter = centerWorldPos - neighborWorldPos;
            float planeDist = max(
                abs(dot(toCenter, neighborGeo.xyz)),
                abs(dot(toCenter, centerGeo.xyz)));
            float planeWeight = saturate(1.0f - planeDist / max(0.05f * centerGeo.w, 0.02f));
            float normalWeight = saturate(dot(neighborGeo.xyz, centerGeo.xyz));

            float weight = planeWeight * normalWeight * normalWeight;
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
