#pragma once
#include "LightComponent.h"

// ============================================================
//  UDirectionalLightComponent
//  UDirectionalLightComponent (Engine/Classes/Components/
//  DirectionalLightComponent.h) に相当。強度は lux (照度)。減衰なし。
//
//  デファードは全てのディレクショナルライトを照らす。フォワード
//  (半透明) と Volumetric Fog は「選択された 1 灯」(ForwardShadingPriority
//  が最大、同値なら最も明るいもの) を使い、CSM / Distance Field
//  シャドウを持つのもその 1 灯だけ [PORT]。
//  Exponential Height Fog の太陽は bAtmosphereSunLight のライトから選ぶ
//  (FScene::AtmosphereLights)。
// ============================================================
class UDirectionalLightComponent : public ULightComponent
{
protected:
	// ---- CSM ----
	// 既定は本エンジンの CSM 実装に合わせた値 [PORT: UE は 200 m / 3 カスケード]
	float m_DynamicShadowDistanceMovableLight = 100.0f;	// CSM がカバーする距離 [m]
	int   m_DynamicShadowCascades = 4;					// カスケード数 (1..MAX_SHADOW_CASCADES)
	float m_CascadeDistributionExponent = 3.0f;			// 分割の指数 (大きいほど手前が細かい)
	float m_ShadowDistanceFadeoutFraction = 0.1f;		// 遠端でフェードアウトする割合

	// ---- Distance Field Shadows ----
	float m_DistanceFieldShadowDistance = 300.0f;		// DF シャドウがカバーする距離 [m] (UE 30000 cm)
	float m_TraceDistance = 100.0f;						// 1 レイの最大トレース距離 [m] (UE 10000 cm)

	// ---- 光源の見かけの大きさ ----
	float m_LightSourceAngle = 0.5357f;					// 見かけの全角 [度] (太陽 = 0.5357)
	float m_LightSourceSoftAngle = 0.0f;				// 柔らかさだけを足す全角 [度] (エネルギー正規化なし)

	// ---- 選択 ----
	int           m_ForwardShadingPriority = 0;			// フォワード / Volumetric Fog で使うライトの優先度
	bool          m_bAtmosphereSunLight = true;			// フォグの太陽として使うか
	unsigned char m_AtmosphereSunLightIndex = 0;		// 0 = 太陽、1 = 月 (フォグは 0 だけを使う)

public:
	UDirectionalLightComponent();

	ELightComponentType GetLightType() const override { return LightType_Directional; }
	FLightSceneProxy* CreateSceneProxy() const override;
	XMFLOAT4 GetLightPosition() const override;

	bool IsUsedAsAtmosphereSunLight() const override { return m_bAtmosphereSunLight; }
	unsigned char GetAtmosphereSunLightIndex() const override { return m_AtmosphereSunLightIndex; }

	// ---- CSM ----
	void  SetDynamicShadowDistanceMovableLight(float NewValue);
	float GetDynamicShadowDistanceMovableLight() const { return m_DynamicShadowDistanceMovableLight; }

	void SetDynamicShadowCascades(int NewValue);
	int  GetDynamicShadowCascades() const { return m_DynamicShadowCascades; }

	void  SetCascadeDistributionExponent(float NewValue);
	float GetCascadeDistributionExponent() const { return m_CascadeDistributionExponent; }

	void  SetShadowDistanceFadeoutFraction(float NewValue);
	float GetShadowDistanceFadeoutFraction() const { return m_ShadowDistanceFadeoutFraction; }

	// ---- Distance Field Shadows ----
	void  SetDistanceFieldShadowDistance(float NewValue);
	float GetDistanceFieldShadowDistance() const { return m_DistanceFieldShadowDistance; }

	void  SetTraceDistance(float NewValue);
	float GetTraceDistance() const { return m_TraceDistance; }

	// ---- 光源の見かけの大きさ ----
	void  SetLightSourceAngle(float NewValue);
	float GetLightSourceAngle() const { return m_LightSourceAngle; }

	void  SetLightSourceSoftAngle(float NewValue);
	float GetLightSourceSoftAngle() const { return m_LightSourceSoftAngle; }

	// ---- 選択 ----
	void SetForwardShadingPriority(int NewValue);
	int  GetForwardShadingPriority() const { return m_ForwardShadingPriority; }

	void SetAtmosphereSunLight(bool bNewValue);
	bool GetAtmosphereSunLight() const { return m_bAtmosphereSunLight; }

	void SetAtmosphereSunLightIndex(int NewValue);
};
