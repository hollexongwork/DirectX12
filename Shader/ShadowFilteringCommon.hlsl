#ifndef SHADOW_FILTERING_COMMON_HLSL
#define SHADOW_FILTERING_COMMON_HLSL

#include "Common.hlsl"
#include "DistanceFieldShadowing.hlsl"
#include "ShadowProjectionCommon.hlsl"

// =============================================================
//  ShadowFilteringCommon
//  ShadowProjectionCommon.ush / ShadowFilteringCommon.ush に相当する
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
