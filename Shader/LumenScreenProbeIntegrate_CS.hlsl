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
//    3. それでも無ければピクセル自身から半球コーンを SDF トレース
//       (ピクセル毎フォールバック。フレームで回転し下のテンポラルで平均)。
//
//  フル解像度テンポラル蓄積 (ScreenProbeGatherTemporal 相当):
//    前フレームの DiffuseIndirect (t26) を PrevViewProjection で
//    リプロジェクションし、前フレーム LinearDepth (t25) で面一致を
//    検証した 4 タップとブレンドする。プローブ側の SH 蓄積が
//    (アンカーが別の面に乗り換えた / 画面端から入った / 曲率が高い等で)
//    履歴を捨てたプローブの生ノイズや、プローブ切り替えによる
//    16px ブロック単位の明滅を、ピクセル単位で吸収する。
//
//  Short Range AO (LumenScreenProbeGather の ShortRangeAO 相当):
//    16px プローブでは潰れる接触部 (数 cm～数十 cm) の遮蔽を、ピクセル
//    ごとに半球方向へ短いスクリーンスペースレイ (PassShortRangeAO.x [m])
//    を飛ばして補う。遮られなかった方向の平均 = ベントノーマルで SH を
//    評価し直し、可視率 (AO) を GI ラディアンスとスカイ可視率に掛ける。
//    半径が短いのでプローブ側の遠距離遮蔽 (スカイ可視率) と範囲が重ならず
//    二重計上にならない。ノイズは下のテンポラル蓄積が平均する。
//
//    t19 = ProbeGeo / t21..t23 = SHR/SHG/SHB / t24 = Aux
//    t25 = PrevLinearDepth / t26 = 前フレーム DiffuseIndirect
//    u4  = DiffuseIndirect (フル解像度 RGBA16F):
//          rgb = 平均入射ラディアンス, a = スカイ可視率 (-1 = 未カバー)
//  Dispatch: (ceil(W/8), ceil(H/8), 1)
// =============================================================

Texture2D<float4> ProbeGeoTexture : register(t19);
Texture2D<float4> SHRTexture : register(t21);
Texture2D<float4> SHGTexture : register(t22);
Texture2D<float4> SHBTexture : register(t23);
Texture2D<float4> AuxTexture : register(t24);
Texture2D<float4> PrevDiffuseIndirectTexture : register(t26);

RWTexture2D<float4> RWDiffuseIndirect : register(u4);

// ピクセル毎フォールバックのコーン数
#define LUMEN_INTEGRATE_FALLBACK_CONES 8u

// Short Range AO のレイあたりステップ数
#define LUMEN_SHORT_RANGE_AO_STEPS 4u

// -------------------------------------------------------------
//  Short Range AO の 1 レイ (スクリーンスペース、LinearDepth と比較)
//  戻り値: 1 = 遮蔽なし, 0 = 遮蔽
//    StepJitter : [0,1) ステップ位相のジッタ (フレーム / ピクセルで変化)
//    Thickness  : 面の厚み [m]。これより深く潜ったら「薄い物の裏へ抜けた」
//                 とみなして遮蔽にしない (SSAO のハロー抑制)
// -------------------------------------------------------------
float LumenShortRangeAOTrace(float3 RayStart, float3 RayDir, float MaxDist,
    float Thickness, float StepJitter)
{
    const uint2 screenSize = (uint2) PassProbeParams1.xy;

    [unroll]
    for (uint s = 0; s < LUMEN_SHORT_RANGE_AO_STEPS; ++s)
    {
        float t = MaxDist * ((float) s + StepJitter) / (float) LUMEN_SHORT_RANGE_AO_STEPS;
        t = max(t, MaxDist * 0.05f);

        float3 p = RayStart + RayDir * t;

        float4 clipPos = mul(float4(p, 1.0f), PassViewProjection);
        if (clipPos.w <= 0.01f)
        {
            return 1.0f; // カメラ背後: 判定不能 = 遮蔽なし
        }

        float2 ndc = clipPos.xy / clipPos.w;
        if (abs(ndc.x) >= 1.0f || abs(ndc.y) >= 1.0f)
        {
            return 1.0f; // 画面外: 判定不能 = 遮蔽なし
        }

        float2 uv = float2(ndc.x * 0.5f + 0.5f, 0.5f - ndc.y * 0.5f);
        int2 depthPixel = clamp((int2) (uv * (float2) screenSize),
            int2(0, 0), (int2) screenSize - 1);
        float sceneZ = LumenLinearDepth.Load(int3(depthPixel, 0)).r;
        if (sceneZ <= 0.0f)
        {
            continue; // スカイ
        }

        // レイ点が面より奥 (= 面に遮られている) かつ厚み以内なら遮蔽
        float depthDelta = clipPos.w - sceneZ;
        if (depthDelta > 0.005f && depthDelta < Thickness)
        {
            return 0.0f;
        }
    }

    return 1.0f;
}

