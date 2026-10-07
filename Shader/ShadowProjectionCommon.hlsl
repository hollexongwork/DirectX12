#ifndef SHADOW_PROJECTION_COMMON_HLSL
#define SHADOW_PROJECTION_COMMON_HLSL

#include "Structs.hlsl"
#include "LightData.hlsl"

// =============================================================
//  ShadowProjectionCommon
//  シャドウ評価の「レジスタ非依存」部分。シャドウマップの投影 + 3x3 PCF を、
//  テクスチャ / 比較サンプラを引数で受け取る純関数として提供する。
//
//  利用側:
//    - ShadowFilteringCommon.hlsl : グラフィックスレジスタ
//      (t14 / t15 / s2, b5) を束ねた従来 API
//      (GetDirectionalShadow / GetLocalLightShadow)
//    - VolumetricFogCommon.hlsl   : Volumetric Fog のライト散乱
//      コンピュート (独立ルートシグネチャ) が froxel 中心の
//      直接光遮蔽をここで評価する
//
//  このファイルは cbuffer / register を一切宣言しない
//  (LightData.hlsl の FDeferredLightData と Structs.hlsl の
//   FLocalShadowParameters のみ参照)。
// =============================================================

// -------------------------------------------------------------
//  3x3 PCF (Texture2DArray + 比較サンプラ)
// -------------------------------------------------------------
float ShadowPCF3x3(Texture2DArray<float> ShadowMap, SamplerComparisonState CmpSampler,
    float Slice, float2 UV, float CompareDepth, float InvResolution)
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
            sum += ShadowMap.SampleCmpLevelZero(CmpSampler, float3(UV + offset, Slice), CompareDepth);
        }
    }
    return sum / 9.0f;
}

// -------------------------------------------------------------
//  CSM カスケード選択 (未使用スロットの split は 1e9)
//  戻り値はカスケードインデックス (>= NumCascades なら範囲外)
// -------------------------------------------------------------
uint SelectShadowCascade(float ViewDepth, float4 CascadeSplits)
{
    uint cascade = 0;
    cascade += (ViewDepth > CascadeSplits.x) ? 1 : 0;
    cascade += (ViewDepth > CascadeSplits.y) ? 1 : 0;
    cascade += (ViewDepth > CascadeSplits.z) ? 1 : 0;
    return cascade;
}

// -------------------------------------------------------------
//  ディレクショナル CSM: 1 カスケードへ投影して PCF
//    WorldToShadow : ワールド -> シャドウクリップ (転置済み。オルソなので w=1)
//    DepthBias     : 受光側深度バイアス (NDC)
//    OffsetPos     : 法線オフセット適用済みの受光点
// -------------------------------------------------------------
float ProjectDirectionalCascadeShadow(Texture2DArray<float> CSM, SamplerComparisonState CmpSampler,
    float4x4 WorldToShadow, float DepthBias, float InvResolution, uint Cascade, float3 OffsetPos)
{
    float4 shadowClip = mul(float4(OffsetPos, 1.0f), WorldToShadow);

    float2 uv = shadowClip.xy * float2(0.5f, -0.5f) + 0.5f;
    float compareDepth = shadowClip.z - DepthBias;

    return ShadowPCF3x3(CSM, CmpSampler, (float) Cascade, uv, compareDepth, InvResolution);
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

// ポイントライトのキューブ面ガードバンド [テクセル]
// (C++ 側 ShadowRendering.h の POINT_SHADOW_GUARD_TEXELS と 1:1)
static const float POINT_SHADOW_GUARD_TEXELS = 6.0f;

// -------------------------------------------------------------
//  ローカルライト (Point / Spot / Rect) のシャドウマップ投影 (1 = 影なし)
//  LightData と Shadow は同じインデックスの t13 / t16 要素を渡すこと。
//  OffsetPos は法線オフセット適用済みの受光点 (オフセット不要なら
//  受光点そのもの)。DF シャドウ (Shadow.DFShadow) と
//  ShadowSliceIndex < 0 の判定は呼び出し側の責務。
// -------------------------------------------------------------
float ProjectLocalLightShadowMap(Texture2DArray<float> Atlas, SamplerComparisonState CmpSampler,
    FDeferredLightData LightData, FLocalShadowParameters Shadow, float3 OffsetPos)
{
    // ポイントライト = スポットでもレクトでもないローカルライト
    [branch]
    if (!LightData.bSpotLight && !LightData.bRectLight)
    {
        // ---- ポイント: 支配軸から面選択 -> デバイス深度再構築 ----
        float3 d = OffsetPos - LightData.WorldPosition; // ライト -> 受光点
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
        float3 xaxis = cross(up, fwd); // 基底同士は直交単位なので正規化不要
        float3 yaxis = cross(fwd, xaxis);

        float zEye = max(axisDepth, Shadow.ShadowNearPlane + 1e-4f);

        // キューブ面ガードバンド: 描画側は 90 度よりわずかに広い FOV
        // (tan(fov/2) = 1/guardScale) なので、受光側 NDC を同率で縮める。
        float guardScale = 1.0f - 2.0f * POINT_SHADOW_GUARD_TEXELS * Shadow.InvShadowResolution;

        float2 ndc = float2(dot(d, xaxis), -dot(d, yaxis)) / zEye * guardScale;
        float2 uv = ndc * 0.5f + 0.5f;

        // 90 度透視 (PerspectiveFovLH) のデバイス深度:
        //   z_ndc = f/(f-n) - f*n / ((f-n) * zEye)
        float invRange = 1.0f / max(Shadow.ShadowFarPlane - Shadow.ShadowNearPlane, 1e-4f);
        float fRange = Shadow.ShadowFarPlane * invRange;
        float deviceZ = fRange - fRange * Shadow.ShadowNearPlane / zEye;

        float slice = (float) (Shadow.ShadowSliceIndex + (int) face);
        return ShadowPCF3x3(Atlas, CmpSampler, slice, uv, deviceZ - Shadow.DepthBiasNDC,
            Shadow.InvShadowResolution);
    }

    // ---- スポット / レクト: 単一透視投影 ----
    float4 shadowClip = mul(float4(OffsetPos, 1.0f), Shadow.WorldToShadow);
    if (shadowClip.w <= 1e-4f)
    {
        return 1.0f; // ライト背後 (投影の外) は影なし
    }
    shadowClip.xyz /= shadowClip.w;

    float2 uv = shadowClip.xy * float2(0.5f, -0.5f) + 0.5f;
    return ShadowPCF3x3(Atlas, CmpSampler, (float) Shadow.ShadowSliceIndex, uv,
        shadowClip.z - Shadow.DepthBiasNDC, Shadow.InvShadowResolution);
}

#endif
