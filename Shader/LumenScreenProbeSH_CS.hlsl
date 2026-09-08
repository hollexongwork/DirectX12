#include "LumenSceneLightingCommon.hlsl"
#include "LumenProbeCommon.hlsl"

// =============================================================
//  LumenScreenProbeSH_CS
//  フィルタ済みトレースラディアンスをプローブごとに SH L1 へ
//  射影し、前フレームのプローブ SH をリプロジェクションして
//  テンポラル蓄積する (ScreenProbeTemporalAccumulation の SH 版)。
//    t19 = ProbeGeo / t20 = FilteredRadiance
//    t21..t23 = 前フレーム SHR/SHG/SHB / t24 = 前フレーム Aux
//    u4..u6   = SHR/SHG/SHB / u7 = Aux (x=スカイ可視率,
//               y=現フレームカメラからの距離, zw=法線 octahedral
//               (どちらも次フレームの履歴検証用))
//  Dispatch: (ceil(PW/8), ceil(PH/8), 1) - 1 スレッド = 1 プローブ
// =============================================================

Texture2D<float4> ProbeGeoTexture : register(t19);
Texture2D<float4> FilteredRadianceTexture : register(t20);
Texture2D<float4> PrevSHRTexture : register(t21);
Texture2D<float4> PrevSHGTexture : register(t22);
Texture2D<float4> PrevSHBTexture : register(t23);
Texture2D<float4> PrevAuxTexture : register(t24);

RWTexture2D<float4> RWSHR : register(u4);
RWTexture2D<float4> RWSHG : register(u5);
RWTexture2D<float4> RWSHB : register(u6);
RWTexture2D<float4> RWAux : register(u7);

