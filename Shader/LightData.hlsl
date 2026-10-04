#ifndef LIGHT_DATA_HLSL
#define LIGHT_DATA_HLSL

// =============================================================
//  LightData
//  シェーダが扱うライトのデータ構造。
//
//    FLocalLightData      : ライトバッファ (ForwardLightBuffer) の 1 要素。
//                           C++ FForwardLocalLightData (LightGridInjection.h) と
//                           1:1 ミラー必須 (128 bytes)。
//    FDirectionalLightData: 選択されたフォワードディレクショナルライト
//                           (b3 ForwardLightData の DirectionalLight* フィールド)。
//    FDeferredLightData   : ライト 1 灯を評価する関数群
//                           (DeferredLightingCommon.hlsl) が受け取る展開済みの形。
//
//  このファイルは cbuffer / register を宣言しない。グラフィックス
//  (LightGridCommon.hlsl の GetLocalLightData / GetDirectionalLightData) と
//  コンピュート (Lumen / Volumetric Fog / ライトグリッド構築) が共有する。
//
//  規約:
//    - 位置はワールド空間 [m]
//    - Direction は受光点からライトへ向かう方向 (発光方向の逆)
//    - Tangent はライトの上方向 (レクトライトの高さ軸 / チューブの軸)
//    - 逆二乗フォールオフは FalloffExponent == 0 で表す
// =============================================================

// ---- ライト種別 (C++ ELightComponentType と 1:1) ----
#define LIGHT_TYPE_DIRECTIONAL 0
#define LIGHT_TYPE_POINT       1
#define LIGHT_TYPE_SPOT        2
#define LIGHT_TYPE_RECT        3
#define LIGHT_TYPE_MAX         4

// ---- ライトフラグ (C++ LIGHT_FLAG_* と 1:1) ----
#define LIGHT_FLAG_CAST_DYNAMIC_SHADOW         (1u << 0) // 動的シャドウを落とす (ShadowedBits)
#define LIGHT_FLAG_AFFECT_TRANSLUCENT_LIGHTING (1u << 1) // 半透明を照らす
#define LIGHT_FLAG_CAST_VOLUMETRIC_SHADOW      (1u << 2) // Volumetric Fog の中で影を落とす

// レクトライト固有のデータ (FRectLightData)
struct FRectLightData
{
    float BarnCosAngle; // バーンドア開き角の cos
    float BarnLength; // バーンドアの長さ [m]
};

// -------------------------------------------------------------
//  FDeferredLightData
//  ライト 1 灯の展開済みパラメータ。
// -------------------------------------------------------------
struct FDeferredLightData
{
    float3 WorldPosition; // ワールド位置 [m]
    float InvRadius; // 1 / AttenuationRadius
    float3 Color; // 線形色 x 明るさ (距離フェード適用済み)
    float FalloffExponent;
    float3 Direction; // 受光点 -> ライト方向 (ディレクショナルライトではそのまま L)
    float3 Tangent; // ライトの上方向
    float SoftSourceRadius;
    float2 SpotAngles; // x = cos(Outer), y = 1 / (cos(Inner) - cos(Outer))
    float SourceRadius; // 球光源の半径 (レクトライトは半幅 / ディレクショナルライトは sin(半角))
    float SourceLength; // チューブの長さ (レクトライトは半高)
    float SpecularScale;
    float DiffuseScale;
    float ContactShadowLength; // 0 = コンタクトシャドウ無し
    float ContactShadowCastingIntensity;
    float ContactShadowNonCastingIntensity;
    bool ContactShadowLengthInWS; // 長さがワールド空間 [m] か (偽ならスクリーン空間)
    bool bInverseSquared; // 逆二乗フォールオフか
    bool bRadialLight; // 減衰半径を持つライト (Point / Spot / Rect) か
    bool bSpotLight;
    bool bRectLight;
    uint ShadowedBits; // 0 = 影無し、3 = 動的シャドウあり
    FRectLightData RectLightData;
    bool bAffectsTranslucentLighting;
    // ---- Volumetric Fog 用 ----
    float VolumetricScatteringIntensity; // Volumetric Fog への散乱寄与
    bool bCastVolumetricShadow; // Volumetric Fog の中で影を落とすか
};

// -------------------------------------------------------------
//  FLocalLightData
//  ライトバッファの 1 要素 (逐次パック 128 bytes)。
//  [0, NumLocalLights) がローカルライト、続く NumDirectionalLights 個が
//  ディレクショナルライト。C++ 側は FForwardLocalLightData。
// -------------------------------------------------------------
struct FLocalLightData
{
    float4 LightPositionAndInvRadius; // xyz = ワールド位置, w = 1 / AttenuationRadius
    float4 LightColorAndFalloffExponent; // rgb = 色 x 明るさ, w = FalloffExponent (0 = 逆二乗)
    float4 LightDirectionAndSpecularScale; // xyz = Direction, w = SpecularScale
    float4 SpotAnglesAndSourceRadiusPacked; // xy = SpotAngles, z = SourceRadius, w = SourceLength
    float4 LightTangentAndSoftSourceRadius; // xyz = Tangent, w = SoftSourceRadius
    float4 RectBarnDoorAndScales; // x = BarnCosAngle, y = BarnLength, z = DiffuseScale, w = VolumetricScatteringIntensity
    float4 ContactShadowParams; // x = ContactShadowLength (負 = ワールド空間), y = CastingIntensity, z = NonCastingIntensity, w = 予約
    uint LightType; // LIGHT_TYPE_*
    uint Flags; // LIGHT_FLAG_*
    uint Pad0;
    uint Pad1;
};

