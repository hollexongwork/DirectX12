#pragma once
#include <DirectXMath.h>
#include "SceneComponent.h"
#include "LightSceneProxy.h"

using namespace DirectX;

// ============================================================
//  ライトコンポーネント階層
//
//    USceneComponent
//      └ ULightComponentBase					 … 強度 / 色 / AffectsWorld
//          └ ULightComponent					 … 色温度 / SpecularScale / FScene 登録 / プロキシ
//              ├ UDirectionalLightComponent	    (強度 = lux)
//              └ ULocalLightComponent          … AttenuationRadius / IntensityUnits
//                  ├ UPointLightComponent      … SourceRadius / SourceLength / 逆二乗
//                  │   └ USpotLightComponent  … Inner / OuterConeAngle
//                  └ URectLightComponent       … SourceWidth / Height / BarnDoor
//
//  結合は UPrimitiveComponent と同じ一方向データフロー (プッシュ型):
//    OnRegister -> FScene::AddLight -> CreateLightSceneProxy()
//    プロパティ変更 -> MarkRenderStateDirty -> ダーティリスト経由で
//                     次フレームにプロキシ再生成
//    トランスフォーム変更 -> MarkRenderTransformDirty -> ダーティリスト
//                     経由で次フレームに SendRenderTransform
//
//  ※ 距離の単位はメートル
//  ※ 発光方向はコンポーネント +Z 。
//	※ 幅軸 (Rect の幅 / チューブの軸) は +X。
// ============================================================

// ELightUnits に相当。ローカルライト (Point/Spot/Rect) の
// Intensity の物理単位。逆二乗フォールオフ有効時のみ意味を持つ。
enum class ELightUnits : int
{
	Unitless = 0,	// スケール (1 unitless = 1/625 cd)
	Candelas,		// cd (= lm/sr)。1000 cd の光は 1m の距離で 1000 lux
	Lumens,			// lm。光の立体角で除して cd 化 (Point=4π, Spot=2π(1-cosθ), Rect=π)
	EV,				// 2^Intensity cd
};

// ============================================================
//  ULightComponentBase
// ============================================================
class ULightComponentBase : public USceneComponent
{
protected:
	float    m_Intensity = 3.1415926535897932f;			// ULightComponentBase 既定値 (π)
	XMFLOAT4 m_LightColor = { 1.0f, 1.0f, 1.0f, 1.0f };	// 線形色
	bool     m_bAffectsWorld = true;					// false でシーンに寄与しない
	bool     m_bCastShadows = true;						// シャドウマップを落とすか
	float    m_ShadowBias = 0.5f;						// 受光側深度バイアス (既定 0.5)
	float    m_ShadowSlopeBias = 0.5f;					// 法線オフセット (スロープバイアス相当)

	// シャドウマップの代わりにメッシュ SDF をレイマーチして影を評価する
	// (bUseRayTracedDistanceFieldShadows)。CastShadows が前提。
	bool     m_bUseRayTracedDistanceFieldShadows = false;

	// レンダーステート変更フラグ。立っている = FScene の
	// レンダーステートダーティリストにエンキュー済み (登録中のみ)。
	// 次の FScene::UpdateAllLightSceneInfos でプロキシが再生成される。
	bool m_RenderStateDirty = false;

public:
	// ---- レンダーステート更新 (MarkRenderStateDirty) ----
	// 基底はフラグのみ。ULightComponent がオーバーライドして
	// FScene のダーティリストへ自分を積む (プッシュ型更新)。
	virtual void MarkRenderStateDirty() { m_RenderStateDirty = true; }
	bool IsRenderStateDirty() const { return m_RenderStateDirty; }
	void ClearRenderStateDirty() { m_RenderStateDirty = false; }

	// ---- プロパティ (セッターは全てプロキシ再生成をトリガー) ----
	void  SetIntensity(float Intensity) { m_Intensity = Intensity; MarkRenderStateDirty(); }
	float GetIntensity() const { return m_Intensity; }

	void            SetLightColor(const XMFLOAT4& Color) { m_LightColor = Color; MarkRenderStateDirty(); }
	const XMFLOAT4& GetLightColor() const { return m_LightColor; }

	void SetAffectsWorld(bool bAffectsWorld) { m_bAffectsWorld = bAffectsWorld; MarkRenderStateDirty(); }
	bool GetAffectsWorld() const { return m_bAffectsWorld; }

