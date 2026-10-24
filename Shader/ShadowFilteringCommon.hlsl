#ifndef SHADOW_FILTERING_COMMON_HLSL
#define SHADOW_FILTERING_COMMON_HLSL

#include "Common.hlsl"
#include "DistanceFieldShadowing.hlsl"
#include "ShadowProjectionCommon.hlsl"

// =============================================================
//  ShadowFilteringCommon
//  シャドウマップの受光側評価。
//
//    - ディレクショナル : CSM (ビュー深度でカスケード選択 + 距離フェード)
//    - スポット / レクト : 単一透視投影 (t15 のスライス 1 枚)
//    - ポイント          : world 軸整列キューブ 6 面。支配軸から面を選び、
//                          デバイス深度を再構築して比較する
//
//  フィルタは 3x3 PCF (比較サンプラ s2 のバイリニア比較込みで
//  実効 4x4 相当)。バイアスは
//    - 深度バイアス   : NDC 空間で比較深度から減算 (CPU 側で正規化済み)
//    - 法線オフセット : 受光点を法線方向へ押し出す (スロープバイアス相当)
//  の 2 系統 (+ 深度パス PSO のラスタライザバイアス)。
//
//  投影 / PCF の本体はレジスタ非依存の ShadowProjectionCommon.hlsl
//  (Volumetric Fog のコンピュートパスと共有)。ここはグラフィックス
//  レジスタ (b5 / t14 / t15 / t16 / s2) を束ねる従来 API のみ。
// =============================================================

// -------------------------------------------------------------
//  ディレクショナルライトの CSM シャドウ係数 (1 = 影なし)
//    WorldPos  : 受光点 (ワールド)
//    N         : ワールド法線
//    ViewDepth : カメラビュー空間の深度 (mul(worldPos, View).z)
// -------------------------------------------------------------
float GetDirectionalShadow(float3 WorldPos, float3 N, float ViewDepth)
{
    const uint numCascades = (uint) DirectionalShadowParams.x;
    if (numCascades == 0)
    {
        return 1.0f;
    }

    const float shadowDistance = DirectionalShadowParams.y;
    const float fadeStart = DirectionalShadowParams.z;

    // ---- CSM 係数 (csmFade: 1 = CSM 完全有効, 0 = CSM 無効域) ----
    float csm = 1.0f;
    float csmFade = 0.0f;
    [branch]
    if (ViewDepth < shadowDistance)
    {
        // カスケード選択 (未使用スロットの split は 1e9)
        uint cascade = SelectShadowCascade(ViewDepth, CascadeSplits);

        if (cascade < numCascades)
        {
            // 法線オフセット + シャドウクリップへ投影 (オルソなので w=1)
            float3 offsetPos = WorldPos + N * CascadeNormalOffset[cascade];

            csm = ProjectDirectionalCascadeShadow(DirectionalShadowCascades, ShadowSampler,
                WorldToShadowCascade[cascade], CascadeDepthBias[cascade],
                DirectionalShadowParams.w, cascade, offsetPos);

            csmFade = saturate((shadowDistance - ViewDepth) / max(shadowDistance - fadeStart, 1e-4f));
        }
    }

    // ---- Distance Field 係数 (RayTraced Distance Field Shadows) ----
    // CSM が減衰しきる領域 (フェード帯 + シャドウ距離以遠) を SDF の
    // レイマーチで補う。DFShadowDistance 付近で 1 へフェードアウトする。
    float farShadow = 1.0f;
    const bool bDF = DFShadowParams1.x > 0.5f;
    const float dfDistance = DFShadowParams0.y;
    [branch]
    if (bDF && csmFade < 1.0f && ViewDepth < dfDistance)
    {
        // CSM / DF シャドウを持つのは選択されたフォワードディレクショナルライト (b3)
        float3 lightDir = normalize(ForwardLightData.DirectionalLightDirection); // 受光面 -> ライト
        // 自己遮蔽オフセット: y=レイ方向 (ShadowBias 由来), z=法線方向 (SlopeBias 由来)
        float3 rayStart = WorldPos + N * DFShadowParams1.z + lightDir * DFShadowParams1.y;

        float df = RayTraceDistanceFieldShadow(rayStart, lightDir,
            DFShadowParams0.w, DFShadowParams0.z);

        float dfFade = saturate((dfDistance - ViewDepth) / max(dfDistance * 0.1f, 1e-4f));
        farShadow = lerp(1.0f, df, dfFade);
    }

    return lerp(farShadow, csm, csmFade);
}

