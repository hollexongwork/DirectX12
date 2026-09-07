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
//               y=現フレームカメラからの距離 (次フレームの検証用))
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
                float2 prevProbeUV = prevUV; // プローブグリッドは画面と相似

                float4 prevAux = PrevAuxTexture.SampleLevel(LumenTraceSampler, prevProbeUV, 0.0f);

                // 前フレームカメラからの距離で妥当性検証 (ディスオクルージョン棄却)
                float expectedPrevDist = length(worldPos - PassPrevCameraOrigin.xyz);
                float distDelta = abs(prevAux.y - expectedPrevDist);

                if (prevAux.y > 0.0f && distDelta < max(0.1f * expectedPrevDist, 0.05f))
                {
                    alpha = PassProbeParams1.z; // テンポラルブレンド率

                    float4 prevR = PrevSHRTexture.SampleLevel(LumenTraceSampler, prevProbeUV, 0.0f);
                    float4 prevG = PrevSHGTexture.SampleLevel(LumenTraceSampler, prevProbeUV, 0.0f);
                    float4 prevB = PrevSHBTexture.SampleLevel(LumenTraceSampler, prevProbeUV, 0.0f);

                    shR = lerp(prevR, shR, alpha);
                    shG = lerp(prevG, shG, alpha);
                    shB = lerp(prevB, shB, alpha);
                    skyVisibility = lerp(prevAux.x, skyVisibility, alpha);
                }
            }
        }
    }

    RWSHR[probe] = shR;
    RWSHG[probe] = shG;
    RWSHB[probe] = shB;

    // y = 現フレームカメラからの距離 (次フレームのリプロジェクション検証用)
    RWAux[probe] = float4(skyVisibility, probeGeo.w, 0.0f, 0.0f);
}