	void SetCastShadows(bool bCastShadows) { m_bCastShadows = bCastShadows; MarkRenderStateDirty(); }
	bool GetCastShadows() const { return m_bCastShadows; }

	void  SetShadowBias(float ShadowBias) { m_ShadowBias = ShadowBias; MarkRenderStateDirty(); }
	float GetShadowBias() const { return m_ShadowBias; }

	void  SetShadowSlopeBias(float ShadowSlopeBias) { m_ShadowSlopeBias = ShadowSlopeBias; MarkRenderStateDirty(); }
	float GetShadowSlopeBias() const { return m_ShadowSlopeBias; }

	void SetUseRayTracedDistanceFieldShadows(bool bUse) { m_bUseRayTracedDistanceFieldShadows = bUse; MarkRenderStateDirty(); }
	bool GetUseRayTracedDistanceFieldShadows() const { return m_bUseRayTracedDistanceFieldShadows; }

	// 発光方向 (正規化不要) を向くピッチ/ヨー/ロール (ラジアン) を返す。
	// エンジン前方 +Z 規約 (FRotationMatrix::MakeFromX 相当)。
	static XMFLOAT3 DirectionToRotator(const XMFLOAT3& Direction);
};

// ============================================================
//  ULightComponent
//  FScene への登録とレンダー側ミラー (FLightSceneProxy) の生成を担う。
// ============================================================
class ULightComponent : public ULightComponentBase
{
protected:
	bool  m_bUseTemperature = false;
	float m_Temperature = 6500.0f;		// ケルビン (1500..15000)。6500K ≒ 白
	float m_SpecularScale = 1.0f;		// スペキュラハイライトのみのスケール

	// レンダー側ミラー (所有は FScene::FLightSceneInfo。ここはキャッシュ)
	FLightSceneProxy* m_SceneProxy = nullptr;

public:
	void OnRegister() override;		// FScene::AddLight
	void OnUnregister() override;	// FScene::RemoveLight

	// レンダー側ミラーの生成 (ULightComponent::CreateSceneProxy)
	virtual FLightSceneProxy* CreateLightSceneProxy() const = 0;
	virtual ELightType GetLightType() const = 0;

	FLightSceneProxy* GetSceneProxy() const { return m_SceneProxy; }
	void SetSceneProxy(FLightSceneProxy* Proxy) { m_SceneProxy = Proxy; }

	// ---- ダーティ通知 (プッシュ型更新) ----
	// フラグを立て、登録済みなら FScene のダーティリストへ自分を積む
	// (フラグが既に立っていれば積まない = 二重登録防止)
	void MarkRenderStateDirty() override;
	void MarkRenderTransformDirty() override;

	// トランスフォーム (位置 / 発光方向 / 幅軸) をプロキシへプッシュ
	// (SendRenderTransform)
	void SendRenderTransform();

	// ---- プロパティ ----
	void SetUseTemperature(bool bUseTemperature) { m_bUseTemperature = bUseTemperature; MarkRenderStateDirty(); }
	bool GetUseTemperature() const { return m_bUseTemperature; }

	void  SetTemperature(float TemperatureKelvin) { m_Temperature = TemperatureKelvin; MarkRenderStateDirty(); }
	float GetTemperature() const { return m_Temperature; }

	void  SetSpecularScale(float SpecularScale) { m_SpecularScale = SpecularScale; MarkRenderStateDirty(); }
	float GetSpecularScale() const { return m_SpecularScale; }

	// 色温度・単位換算込みの最終線形色 (GetColoredLightBrightness)
	XMFLOAT3 GetColoredLightBrightness() const;

	// 単位換算後の強度 (ComputeLightBrightness)。基底は Intensity 素通し
	virtual float ComputeLightBrightness() const { return m_Intensity; }

	// Planckian locus による色温度 -> リニア sRGB 変換
	// (FLinearColor::MakeFromColorTemperature の移植)
	static XMFLOAT3 ColorTemperatureToRGB(float TemperatureKelvin);
};

// ============================================================
//  UDirectionalLightComponent
//  強度は lux (照度)。減衰なし。ENV 定数 (b0) 経由で GPU へ渡される。
// ============================================================
class UDirectionalLightComponent : public ULightComponent
{
protected:
	// ---- CSM (DynamicShadowDistanceMovableLight / DynamicShadowCascades /
	//      CascadeDistributionExponent 相当) ----
	float m_DynamicShadowDistance = 100.0f;			// CSM カバー距離 [m]
	int   m_DynamicShadowCascades = 4;				// カスケード数 (1..MAX_SHADOW_CASCADES)
	float m_CascadeDistributionExponent = 3.0f;		// 分割の指数 (大きいほど手前が細かい)
	float m_ShadowDistanceFadeoutFraction = 0.1f;	// 遠端フェード比率

