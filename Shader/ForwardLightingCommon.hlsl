#ifndef FORWARD_LIGHTING_COMMON_HLSL
#define FORWARD_LIGHTING_COMMON_HLSL

#include "DeferredLightingCommon.hlsl"
#include "SubstrateEvaluation.hlsl"
#include "ShadowFilteringCommon.hlsl"
#include "LightGridCommon.hlsl"

// =============================================================
//  ForwardLightingCommon
//  1 ピクセルに届く全ライトの直接光を、ライトグリッドのセルを巡回して合計する。
//  デファードライティング (DeferredPS) とフォワード半透明 (TranslucentPS) が共有する。
//
//    ディレクショナルライト:
//      デファード (bForwardShading = false) : ライトバッファの全ディレクショナルライト。
//          CSM / Distance Field シャドウは「選択された 1 灯」
//          (ForwardLightData.DirectionalLightBufferIndex) だけに掛かる
//      フォワード (bForwardShading = true)  : 選択された 1 灯だけ。
//          bAffectTranslucentLighting が偽のライトは照らさない
//    ローカルライト:
//      ライトグリッドのセル (無効時は全灯) を巡回し、ライトごとにシャドウマップ /
//      Distance Field シャドウをインラインで評価する。
//      インデックスはライトバッファ (t13) とローカルシャドウパラメータ (t16) で共通。
//
//  BxDF はレガシー (FGBufferData -> GetDynamicLighting) と
//  Substrate Slab (FSubstrateBSDF -> SubstrateDeferredLighting) の 2 系統。
// =============================================================

// -------------------------------------------------------------
//  レガシー経路の直接光
//    GridIndex         : このピクセルのライトグリッドセル
//    CameraVector      : カメラ -> 受光点 (正規化)
//    DirectionalShadow : 選択されたディレクショナルライトのシャドウ係数 (CSM + DF)
//    Dither            : コンタクトシャドウのディザ (0..1)
// -------------------------------------------------------------
float3 GetForwardDirectLighting(
    uint GridIndex, float3 WorldPosition, float3 CameraVector, FGBufferData GBuffer,
    float DirectionalShadow, float Dither, bool bForwardShading)
{
    float3 DirectLighting = float3(0.0f, 0.0f, 0.0f);
    float SurfaceShadow = 1.0f;

    // ---- ディレクショナルライト ----
    [branch]
    if (bForwardShading)
    {
        const FDirectionalLightData DirectionalLightData = GetDirectionalLightData();

        [branch]
        if (DirectionalLightData.HasDirectionalLight)
        {
            FDeferredLightData LightData = ConvertToDeferredLight(DirectionalLightData);

            [branch]
            if (LightData.bAffectsTranslucentLighting)
            {
                DirectLighting += GetDynamicLighting(WorldPosition, CameraVector, GBuffer, 1.0f, LightData, DirectionalShadow, Dither, SurfaceShadow);
            }
        }
    }
    else
    {
        for (uint DirectionalIndex = 0; DirectionalIndex < ForwardLightData.NumDirectionalLights; ++DirectionalIndex)
        {
            const uint LightIndex = ForwardLightData.NumLocalLights + DirectionalIndex;
            FDeferredLightData LightData = ConvertToDeferredLight(GetLocalLightData(LightIndex));

            // CSM / Distance Field シャドウを持つのは選択された 1 灯だけ
            float LightAttenuation = (LightIndex == ForwardLightData.DirectionalLightBufferIndex) ? DirectionalShadow : 1.0f;

            DirectLighting += GetDynamicLighting(WorldPosition, CameraVector, GBuffer, 1.0f, LightData, LightAttenuation, Dither, SurfaceShadow);
        }
    }

    // ---- ローカルライト (Point / Spot / Rect) ----
    // グリッド有効: このピクセルのセルに入っているライトだけ巡回する。
    // グリッド無効: 全灯ループ (フォールバック)。
    uint NumLights = ForwardLightData.NumLocalLights;
    uint DataStartIndex = 0u;

    [branch]
    if (ForwardLightData.bUseLightGrid != 0u)
    {
        const FCulledLightsGridHeader GridHeader = GetCulledLightsGridHeader(GridIndex);
        NumLights = GridHeader.NumLights;
        DataStartIndex = GridHeader.DataStartIndex;
    }

    for (uint i = 0; i < NumLights; ++i)
    {
        uint LightIndex = i;

        [branch]
        if (ForwardLightData.bUseLightGrid != 0u)
        {
            LightIndex = GetCulledLightDataGrid(DataStartIndex + i);
        }

        FDeferredLightData LightData = ConvertToDeferredLight(GetLocalLightData(LightIndex));

        [branch]
        if (bForwardShading && !LightData.bAffectsTranslucentLighting)
        {
            continue;
        }

        // ローカルシャドウ (t16 はライトバッファ t13 と同じインデックスで 1:1)
        float LightAttenuation = GetLocalLightShadow(LightData, LocalShadowParams[LightIndex], WorldPosition, GBuffer.WorldNormal);

        DirectLighting += GetDynamicLighting(WorldPosition, CameraVector, GBuffer, 1.0f, LightData, LightAttenuation, Dither, SurfaceShadow);
    }

    return DirectLighting;
}

