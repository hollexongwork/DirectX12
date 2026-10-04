#pragma once
#include "LocalLightComponent.h"

// ============================================================
//  UPointLightComponent
//  UPointLightComponent に相当。
//  全方位に光る点光源。SourceRadius / SourceLength で球 / チューブの面光源になる。
// ============================================================
class UPointLightComponent : public ULocalLightComponent
{
protected:
	// 物理ベースの逆二乗減衰を使うか。false のときは LightFalloffExponent による指数フォールオフ
	// (このとき Intensity は単位なしの明るさ)
	bool  m_bUseInverseSquaredFalloff = true;
	float m_LightFalloffExponent = 8.0f;	// 逆二乗でないときの指数 (既定 8)

	float m_SourceRadius = 0.0f;			// 球光源の半径 [m]
	float m_SoftSourceRadius = 0.0f;		// 柔らかさだけを足す半径 [m]
	float m_SourceLength = 0.0f;			// チューブの長さ [m] (軸はコンポーネント +Y)

public:
	ELightComponentType GetLightType() const override { return LightType_Point; }
	FLightSceneProxy* CreateSceneProxy() const override;

	// 単位換算 (メートル世界向け):
	//   Candelas: x1 / Lumens: x 1/(4π) / EV: 1.2 * 2^Intensity / Unitless: x 1/625
	// 逆二乗でないときは Intensity そのまま
	float ComputeLightBrightness() const override;
	// ComputeLightBrightness の逆。明るさ (cd 相当) から Intensity を設定する
	virtual void SetLightBrightness(float InBrightness);

	void SetUseInverseSquaredFalloff(bool bNewValue);
	bool GetUseInverseSquaredFalloff() const { return m_bUseInverseSquaredFalloff; }

	void  SetLightFalloffExponent(float NewLightFalloffExponent);
	float GetLightFalloffExponent() const { return m_LightFalloffExponent; }

	void  SetSourceRadius(float NewValue);
	float GetSourceRadius() const { return m_SourceRadius; }

	void  SetSoftSourceRadius(float NewValue);
	float GetSoftSourceRadius() const { return m_SoftSourceRadius; }

	void  SetSourceLength(float NewValue);
	float GetSourceLength() const { return m_SourceLength; }
};