	// ---- Distance Field Shadows (DistanceFieldShadowDistance /
	//      DistanceField Trace Distance / LightSourceAngle 相当) ----
	float m_DistanceFieldShadowDistance = 300.0f;	// DF シャドウのカバー距離 [m]
	float m_DistanceFieldTraceDistance = 100.0f;	// 1 レイの最大トレース距離 [m]
	float m_LightSourceAngle = 1.0f;				// 光源の見かけ全角 [度] (ソフトネス)

public:
	UDirectionalLightComponent()
	{
		m_Intensity = 10.0f;	// 既定: 10 lux
	}

	ELightType GetLightType() const override { return ELightType::Directional; }
	FLightSceneProxy* CreateLightSceneProxy() const override;

	// ---- CSM プロパティ ----
	void  SetDynamicShadowDistance(float Distance) { m_DynamicShadowDistance = Distance; MarkRenderStateDirty(); }
	float GetDynamicShadowDistance() const { return m_DynamicShadowDistance; }

	void SetDynamicShadowCascades(int Cascades) { m_DynamicShadowCascades = Cascades; MarkRenderStateDirty(); }
	int  GetDynamicShadowCascades() const { return m_DynamicShadowCascades; }

	void  SetCascadeDistributionExponent(float Exponent) { m_CascadeDistributionExponent = Exponent; MarkRenderStateDirty(); }
	float GetCascadeDistributionExponent() const { return m_CascadeDistributionExponent; }

	void  SetShadowDistanceFadeoutFraction(float Fraction) { m_ShadowDistanceFadeoutFraction = Fraction; MarkRenderStateDirty(); }
	float GetShadowDistanceFadeoutFraction() const { return m_ShadowDistanceFadeoutFraction; }

	// ---- Distance Field Shadows プロパティ ----
	void  SetDistanceFieldShadowDistance(float Distance) { m_DistanceFieldShadowDistance = Distance; MarkRenderStateDirty(); }
	float GetDistanceFieldShadowDistance() const { return m_DistanceFieldShadowDistance; }

	void  SetDistanceFieldTraceDistance(float Distance) { m_DistanceFieldTraceDistance = Distance; MarkRenderStateDirty(); }
	float GetDistanceFieldTraceDistance() const { return m_DistanceFieldTraceDistance; }

	void  SetLightSourceAngle(float AngleDeg) { m_LightSourceAngle = AngleDeg; MarkRenderStateDirty(); }
	float GetLightSourceAngle() const { return m_LightSourceAngle; }
};

// ============================================================
//  ULocalLightComponent
//  減衰半径を持つライト (Point / Spot / Rect) の共通基底。
// ============================================================
class ULocalLightComponent : public ULightComponent
{
protected:
	float       m_AttenuationRadius = 10.0f;			// 既定 1000cm -> 10m
	ELightUnits m_IntensityUnits = ELightUnits::Lumens;	// 既定 (逆二乗時は lm)

public:
	void  SetAttenuationRadius(float Radius) { m_AttenuationRadius = Radius; MarkRenderStateDirty(); }
	float GetAttenuationRadius() const { return m_AttenuationRadius; }

	void        SetIntensityUnits(ELightUnits Units) { m_IntensityUnits = Units; MarkRenderStateDirty(); }
	ELightUnits GetIntensityUnits() const { return m_IntensityUnits; }
};

// ============================================================
//  UPointLightComponent
// ============================================================
class UPointLightComponent : public ULocalLightComponent
{
protected:
	float m_SourceRadius = 0.0f;				// 球光源半径 [m] (スペキュラの丸み / ソフト化)
	float m_SoftSourceRadius = 0.0f;			// 見かけだけ柔らかくする追加半径 [m]
	float m_SourceLength = 0.0f;				// チューブ長 [m] (蛍光灯など。軸はコンポーネント +X)
	float m_LightFalloffExponent = 8.0f;		// 逆二乗無効時の指数フォールオフ (既定 8)
	bool  m_bUseInverseSquaredFalloff = true;	// 物理ベース逆二乗減衰 (既定 true)

public:
	UPointLightComponent()
	{
		m_Intensity = 5000.0f;	// 既定: 5000 lm (1700 lm ≒ 100W 電球)
	}

