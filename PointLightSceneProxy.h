#pragma once
#include "LocalLightSceneProxy.h"

class UPointLightComponent;

// ============================================================
//  FPointLightSceneProxy
//  FPointLightSceneProxy (Engine/Public/PointLightSceneProxy.h) に相当。
//  光源形状 (球 / チューブ) とフォールオフ (逆二乗 / 指数) を持つ。
//  チューブの軸はライトの上方向 (Tangent) [PORT: UE ローカル Z = 本エンジン +Y]。
// ============================================================
class FPointLightSceneProxy : public FLocalLightSceneProxy
{
public:
	explicit FPointLightSceneProxy(const UPointLightComponent* Component);

	float GetSourceRadius() const override { return m_SourceRadius; }
	bool  IsInverseSquared() const override { return m_bInverseSquared; }

	void GetLightShaderParameters(FLightRenderParameters& OutLightParameters) const override;

protected:
	float m_FalloffExponent = 8.0f;		// 逆二乗でないときの指数フォールオフ
	float m_SourceRadius = 0.0f;		// 球光源の半径 [m]
	float m_SoftSourceRadius = 0.0f;	// 柔らかさだけを足す半径 [m]
	float m_SourceLength = 0.0f;		// チューブの長さ [m]
	bool  m_bInverseSquared = true;
};