// -------------------------------------------------------------
//  ディレクショナルライトの「薄い葉」用 透過シャドウ係数 (1 = 影なし)
//  CSM の遮蔽物深度から受光点までの距離 d [m] (光線方向) を求め、
//  影の中でも MaxTransmission * exp(-d / DepthScale) の透過を残す。
//    SurfaceShadow : GetDirectionalShadow の結果 (戻り値の下限)
//  CSM 範囲外 (DF / フェード帯) は SurfaceShadow をそのまま返す。
//  遮蔽物は一様な葉の媒質とみなす (UE の Subsurface Shadow と同じ近似)。
// -------------------------------------------------------------
float GetDirectionalFoliageTransmissionShadow(float3 WorldPos, float3 N, float ViewDepth,
    float SurfaceShadow, float MaxTransmission, float DepthScale)
{
    float result = SurfaceShadow;

    const uint numCascades = (uint) DirectionalShadowParams.x;
    const float shadowDistance = DirectionalShadowParams.y;
    const uint cascade = SelectShadowCascade(ViewDepth, CascadeSplits);

    [branch]
    if (SurfaceShadow < 1.0f && ViewDepth < shadowDistance && cascade < numCascades)
    {
        float3 offsetPos = WorldPos + N * CascadeNormalOffset[cascade];
        float4x4 worldToShadow = WorldToShadowCascade[cascade];
        float4 shadowClip = mul(float4(offsetPos, 1.0f), worldToShadow);
        float2 uv = shadowClip.xy * float2(0.5f, -0.5f) + 0.5f;
        float receiverDepth = saturate(shadowClip.z - CascadeDepthBias[cascade]);

        // NDC 深度 -> ワールド距離 (オルソなので z 列の長さが一定スケール)
        float depthPerMeter = max(length(float3(worldToShadow[0][2], worldToShadow[1][2], worldToShadow[2][2])), 1e-6f);

        // 3x3 テクセルの遮蔽物までの距離を平均 (遮蔽しているテクセルのみ)
        int2 center = int2(uv / DirectionalShadowParams.w);
        float distSum = 0.0f;
        float numOccluded = 0.0f;
        [unroll]
        for (int y = -1; y <= 1; ++y)
        {
            [unroll]
            for (int x = -1; x <= 1; ++x)
            {
                float occluderDepth = DirectionalShadowCascades.Load(int4(center + int2(x, y), cascade, 0));
                float delta = receiverDepth - occluderDepth;
                distSum += max(delta, 0.0f) / depthPerMeter;
                numOccluded += (delta > 0.0f) ? 1.0f : 0.0f;
            }
        }

        float meanDistance = distSum / max(numOccluded, 1.0f);
        float throughCrown = MaxTransmission * exp(-meanDistance / DepthScale);

        // CSM の距離フェード帯では元の係数へ戻す
        float csmFade = saturate((shadowDistance - ViewDepth) / max(shadowDistance - DirectionalShadowParams.z, 1e-4f));
        result = lerp(SurfaceShadow, lerp(throughCrown, 1.0f, SurfaceShadow), csmFade);
    }

    return result;
}

// -------------------------------------------------------------
//  ローカルライト (Point / Spot / Rect) のシャドウ係数 (1 = 影なし)
//  LightData と Shadow は同じインデックスの t13 / t16 要素を渡すこと。
// -------------------------------------------------------------
float GetLocalLightShadow(FDeferredLightData LightData, FLocalShadowParameters Shadow,
    float3 WorldPos, float3 N)
{
    // ---- DF シャドウ指定 (bUseRayTracedDistanceFieldShadows) ----
    // シャドウマップの代わりに受光点 -> ライトへメッシュ SDF をレイマーチ。
    // コーン幅は光源半径の見かけ角から決める。
    [branch]
    if (Shadow.DFShadow > 0.5f)
    {
        float3 toLight = LightData.WorldPosition - WorldPos;
        float distToLight = length(toLight);
        float3 dir = toLight / max(distToLight, 1e-4f);
        float tanCone = max(LightData.SourceRadius, 0.02f) / max(distToLight, 0.01f);
        // 自己遮蔽オフセットはライトごと (t16):
        //   NormalOffsetWorld = SlopeBias 由来 / DFSelfShadowBias = ShadowBias 由来
        float3 rayStart = WorldPos + N * Shadow.NormalOffsetWorld + dir * Shadow.DFSelfShadowBias;

        return RayTraceDistanceFieldShadow(rayStart, dir, distToLight, tanCone);
    }

    if (Shadow.ShadowSliceIndex < 0)
    {
        return 1.0f;
    }

    float3 offsetPos = WorldPos + N * Shadow.NormalOffsetWorld;

    return ProjectLocalLightShadowMap(LocalLightShadows, ShadowSampler, LightData, Shadow, offsetPos);
}

#endif
