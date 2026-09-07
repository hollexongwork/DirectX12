#include "LumenSceneLightingCommon.hlsl"
#include "LumenProbeCommon.hlsl"

// =============================================================
//  LumenScreenProbeSetup_CS
//  ScreenProbeGather のプローブ配置 (均一グリッド版)。
//  16px ごとのアンカーピクセルから G-Buffer を読み、プローブの
//  ジオメトリ (法線 + ビュー距離) を確定する。
//    u4 = ProbeGeo (RGBA16F): xyz = ワールド法線, w = ビュー距離
//         (w <= 0 = 無効プローブ: スカイ / Unlit)
//  Dispatch: (ceil(PW/8), ceil(PH/8), 1)
// =============================================================

RWTexture2D<float4> RWProbeGeo : register(u4);

[numthreads(8, 8, 1)]
void main(uint3 DTid : SV_DispatchThreadID)
{
    const uint2 probeCount = (uint2) PassProbeParams0.xy;
    if (DTid.x >= probeCount.x || DTid.y >= probeCount.y)
    {
        return;
    }

    const uint downsample = (uint) PassProbeParams0.z;
    const uint2 screenSize = (uint2) PassProbeParams1.xy;

    // アンカーピクセル (プローブセル中央)
    uint2 anchor = min(DTid.xy * downsample + downsample / 2u, screenSize - 1u);

    float deviceDepth = LumenSceneDepth.Load(int3(anchor, 0));
    float4 normalSample = LumenGBufferNormal.Load(int3(anchor, 0));

    // スカイ (深度クリア値 1.0) / Unlit (Normal.w = 0) は無効プローブ
    if (deviceDepth >= 0.9999f || normalSample.w < 0.5f)
    {
        RWProbeGeo[DTid.xy] = float4(0.0f, 0.0f, 0.0f, 0.0f);
        return;
    }

    float3 worldPos = LumenReconstructWorldPosition(anchor, deviceDepth);
    float viewDist = length(worldPos - PassCameraOrigin.xyz);

    RWProbeGeo[DTid.xy] = float4(normalize(normalSample.xyz), viewDist);
}