// -------------------------------------------------------------
//  Substrate Slab 経路の直接光 (引数はレガシー経路と同じ意味)
//    N          : シェーディング法線
//    SceneDepth : 受光点のビュー空間 Z [m]
// -------------------------------------------------------------
float3 GetForwardDirectLightingSubstrate(
    uint GridIndex, float3 WorldPosition, float3 CameraVector, FSubstrateBSDF BSDF, float3 N, float SceneDepth,
    float DirectionalShadow, float Dither, bool bForwardShading)
{
    float3 DirectLighting = float3(0.0f, 0.0f, 0.0f);

    // ---- ディレクショナルライト ----
    [branch]
    if (bForwardShading)
    {
        const FDirectionalLightData DirectionalLightData = GetDirectionalLightData();

        [branch]
        if (DirectionalLightData.HasDirectionalLight)
        {
            FDeferredLightData LightData = ConvertToDeferredLight(DirectionalLightData);

            [branch]
            if (LightData.bAffectsTranslucentLighting)
            {
                DirectLighting += SubstrateDeferredLighting(BSDF, WorldPosition, CameraVector, N, SceneDepth, LightData, DirectionalShadow, Dither);
            }
        }
    }
    else
    {
        for (uint DirectionalIndex = 0; DirectionalIndex < ForwardLightData.NumDirectionalLights; ++DirectionalIndex)
        {
            const uint LightIndex = ForwardLightData.NumLocalLights + DirectionalIndex;
            FDeferredLightData LightData = ConvertToDeferredLight(GetLocalLightData(LightIndex));

            float LightAttenuation = (LightIndex == ForwardLightData.DirectionalLightBufferIndex) ? DirectionalShadow : 1.0f;

            DirectLighting += SubstrateDeferredLighting(BSDF, WorldPosition, CameraVector, N, SceneDepth, LightData, LightAttenuation, Dither);
        }
    }

    // ---- ローカルライト (Point / Spot / Rect) ----
    uint NumLights = ForwardLightData.NumLocalLights;
    uint DataStartIndex = 0u;

    [branch]
    if (ForwardLightData.bUseLightGrid != 0u)
    {
        const FCulledLightsGridHeader GridHeader = GetCulledLightsGridHeader(GridIndex);
        NumLights = GridHeader.NumLights;
        DataStartIndex = GridHeader.DataStartIndex;
    }

    for (uint i = 0; i < NumLights; ++i)
    {
        uint LightIndex = i;

        [branch]
        if (ForwardLightData.bUseLightGrid != 0u)
        {
            LightIndex = GetCulledLightDataGrid(DataStartIndex + i);
        }

        FDeferredLightData LightData = ConvertToDeferredLight(GetLocalLightData(LightIndex));

        [branch]
        if (bForwardShading && !LightData.bAffectsTranslucentLighting)
        {
            continue;
        }

        float LightAttenuation = GetLocalLightShadow(LightData, LocalShadowParams[LightIndex], WorldPosition, N);

        DirectLighting += SubstrateDeferredLighting(BSDF, WorldPosition, CameraVector, N, SceneDepth, LightData, LightAttenuation, Dither);
    }

    return DirectLighting;
}

#endif
