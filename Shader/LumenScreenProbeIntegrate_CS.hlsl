#include "LumenSceneLightingCommon.hlsl"
#include "LumenProbeCommon.hlsl"

// =============================================================
//  LumenScreenProbeIntegrate_CS
//  ScreenProbeIntegrate 相当。ピクセルごとに周囲 4 プローブの
//  SH L1 を平面距離 / 法線重み付きバイリニアでブレンドし、ピクセル
//  法線で評価した平均入射ラディアンスをフル解像度へ書き出す。
//
//  エッジ対策 (適応プローブ配置の代替):
//    1. 深度差ではなく「プローブ接平面からの距離」で重み付けする。
//       斜めから見た床や曲面は 8px 離れるだけで視距離が大きく変わる
//       ため、視距離差 (10%) だと同一面なのに棄却されて真っ黒になる。
//       接平面距離なら同一面は 0、前後の別物体だけが棄却される。
//    2. 2x2 が全滅したら 4x4 近傍から同一面のプローブを探す
//       (シルエット / 細い形状でアンカーが背景に落ちた場合の救済)。
//    3. それでも無ければ a = -1 (未カバー) を書き、DeferredPS が
//       ピクセル毎コーントレース (LumenScreenGather) で埋める。
//
//    t19 = ProbeGeo / t21..t23 = SHR/SHG/SHB / t24 = Aux
//    u4  = DiffuseIndirect (フル解像度 RGBA16F):
//          rgb = 平均入射ラディアンス, a = スカイ可視率 (-1 = 未カバー)
//  Dispatch: (ceil(W/8), ceil(H/8), 1)
// =============================================================

Texture2D<float4> ProbeGeoTexture : register(t19);
Texture2D<float4> SHRTexture : register(t21);
Texture2D<float4> SHGTexture : register(t22);
Texture2D<float4> SHBTexture : register(t23);
Texture2D<float4> AuxTexture : register(t24);

RWTexture2D<float4> RWDiffuseIndirect : register(u4);

// プローブのワールド位置 (アンカーピクセルから再構築。Setup / Trace と同一)
float3 LumenGetProbeWorldPosition(int2 Probe)
{
    const uint downsample = (uint) PassProbeParams0.z;
    const uint2 screenSize = (uint2) PassProbeParams1.xy;
    uint2 anchor = min((uint2) Probe * downsample + downsample / 2u, screenSize - 1u);
    float deviceDepth = LumenSceneDepth.Load(int3(anchor, 0));
    return LumenReconstructWorldPosition(anchor, deviceDepth);
}

