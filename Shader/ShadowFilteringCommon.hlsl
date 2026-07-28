#ifndef SHADOW_FILTERING_COMMON_HLSL
#define SHADOW_FILTERING_COMMON_HLSL

#include "Common.hlsl"
#include "DistanceFieldShadowing.hlsl"

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
// =============================================================

// -------------------------------------------------------------
//  3x3 PCF (Texture2DArray + 比較サンプラ)
// -------------------------------------------------------------
float ShadowPCF3x3(Texture2DArray<float> ShadowMap, float Slice, float2 UV, float CompareDepth, float InvResolution)
{
    // 深度レンジ外 (far 越え) が境界白と誤比較しないようクランプ
    CompareDepth = saturate(CompareDepth);

    float sum = 0.0f;
    [unroll]
    for (int y = -1; y <= 1; ++y)
    {
        [unroll]
        for (int x = -1; x <= 1; ++x)
        {
            float2 offset = float2(x, y) * InvResolution;
            sum += ShadowMap.SampleCmpLevelZero(ShadowSampler, float3(UV + offset, Slice), CompareDepth);
        }
    }
    return sum / 9.0f;
}

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
        uint cascade = 0;
        cascade += (ViewDepth > CascadeSplits.x) ? 1 : 0;
        cascade += (ViewDepth > CascadeSplits.y) ? 1 : 0;
        cascade += (ViewDepth > CascadeSplits.z) ? 1 : 0;

        if (cascade < numCascades)
        {
            // 法線オフセット + シャドウクリップへ投影 (オルソなので w=1)
            float3 offsetPos = WorldPos + N * CascadeNormalOffset[cascade];
            float4 shadowClip = mul(float4(offsetPos, 1.0f), WorldToShadowCascade[cascade]);

            float2 uv = shadowClip.xy * float2(0.5f, -0.5f) + 0.5f;
            float compareDepth = shadowClip.z - CascadeDepthBias[cascade];

            csm = ShadowPCF3x3(DirectionalShadowCascades, (float) cascade, uv, compareDepth,
                DirectionalShadowParams.w);

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
        float3 lightDir = normalize(DirectionalLightDirection.xyz); // 受光面 -> ライト
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
//  キューブ 6 面の基底 (C++ 側 ShadowRendering.cpp と 1:1 必須)
//  面順: +X, -X, +Y, -Y, +Z, -Z
// -------------------------------------------------------------
static const float3 CubeFaceForward[6] =
{
    float3(1.0f, 0.0f, 0.0f), float3(-1.0f, 0.0f, 0.0f),
    float3(0.0f, 1.0f, 0.0f), float3(0.0f, -1.0f, 0.0f),
    float3(0.0f, 0.0f, 1.0f), float3(0.0f, 0.0f, -1.0f),
};
static const float3 CubeFaceUp[6] =
{
    float3(0.0f, 1.0f, 0.0f), float3(0.0f, 1.0f, 0.0f),
    float3(0.0f, 0.0f, -1.0f), float3(0.0f, 0.0f, 1.0f),
    float3(0.0f, 1.0f, 0.0f), float3(0.0f, 1.0f, 0.0f),
};

// -------------------------------------------------------------
//  ローカルライト (Point / Spot / Rect) のシャドウ係数 (1 = 影なし)
//  Light と Shadow は同じインデックスの t13 / t16 要素を渡すこと。
// -------------------------------------------------------------
float GetLocalLightShadow(FLightShaderParameters Light, FLocalShadowParameters Shadow,
    float3 WorldPos, float3 N)
{
    // ---- DF シャドウ指定 (bUseRayTracedDistanceFieldShadows) ----
    // シャドウマップの代わりに受光点 -> ライトへメッシュ SDF をレイマーチ。
    // コーン幅は光源半径の見かけ角から決める。
    [branch]
    if (Shadow.DFShadow > 0.5f)
    {
        float3 toLight = Light.Position - WorldPos;
        float distToLight = length(toLight);
        float3 dir = toLight / max(distToLight, 1e-4f);
        float tanCone = max(Light.SourceRadius, 0.02f) / max(distToLight, 0.01f);
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

    [branch]
    if (Light.Type == LIGHT_TYPE_POINT)
    {
        // ---- ポイント: 支配軸から面選択 -> デバイス深度再構築 ----
        float3 d = offsetPos - Light.Position;    // ライト -> 受光点
        float3 ad = abs(d);

        uint face;
        float axisDepth;
        if (ad.x >= ad.y && ad.x >= ad.z)
        {
            face = (d.x > 0.0f) ? 0 : 1;
            axisDepth = ad.x;
        }
        else if (ad.y >= ad.z)
        {
            face = (d.y > 0.0f) ? 2 : 3;
            axisDepth = ad.y;
        }
        else
        {
            face = (d.z > 0.0f) ? 4 : 5;
            axisDepth = ad.z;
        }

        // LookToLH と同じ基底構成 (xaxis = cross(up, fwd), yaxis = cross(fwd, xaxis))
        float3 fwd = CubeFaceForward[face];
        float3 up = CubeFaceUp[face];
        float3 xaxis = cross(up, fwd);      // 基底同士は直交単位なので正規化不要
        float3 yaxis = cross(fwd, xaxis);

        float zEye = max(axisDepth, Shadow.ShadowNearPlane + 1e-4f);
        
        // キューブ面ガードバンド: 描画側は 90 度よりわずかに広い FOV
        // (tan(fov/2) = 1/guardScale) なので、受光側 NDC を同率で縮める。
        // C++ 側 (ShadowRendering.h) の POINT_SHADOW_GUARD_TEXELS と 1:1
        const float POINT_SHADOW_GUARD_TEXELS = 6.0f;
        float guardScale = 1.0f - 2.0f * POINT_SHADOW_GUARD_TEXELS * Shadow.InvShadowResolution;

        float2 ndc = float2(dot(d, xaxis), -dot(d, yaxis)) / zEye * guardScale;
        float2 uv = ndc * 0.5f + 0.5f;

        // 90 度透視 (PerspectiveFovLH) のデバイス深度:
        //   z_ndc = f/(f-n) - f*n / ((f-n) * zEye)
        float invRange = 1.0f / max(Shadow.ShadowFarPlane - Shadow.ShadowNearPlane, 1e-4f);
        float fRange = Shadow.ShadowFarPlane * invRange;
        float deviceZ = fRange - fRange * Shadow.ShadowNearPlane / zEye;

        float slice = (float) (Shadow.ShadowSliceIndex + (int) face);
        return ShadowPCF3x3(LocalLightShadows, slice, uv, deviceZ - Shadow.DepthBiasNDC,
            Shadow.InvShadowResolution);
    }

    // ---- スポット / レクト: 単一透視投影 ----
    float4 shadowClip = mul(float4(offsetPos, 1.0f), Shadow.WorldToShadow);
    if (shadowClip.w <= 1e-4f)
    {
        return 1.0f;    // ライト背後 (投影の外) は影なし
    }
    shadowClip.xyz /= shadowClip.w;

    float2 uv = shadowClip.xy * float2(0.5f, -0.5f) + 0.5f;
    return ShadowPCF3x3(LocalLightShadows, (float) Shadow.ShadowSliceIndex, uv,
        shadowClip.z - Shadow.DepthBiasNDC, Shadow.InvShadowResolution);
}

#endif