	ELightType GetLightType() const override { return ELightType::Point; }
	FLightSceneProxy* CreateLightSceneProxy() const override;

	// 単位換算 (cm^2 係数はメートル世界向けに換算済み):
	//   Candelas: x1 / Lumens: x1/(4π) / Unitless: x1/625 / EV: 2^Intensity
	float ComputeLightBrightness() const override;

	void  SetSourceRadius(float Radius) { m_SourceRadius = Radius; MarkRenderStateDirty(); }
	float GetSourceRadius() const { return m_SourceRadius; }

	void  SetSoftSourceRadius(float Radius) { m_SoftSourceRadius = Radius; MarkRenderStateDirty(); }
	float GetSoftSourceRadius() const { return m_SoftSourceRadius; }

	void  SetSourceLength(float Length) { m_SourceLength = Length; MarkRenderStateDirty(); }
	float GetSourceLength() const { return m_SourceLength; }

	void  SetLightFalloffExponent(float Exponent) { m_LightFalloffExponent = Exponent; MarkRenderStateDirty(); }
	float GetLightFalloffExponent() const { return m_LightFalloffExponent; }

	void SetUseInverseSquaredFalloff(bool bUse) { m_bUseInverseSquaredFalloff = bUse; MarkRenderStateDirty(); }
	bool GetUseInverseSquaredFalloff() const { return m_bUseInverseSquaredFalloff; }
};

// ============================================================
//  USpotLightComponent
//  UPointLightComponent 派生 (球光源 + コーン減衰)。
// ============================================================
class USpotLightComponent : public UPointLightComponent
{
protected:
	float m_InnerConeAngle = 0.0f;		// 度 (既定 0)
	float m_OuterConeAngle = 44.0f;		// 度 (既定 44)

public:
	ELightType GetLightType() const override { return ELightType::Spot; }
	FLightSceneProxy* CreateLightSceneProxy() const override;

	// Lumens はコーン立体角 2π(1-cosθ) で除して cd 化
	float ComputeLightBrightness() const override;

	void  SetInnerConeAngle(float AngleDegrees) { m_InnerConeAngle = AngleDegrees; MarkRenderStateDirty(); }
	float GetInnerConeAngle() const { return m_InnerConeAngle; }

	void  SetOuterConeAngle(float AngleDegrees) { m_OuterConeAngle = AngleDegrees; MarkRenderStateDirty(); }
	float GetOuterConeAngle() const { return m_OuterConeAngle; }

	// クランプ済み外側コーン半角 [ラジアン] (1..80 度)
	float GetHalfConeAngle() const;
	float GetCosHalfConeAngle() const;
};

// ============================================================
//  URectLightComponent
//  矩形面光源。発光はコンポーネント +Z、幅は +X、高さは +Y。
//  常に逆二乗フォールオフ。
// ============================================================
class URectLightComponent : public ULocalLightComponent
{
protected:
	float m_SourceWidth = 0.64f;		// 既定 64cm -> 0.64m
	float m_SourceHeight = 0.64f;		// 既定 64cm -> 0.64m
	float m_BarnDoorAngle = 88.0f;		// 度。88 ≒ 全開 
	float m_BarnDoorLength = 0.2f;		// 既定 20cm -> 0.2m

public:
	URectLightComponent()
	{
		m_Intensity = 5000.0f;	// 既定: 5000 lm
	}

	ELightType GetLightType() const override { return ELightType::Rect; }
	FLightSceneProxy* CreateLightSceneProxy() const override;

	// Lumens は半球コサイン分布の立体角 π で除して cd 化
	float ComputeLightBrightness() const override;

	void  SetSourceWidth(float Width) { m_SourceWidth = Width; MarkRenderStateDirty(); }
	float GetSourceWidth() const { return m_SourceWidth; }

	void  SetSourceHeight(float Height) { m_SourceHeight = Height; MarkRenderStateDirty(); }
	float GetSourceHeight() const { return m_SourceHeight; }

	void  SetBarnDoorAngle(float AngleDegrees) { m_BarnDoorAngle = AngleDegrees; MarkRenderStateDirty(); }
	float GetBarnDoorAngle() const { return m_BarnDoorAngle; }

	void  SetBarnDoorLength(float Length) { m_BarnDoorLength = Length; MarkRenderStateDirty(); }
	float GetBarnDoorLength() const { return m_BarnDoorLength; }
};