// -------------------------------------------------------------
//  FDirectionalLightData
//  フォワードシェーディング (半透明 / Volumetric Fog) が使う
//  「選択された 1 灯」のディレクショナルライト。
// -------------------------------------------------------------
struct FDirectionalLightData
{
    uint HasDirectionalLight;
    float3 DirectionalLightColor;
    float DirectionalLightVolumetricScatteringIntensity;
    float3 DirectionalLightDirection; // 受光点 -> ライト方向
    float DirectionalLightSourceRadius; // sin(見かけの半角)
    float DirectionalLightSoftSourceRadius;
    float DirectionalLightSpecularScale;
    float DirectionalLightDiffuseScale;
    uint DirectionalLightFlags; // LIGHT_FLAG_*
};

// ライトバッファの要素 -> FDeferredLightData (ConvertToDeferredLight)
FDeferredLightData ConvertToDeferredLight(FLocalLightData In)
{
    FDeferredLightData Out = (FDeferredLightData) 0;

    Out.WorldPosition = In.LightPositionAndInvRadius.xyz;
    Out.InvRadius = In.LightPositionAndInvRadius.w;
    Out.Color = In.LightColorAndFalloffExponent.xyz;
    Out.FalloffExponent = In.LightColorAndFalloffExponent.w;
    Out.Direction = In.LightDirectionAndSpecularScale.xyz;
    Out.SpecularScale = In.LightDirectionAndSpecularScale.w;
    Out.SpotAngles = In.SpotAnglesAndSourceRadiusPacked.xy;
    Out.SourceRadius = In.SpotAnglesAndSourceRadiusPacked.z;
    Out.SourceLength = In.SpotAnglesAndSourceRadiusPacked.w;
    Out.Tangent = In.LightTangentAndSoftSourceRadius.xyz;
    Out.SoftSourceRadius = In.LightTangentAndSoftSourceRadius.w;
    Out.RectLightData.BarnCosAngle = In.RectBarnDoorAndScales.x;
    Out.RectLightData.BarnLength = In.RectBarnDoorAndScales.y;
    Out.DiffuseScale = In.RectBarnDoorAndScales.z;
    Out.VolumetricScatteringIntensity = In.RectBarnDoorAndScales.w;

    // 符号がワールド空間 / スクリーン空間を表す
    Out.ContactShadowLength = abs(In.ContactShadowParams.x);
    Out.ContactShadowLengthInWS = In.ContactShadowParams.x < 0.0f;
    Out.ContactShadowCastingIntensity = In.ContactShadowParams.y;
    Out.ContactShadowNonCastingIntensity = In.ContactShadowParams.z;

    Out.bRadialLight = In.LightType != LIGHT_TYPE_DIRECTIONAL;
    // ディレクショナルライトは逆二乗減衰を使わない
    Out.bInverseSquared = Out.bRadialLight && Out.FalloffExponent == 0.0f;
    Out.bSpotLight = In.LightType == LIGHT_TYPE_SPOT;
    Out.bRectLight = In.LightType == LIGHT_TYPE_RECT;

    Out.ShadowedBits = (In.Flags & LIGHT_FLAG_CAST_DYNAMIC_SHADOW) ? 3u : 0u;
    Out.bAffectsTranslucentLighting = (In.Flags & LIGHT_FLAG_AFFECT_TRANSLUCENT_LIGHTING) != 0u;
    Out.bCastVolumetricShadow = (In.Flags & LIGHT_FLAG_CAST_VOLUMETRIC_SHADOW) != 0u;

    return Out;
}

// フォワードディレクショナルライト -> FDeferredLightData (ConvertToDeferredLight)
FDeferredLightData ConvertToDeferredLight(FDirectionalLightData In)
{
    FDeferredLightData Out = (FDeferredLightData) 0;

    Out.Color = In.DirectionalLightColor;
    Out.FalloffExponent = 0.0f;
    Out.Direction = In.DirectionalLightDirection;
    Out.Tangent = In.DirectionalLightDirection;
    Out.SpotAngles = float2(0.0f, 0.0f);
    Out.SourceRadius = In.DirectionalLightSourceRadius;
    Out.SoftSourceRadius = In.DirectionalLightSoftSourceRadius;
    Out.SourceLength = 0.0f;
    Out.SpecularScale = In.DirectionalLightSpecularScale;
    Out.DiffuseScale = In.DirectionalLightDiffuseScale;
    Out.VolumetricScatteringIntensity = In.DirectionalLightVolumetricScatteringIntensity;

    Out.bRadialLight = false;
    Out.bInverseSquared = false;
    Out.bSpotLight = false;
    Out.bRectLight = false;

    Out.ShadowedBits = (In.DirectionalLightFlags & LIGHT_FLAG_CAST_DYNAMIC_SHADOW) ? 3u : 0u;
    Out.bAffectsTranslucentLighting = (In.DirectionalLightFlags & LIGHT_FLAG_AFFECT_TRANSLUCENT_LIGHTING) != 0u;
    Out.bCastVolumetricShadow = (In.DirectionalLightFlags & LIGHT_FLAG_CAST_VOLUMETRIC_SHADOW) != 0u;

    return Out;
}

#endif