// プローブのワールド位置 (アンカーピクセルから再構築。Setup / Trace と同一)
float3 LumenGetProbeWorldPosition(int2 Probe)
{
    const uint downsample = (uint) PassProbeParams0.z;
    const uint2 screenSize = (uint2) PassProbeParams1.xy;
    uint2 anchor = LumenGetProbeAnchor((uint2) Probe);
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

    // ピクセルを囲む 4 プローブ (アンカー基準のバイリニア)。
    // プローブ i のアンカーピクセル中心は i*ds + jitter + 0.5、ピクセル中心は
    // pixel + 0.5 なので、連続プローブ座標 = (pixel - jitter) / ds
    float2 probePos = ((float2) pixel - PassProbeJitter.xy) / downsample;
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

    float3 meanRadiance;

    // ピクセル IGN + フレーム番号の回転 (フォールバックコーン / Short Range AO 共通。
    // フレームで回るので下のテンポラル蓄積がサンプルを平均する)
    const float ign = frac(52.9829189f *
        frac(dot(float2(pixel), float2(0.06711056f, 0.00583715f))));
    const float frameRot = frac((float) ((uint) PassAtlasParams.w & 63u) * 0.6180339887f);
    const float randomRotation = frac(ign + frameRot) * (2.0f * LUMEN_PI);

    //======================================================
    // Short Range AO (ベントノーマル + 近距離可視率)
    //======================================================
    float shortRangeAO = 1.0f;
    float3 bentNormal = pixelNormal;

    const float aoMaxDist = PassShortRangeAO.x; // 0 = 無効
    [branch]
    if (aoMaxDist > 0.0f)
    {
        const uint numAORays = clamp((uint) PassShortRangeAO.y, 1u, 8u);
        const float aoThickness = max(PassShortRangeAO.w, 0.01f);
        // 開始オフセットは面バイアスより小さく (接触部の陰影を残す)
        const float aoBias = min(PassTraceParams.y, 0.01f);
        float3 aoStart = worldPos + pixelNormal * aoBias;
        // ステップ位相もフレーム / ピクセルでずらす
        float stepJitter = frac(ign * 7.0f + frameRot);

        float visibleSum = 0.0f;
        float3 bentSum = float3(0.0f, 0.0f, 0.0f);

        [loop]
        for (uint r = 0; r < numAORays; ++r)
        {
            // コサイン分布なので可視率の平均がそのままコサイン重み AO
            float3 dir = GetLumenHemisphereRay(pixelNormal, r, numAORays, randomRotation);
            float vis = LumenShortRangeAOTrace(aoStart, dir, aoMaxDist, aoThickness, stepJitter);
            visibleSum += vis;
            bentSum += dir * vis;
        }

        float rawAO = visibleSum / (float) numAORays;
        shortRangeAO = lerp(1.0f, rawAO, saturate(PassShortRangeAO.z)); // 強度

        // ベントノーマル (全遮蔽なら幾何法線のまま)
        if (PassRadiosityParams.w > 0.5f && visibleSum > 0.0f)
        {
            bentNormal = normalize(bentSum + pixelNormal * 0.01f);
        }
    }

    //======================================================
    // 3. 未カバー: ピクセル自身から半球コーンをトレース
    //    (方位はピクセル IGN + フレーム番号で回転し、下のテンポラル蓄積が
    //     フレーム間で平均する。SWRT (メッシュ SDF + Global SDF) 経路)
    //======================================================
    [branch]
    if (totalWeight < 1e-4f)
    {
        const uint numCones = LUMEN_INTEGRATE_FALLBACK_CONES;

        // 半球を numCones 個のコーンで分割したときの半角 tan
        const float coneCos = saturate(1.0f - 1.0f / (float) numCones);
        const float coneTan = sqrt(saturate(1.0f - coneCos * coneCos)) / max(coneCos, 0.1f);

        const float maxTrace = PassTraceParams.x;
        const float bias = PassTraceParams.y;
        float3 rayStart = worldPos + pixelNormal * bias;

        float3 radianceSum = float3(0.0f, 0.0f, 0.0f);
        float visibilitySum = 0.0f;

        [loop]
        for (uint c = 0; c < numCones; ++c)
        {
            float3 rayDir = GetLumenHemisphereRay(pixelNormal, c, numCones, randomRotation);
            FLumenTraceResult trace = TraceLumenRay(
                rayStart, rayDir, maxTrace, coneTan, PassNumLumenObjects);

            // プローブ経路と同じ規約: ミスはラディアンス 0 (スカイは IBL 側)
            if (trace.bHit)
            {
                radianceSum += ResolveLumenRayRadiance(trace, rayStart, rayDir, PassRCParams1.w);
            }
            visibilitySum += trace.Visibility;
        }

        meanRadiance = radianceSum / (float) numCones;
        skyVisibility = visibilitySum / (float) numCones;
    }
    else
    {
        float invWeight = 1.0f / totalWeight;
        shR *= invWeight;
        shG *= invWeight;
        shB *= invWeight;
        skyVisibility *= invWeight;

        // ベントノーマルで評価 = 遮られていない方向の放射輝度を優先 (方向性 AO)
        meanRadiance = LumenSH1EvaluateMeanRadiance(shR, shG, shB, bentNormal);
    }

    // Short Range AO を GI ラディアンスとスカイ可視率 (IBL 減衰) の両方に適用
    meanRadiance *= shortRangeAO;
    skyVisibility *= shortRangeAO;

    float4 result = float4(meanRadiance, saturate(skyVisibility));

    // デバッグ表示 (PassRadiosityParams.z = 1): rgb に AO そのものを出す
    if (PassRadiosityParams.z > 0.5f)
    {
        result.rgb = shortRangeAO.xxx;
    }

    //======================================================
    // 4. フル解像度テンポラル蓄積 (前フレームへリプロジェクション)
    //======================================================
    const float screenAlpha = PassRadiosityParams.y;

    [branch]
    if (PassProbeParams1.w > 0.5f && screenAlpha < 1.0f)
    {
        float4 prevClip = mul(float4(worldPos, 1.0f), PassPrevViewProjection);
        if (prevClip.w > 0.01f)
        {
            float2 prevNDC = prevClip.xy / prevClip.w;
            if (abs(prevNDC.x) < 1.0f && abs(prevNDC.y) < 1.0f)
            {
                float2 prevUV = float2(prevNDC.x * 0.5f + 0.5f, 0.5f - prevNDC.y * 0.5f);
                float2 prevPixelF = prevUV * PassProbeParams1.xy - 0.5f;
                int2 prevBase = (int2) floor(prevPixelF);
                float2 prevFrac = prevPixelF - (float2) prevBase;

                // 前フレームのビュー深度 (clip.w) と履歴 LinearDepth を
                // タップ単位で比較し、別の面 (ディスオクルージョン) を弾く
                const float prevViewZ = prevClip.w;
                const float depthTolerance = max(0.05f * prevViewZ, 0.02f);

                float4 history = float4(0.0f, 0.0f, 0.0f, 0.0f);
                float histWeight = 0.0f;

                [unroll]
                for (int ty = 0; ty <= 1; ++ty)
                {
                    [unroll]
                    for (int tx = 0; tx <= 1; ++tx)
                    {
                        int2 tap = prevBase + int2(tx, ty);
                        if (tap.x < 0 || tap.y < 0 ||
                            tap.x >= (int) screenSize.x || tap.y >= (int) screenSize.y)
                        {
                            continue;
                        }

                        float prevDepth = LumenPrevLinearDepth.Load(int3(tap, 0)).r;
                        if (prevDepth <= 0.0f || abs(prevDepth - prevViewZ) > depthTolerance)
                        {
                            continue;
                        }

                        float4 prevValue = PrevDiffuseIndirectTexture.Load(int3(tap, 0));
                        if (prevValue.a < 0.0f)
                        {
                            continue; // 前フレームの未カバーマーカー
                        }

                        float w = ((tx == 0) ? (1.0f - prevFrac.x) : prevFrac.x)
                                * ((ty == 0) ? (1.0f - prevFrac.y) : prevFrac.y);
                        history += prevValue * w;
                        histWeight += w;
                    }
                }

                if (histWeight > 0.05f)
                {
                    history /= histWeight;
                    result = lerp(history, result, screenAlpha);
                }
            }
        }
    }

    RWDiffuseIndirect[pixel] = result;
}
