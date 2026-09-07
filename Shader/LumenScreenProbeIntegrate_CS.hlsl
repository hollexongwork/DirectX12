#include "LumenSceneLightingCommon.hlsl"
#include "LumenProbeCommon.hlsl"

// =============================================================
//  LumenScreenProbeIntegrate_CS
//  ScreenProbeIntegrate 相当。ピクセルごとに周囲 4 プローブの
//  SH L1 を深度 / 法線重み付きバイリニアでブレンドし、ピクセル
//  法線で評価した平均入射ラディアンスをフル解像度へ書き出す。
//    t19 = ProbeGeo / t21..t23 = SHR/SHG/SHB / t24 = Aux
//    u4  = DiffuseIndirect (フル解像度 RGBA16F):
//          rgb = 平均入射ラディアンス, a = スカイ可視率
//  Dispatch: (ceil(W/8), ceil(H/8), 1)
// =============================================================

Texture2D<float4> ProbeGeoTexture : register(t19);
Texture2D<float4> SHRTexture : register(t21);
Texture2D<float4> SHGTexture : register(t22);
Texture2D<float4> SHBTexture : register(t23);
Texture2D<float4> AuxTexture : register(t24);

RWTexture2D<float4> RWDiffuseIndirect : register(u4);

[numthreads(8, 8, 1)]
void main(uint3 DTid : SV_DispatchThreadID)
{
    const uint2 screenSize = (uint2) PassProbeParams1.xy;
    if (DTid.x >= screenSize.x || DTid.y >= screenSize.y)
    {
        return;
    }

    const uint2 pixel = DTid.xy;

    float deviceDepth = LumenSceneDepth.Load(int3(pixel, 0));
    float4 normalSample = LumenGBufferNormal.Load(int3(pixel, 0));

    [branch]
    if (deviceDepth >= 0.9999f || normalSample.w < 0.5f)
    {
        RWDiffuseIndirect[pixel] = float4(0.0f, 0.0f, 0.0f, 1.0f);
        return;
    }

    float3 worldPos = LumenReconstructWorldPosition(pixel, deviceDepth);
    float3 pixelNormal = normalize(normalSample.xyz);
    float pixelDist = length(worldPos - PassCameraOrigin.xyz);

    const float downsample = PassProbeParams0.z;
    const int2 probeCount = (int2) PassProbeParams0.xy;

    // ピクセルを囲む 4 プローブ (アンカー = セル中央基準のバイリニア)
    float2 probePos = ((float2) pixel - downsample * 0.5f + 0.5f) / downsample;
    int2 baseProbe = (int2) floor(probePos);
    float2 fracPos = probePos - (float2) baseProbe;

    float4 shR = float4(0.0f, 0.0f, 0.0f, 0.0f);
    float4 shG = float4(0.0f, 0.0f, 0.0f, 0.0f);
    float4 shB = float4(0.0f, 0.0f, 0.0f, 0.0f);
    float skyVisibility = 0.0f;
    float totalWeight = 0.0f;

    [unroll]
    for (int dy = 0; dy <= 1; ++dy)
    {
        [unroll]
        for (int dx = 0; dx <= 1; ++dx)
        {
            int2 probe = clamp(baseProbe + int2(dx, dy),
                int2(0, 0), probeCount - 1);

            float4 probeGeo = ProbeGeoTexture.Load(int3(probe, 0));
            if (probeGeo.w <= 0.0f)
            {
                continue;
            }

            // バイリニア x 深度 x 法線の重み
            float2 bilinear2 = float2(
                (dx == 0) ? (1.0f - fracPos.x) : fracPos.x,
                (dy == 0) ? (1.0f - fracPos.y) : fracPos.y);
            // 下限を設けない: 深度 / 法線の重みでエッジ越しを棄却する意図を
            // 1% の床が打ち消してしまうため (全プローブ棄却時は totalWeight
            // のフォールバックが受ける)
            float bilinearWeight = bilinear2.x * bilinear2.y;

            float depthDelta = abs(probeGeo.w - pixelDist);
            float depthWeight = saturate(1.0f - depthDelta / max(0.1f * pixelDist, 0.05f));
            float normalWeight = saturate(dot(probeGeo.xyz, pixelNormal));

            float weight = bilinearWeight * depthWeight * (normalWeight * normalWeight + 0.05f);
            if (weight < 0.001f)
            {
                continue;
            }

            shR += SHRTexture.Load(int3(probe, 0)) * weight;
            shG += SHGTexture.Load(int3(probe, 0)) * weight;
            shB += SHBTexture.Load(int3(probe, 0)) * weight;
            skyVisibility += AuxTexture.Load(int3(probe, 0)).x * weight;
            totalWeight += weight;
        }
    }

    [branch]
    if (totalWeight < 0.001f)
    {
        // 有効プローブなし: GI 0 / スカイ遮蔽なし
        RWDiffuseIndirect[pixel] = float4(0.0f, 0.0f, 0.0f, 1.0f);
        return;
    }

    float invWeight = 1.0f / totalWeight;
    shR *= invWeight;
    shG *= invWeight;
    shB *= invWeight;
    skyVisibility *= invWeight;

    float3 meanRadiance = LumenSH1EvaluateMeanRadiance(shR, shG, shB, pixelNormal);

    RWDiffuseIndirect[pixel] = float4(meanRadiance, saturate(skyVisibility));
}