// ピクセルとプローブの「同一面らしさ」 (0 = 別物体, 1 = 同一面)
//   接平面距離: ピクセル位置からプローブ接平面までの距離を視距離比で評価
//   法線      : 向きの近さ (角では別面のプローブが平面距離 0 になるため必要)
float LumenProbePlaneWeight(float4 ProbeGeo, int2 Probe, float3 PixelWorldPos,
    float3 PixelNormal, float PixelDist, out float OutNormalWeight)
{
    float3 probeWorldPos = LumenGetProbeWorldPosition(Probe);
    float3 toPixel = PixelWorldPos - probeWorldPos;

    // 両方の接平面で評価し厳しい方を採る (どちらか一方の面が
    // 他方の点を含んでしまう L 字コーナーの漏れを抑える)
    float planeDist = max(
        abs(dot(toPixel, ProbeGeo.xyz)),
        abs(dot(toPixel, PixelNormal)));

    // 視距離の 5% (最低 2cm) を超えたら別物体
    float planeWeight = saturate(1.0f - planeDist / max(0.05f * PixelDist, 0.02f));

    OutNormalWeight = saturate(dot(ProbeGeo.xyz, PixelNormal));
    return planeWeight;
}

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

    // ピクセルを囲む 4 プローブ (アンカー = セル中央基準のバイリニア)。
    // プローブ i のアンカーピクセル中心は i*ds + ds/2 + 0.5、ピクセル中心は
    // pixel + 0.5 なので、連続プローブ座標 = (pixel - ds/2) / ds
    float2 probePos = ((float2) pixel - downsample * 0.5f) / downsample;
    int2 baseProbe = (int2) floor(probePos);
    float2 fracPos = probePos - (float2) baseProbe;

    float4 shR = float4(0.0f, 0.0f, 0.0f, 0.0f);
    float4 shG = float4(0.0f, 0.0f, 0.0f, 0.0f);
    float4 shB = float4(0.0f, 0.0f, 0.0f, 0.0f);
    float skyVisibility = 0.0f;
    float totalWeight = 0.0f;

    //======================================================
    // 1. 2x2 バイリニア (平面距離 x 法線で別物体を棄却)
    //======================================================
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

            // 下限を設けない: 平面 / 法線の重みでエッジ越しを棄却する意図を
            // 床が打ち消してしまうため (全プローブ棄却時は下の探索が受ける)
            float2 bilinear2 = float2(
                (dx == 0) ? (1.0f - fracPos.x) : fracPos.x,
                (dy == 0) ? (1.0f - fracPos.y) : fracPos.y);
            float bilinearWeight = bilinear2.x * bilinear2.y;

            float normalWeight;
            float planeWeight = LumenProbePlaneWeight(
                probeGeo, probe, worldPos, pixelNormal, pixelDist, normalWeight);

            float weight = bilinearWeight * planeWeight * (normalWeight * normalWeight + 0.05f);
            if (weight <= 0.0f)
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

    //======================================================
    // 2. 救済探索: 4x4 近傍から同一面 (平面距離内 + 法線が同じ向き) の
    //    プローブを距離減衰で集める (シルエット / 細い形状用)
    //======================================================
    [branch]
    if (totalWeight < 1e-4f)
    {
        shR = float4(0.0f, 0.0f, 0.0f, 0.0f);
        shG = float4(0.0f, 0.0f, 0.0f, 0.0f);
        shB = float4(0.0f, 0.0f, 0.0f, 0.0f);
        skyVisibility = 0.0f;
        totalWeight = 0.0f;

        [unroll]
        for (int sy = -1; sy <= 2; ++sy)
        {
            [unroll]
            for (int sx = -1; sx <= 2; ++sx)
            {
                int2 probe = baseProbe + int2(sx, sy);
                if (probe.x < 0 || probe.y < 0 ||
                    probe.x >= probeCount.x || probe.y >= probeCount.y)
                {
                    continue;
                }

                float4 probeGeo = ProbeGeoTexture.Load(int3(probe, 0));
                if (probeGeo.w <= 0.0f)
                {
                    continue;
                }

                float normalWeight;
                float planeWeight = LumenProbePlaneWeight(
                    probeGeo, probe, worldPos, pixelNormal, pixelDist, normalWeight);

                // 救済は同じ向きの面に限定 (床の穴埋めに壁のプローブを使わない)
                if (planeWeight <= 0.0f || normalWeight < 0.5f)
                {
                    continue;
                }

                // プローブ空間距離で減衰 (近いプローブ優先)
                float2 delta = ((float2) probe - probePos);
                float distWeight = 1.0f / (1.0f + dot(delta, delta));

                float weight = distWeight * planeWeight * normalWeight * normalWeight;

                shR += SHRTexture.Load(int3(probe, 0)) * weight;
                shG += SHGTexture.Load(int3(probe, 0)) * weight;
                shB += SHBTexture.Load(int3(probe, 0)) * weight;
                skyVisibility += AuxTexture.Load(int3(probe, 0)).x * weight;
                totalWeight += weight;
            }
        }
    }

    //======================================================
    // 3. 未カバー: a = -1 を書き、DeferredPS がピクセル毎コーントレースで埋める
    //======================================================
    [branch]
    if (totalWeight < 1e-4f)
    {
        RWDiffuseIndirect[pixel] = float4(0.0f, 0.0f, 0.0f, -1.0f);
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