[numthreads(8, 8, 1)]
void main(uint3 DTid : SV_DispatchThreadID)
{
    const uint2 probeCount = (uint2) PassProbeParams0.xy;
    if (DTid.x >= probeCount.x || DTid.y >= probeCount.y)
    {
        return;
    }

    const uint2 probe = DTid.xy;
    float4 probeGeo = ProbeGeoTexture.Load(int3(probe, 0));

    [branch]
    if (probeGeo.w <= 0.0f)
    {
        RWSHR[probe] = float4(0.0f, 0.0f, 0.0f, 0.0f);
        RWSHG[probe] = float4(0.0f, 0.0f, 0.0f, 0.0f);
        RWSHB[probe] = float4(0.0f, 0.0f, 0.0f, 0.0f);
        RWAux[probe] = float4(1.0f, 0.0f, 0.0f, 0.0f);
        return;
    }

    // ---- プローブのワールド位置 / レイ方向を再構築 (Trace と同一) ----
    const uint downsample = (uint) PassProbeParams0.z;
    const uint2 screenSize = (uint2) PassProbeParams1.xy;
    uint2 anchor = min(probe * downsample + downsample / 2u, screenSize - 1u);
    float deviceDepth = LumenSceneDepth.Load(int3(anchor, 0));
    float3 worldPos = LumenReconstructWorldPosition(anchor, deviceDepth);

    const float3 probeNormal = probeGeo.xyz;
    float3 tangent, bitangent;
    LumenBuildTangentBasis(probeNormal, tangent, bitangent);

    uint probeSeed = probe.x | (probe.y << 16);
    float2 jitter = LumenGetFrameJitter((uint) PassAtlasParams.w, probeSeed);

    // ---- SH L1 射影 (半球 = 立体角 2pi を 64 テクセルで分割) ----
    float4 shR = float4(0.0f, 0.0f, 0.0f, 0.0f);
    float4 shG = float4(0.0f, 0.0f, 0.0f, 0.0f);
    float4 shB = float4(0.0f, 0.0f, 0.0f, 0.0f);
    float skyVisibility = 0.0f;

    const float texelWeight = (2.0f * LUMEN_PI) / 64.0f;

    [loop]
    for (uint y = 0; y < 8; ++y)
    {
        [loop]
        for (uint x = 0; x < 8; ++x)
        {
            float2 octaUV = (float2((float) x, (float) y) + jitter) / 8.0f;
            float3 localDir = LumenHemiOctahedronToDirection(octaUV);
            float3 dir = normalize(
                tangent * localDir.x + bitangent * localDir.y + probeNormal * localDir.z);

            float4 radiance = FilteredRadianceTexture.Load(
                int3(probe * 8u + uint2(x, y), 0));

            LumenSH1Project(radiance.rgb, dir, texelWeight, shR, shG, shB);
            skyVisibility += radiance.a;
        }
    }

    skyVisibility /= 64.0f;

    // ---- テンポラル蓄積 (前フレームプローブグリッドへリプロジェクション) ----
    // 履歴は 4 タップを手動でバイリニアし、タップごとに「前フレームの
    // 同じ面か」を検証して重みに入れる。ハードウェアバイリニアで
    // Aux / SH を混ぜてから検証すると、深度エッジのプローブは隣の別物体の
    // 距離が混ざって毎フレーム検証に落ち (alpha = 1)、64 レイの生の推定が
    // そのまま出てプルプル震える。タップ単位なら同じ面のタップだけで
    // 履歴を作れるので、エッジのプローブも蓄積が効く。
    float alpha = 1.0f;

    [branch]
    if (PassProbeParams1.w > 0.5f) // 履歴有効
    {
        float4 prevClip = mul(float4(worldPos, 1.0f), PassPrevViewProjection);
        if (prevClip.w > 0.01f)
        {
            float2 prevNDC = prevClip.xy / prevClip.w;
            if (abs(prevNDC.x) < 1.0f && abs(prevNDC.y) < 1.0f)
            {
                float2 prevUV = float2(prevNDC.x * 0.5f + 0.5f, 0.5f - prevNDC.y * 0.5f);

                // スクリーン UV -> プローブ座標 (連続値)。
                // プローブ i のアンカーピクセルは i * downsample + downsample/2 で、
                // その中心は + 0.5 なので、プローブテクスチャは画面の単純な
                // 相似ではない (画面サイズが downsample の倍数でないとき、
                // 1080 / 16 -> 68 プローブ = 1088px 相当でずれる)。
                // prevUV のまま使うと画面下側ほど 1 プローブ近くずれた履歴を
                // 引き、縦方向のゴースト / にじみになる。
                const float downsampleF = PassProbeParams0.z;
                float2 prevPixel = prevUV * PassProbeParams1.xy;
                float2 prevProbe = (prevPixel - 0.5f - 0.5f * downsampleF) / downsampleF;

                int2 prevBase = (int2) floor(prevProbe);
                float2 prevFrac = prevProbe - (float2) prevBase;

                float expectedPrevDist = length(worldPos - PassPrevCameraOrigin.xyz);

                float4 histR = float4(0.0f, 0.0f, 0.0f, 0.0f);
                float4 histG = float4(0.0f, 0.0f, 0.0f, 0.0f);
                float4 histB = float4(0.0f, 0.0f, 0.0f, 0.0f);
                float histSky = 0.0f;
                float histWeight = 0.0f;

                [unroll]
                for (int ty = 0; ty <= 1; ++ty)
                {
                    [unroll]
                    for (int tx = 0; tx <= 1; ++tx)
                    {
                        int2 tap = prevBase + int2(tx, ty);
                        if (tap.x < 0 || tap.y < 0 ||
                            tap.x >= (int) probeCount.x || tap.y >= (int) probeCount.y)
                        {
                            continue;
                        }

                        float4 prevAux = PrevAuxTexture.Load(int3(tap, 0));
                        if (prevAux.y <= 0.0f)
                        {
                            continue; // 無効プローブ (スカイ / Unlit)
                        }

                        // 前フレームのプローブ位置を再構築:
                        //   前アンカーピクセルの視線方向 (PrevInvViewProjection)
                        //   x 前カメラからの距離 (Aux.y)
                        // 距離差だけで検証すると、斜めから見た床では隣のプローブ
                        // (16px) で視距離が 10% 以上変わるため、カメラを動かして
                        // リプロジェクション先がプローブの中間に落ちるたびに履歴が
                        // 棄却され (alpha = 1)、64 レイの生推定が出て激しく明滅する。
                        // 「前プローブの接平面からの距離」なら同一面は動いても 0。
                        uint2 prevAnchor = min((uint2) tap * downsample + downsample / 2u,
                            screenSize - 1u);
                        float2 prevAnchorUV = ((float2) prevAnchor + 0.5f) / PassProbeParams1.xy;
                        float4 prevAnchorNDC = float4(
                            prevAnchorUV.x * 2.0f - 1.0f, (1.0f - prevAnchorUV.y) * 2.0f - 1.0f,
                            0.5f, 1.0f);
                        float4 prevRayH = mul(prevAnchorNDC, PassPrevInvViewProjection);
                        float3 prevRayDir = normalize(
                            prevRayH.xyz / prevRayH.w - PassPrevCameraOrigin.xyz);
                        float3 prevProbePos = PassPrevCameraOrigin.xyz + prevRayDir * prevAux.y;

                        float3 prevNormal = LumenOctahedronToDirection(prevAux.zw);
                        float3 toCurrent = worldPos - prevProbePos;
                        float planeDist = max(
                            abs(dot(toCurrent, prevNormal)),
                            abs(dot(toCurrent, probeNormal)));
                        if (planeDist > max(0.05f * expectedPrevDist, 0.02f))
                        {
                            continue; // 別物体 (ディスオクルージョン)
                        }

                        // 粗い距離検証 (平面が偶然揃う遠くの別物体を除外)
                        if (abs(prevAux.y - expectedPrevDist) > max(0.3f * expectedPrevDist, 0.1f))
                        {
                            continue;
                        }

                        // 前フレームのプローブ法線で面一致を検証
                        // (平面距離が近いだけの別面 = 角の向こう側を弾く)。
                        // 曲面 (彫像など) では 16px で法線が大きく回るため
                        // 60 度まで許容する (厳しすぎると動かした時だけ履歴が
                        // 落ちて曲面が明滅する。残りはフル解像度側の蓄積が吸収)
                        if (dot(prevNormal, probeNormal) < 0.5f)
                        {
                            continue;
                        }

                        float w = ((tx == 0) ? (1.0f - prevFrac.x) : prevFrac.x)
                                * ((ty == 0) ? (1.0f - prevFrac.y) : prevFrac.y);
                        if (w <= 0.0f)
                        {
                            continue;
                        }

                        histR += PrevSHRTexture.Load(int3(tap, 0)) * w;
                        histG += PrevSHGTexture.Load(int3(tap, 0)) * w;
                        histB += PrevSHBTexture.Load(int3(tap, 0)) * w;
                        histSky += prevAux.x * w;
                        histWeight += w;
                    }
                }

                // 有効タップの合計重みが小さすぎる (ほぼ別物体) 場合は履歴を捨てる
                if (histWeight > 0.05f)
                {
                    float invHist = 1.0f / histWeight;
                    histR *= invHist;
                    histG *= invHist;
                    histB *= invHist;
                    histSky *= invHist;

                    alpha = PassProbeParams1.z; // テンポラルブレンド率

                    shR = lerp(histR, shR, alpha);
                    shG = lerp(histG, shG, alpha);
                    shB = lerp(histB, shB, alpha);
                    skyVisibility = lerp(histSky, skyVisibility, alpha);
                }
            }
        }
    }

    RWSHR[probe] = shR;
    RWSHG[probe] = shG;
    RWSHB[probe] = shB;

    // y  = 現フレームカメラからの距離 (次フレームのリプロジェクション検証用)
    // zw = プローブ法線 (octahedral, 次フレームの面一致検証用)
    RWAux[probe] = float4(skyVisibility, probeGeo.w, LumenDirectionToOctahedron(probeNormal));
}
