#pragma once
#include "LightSceneProxy.h"

class UDirectionalLightComponent;

// ============================================================
//  FDirectionalLightSceneProxy
//  FDirectionalLightSceneProxy に相当。シャドウ描画系
//  (FShadowSceneRenderer) が CSM / Distance Field のパラメータを
//  直接読むのでヘッダに出す。
//
//  光源は見かけの全角 LightSourceAngle [度] の円盤として扱い、
//  SourceRadius = sin(半角) をシェーダへ渡す (スペキュラの広がりと
//  地平線付近の NoL の回り込みに効く)。
// ============================================================
class FDirectionalLightSceneProxy : public FLightSceneProxy
{
public:
	explicit FDirectionalLightSceneProxy(const UDirectionalLightComponent* Component);

	void GetLightShaderParameters(FLightRenderParameters& OutLightParameters) const override;

	float GetLightSourceAngle() const override { return m_LightSourceAngle; }
	float GetTraceDistance() const override { return m_TraceDistance; }
	int   GetDirectionalLightForwardShadingPriority() const override { return m_ForwardShadingPriority; }

	// ---- CSM (FShadowSceneRenderer::SetupDirectionalShadows が読む) ----
	// CSM がカバーする距離 [m] (= DynamicShadowDistanceMovableLight)
	float GetWholeSceneDynamicShadowRadius() const { return m_WholeSceneDynamicShadowRadius; }
	int   GetNumDynamicShadowCascades() const { return m_DynamicShadowCascades; }
	float GetCascadeDistributionExponent() const { return m_CascadeDistributionExponent; }
	float GetShadowDistanceFadeoutFraction() const { return m_ShadowDistanceFadeoutFraction; }

	// ---- Distance Field Shadows ----
	// DF シャドウがカバーする距離 [m]
	float GetDistanceFieldShadowDistance() const { return m_DistanceFieldShadowDistance; }

	float GetLightSourceSoftAngle() const { return m_LightSourceSoftAngle; }

protected:
	float m_WholeSceneDynamicShadowRadius = 100.0f;
	int   m_DynamicShadowCascades = 4;
	float m_CascadeDistributionExponent = 3.0f;
	float m_ShadowDistanceFadeoutFraction = 0.1f;
	float m_DistanceFieldShadowDistance = 300.0f;
	float m_TraceDistance = 100.0f;
	float m_LightSourceAngle = 0.5357f;		// [度]
	float m_LightSourceSoftAngle = 0.0f;	// [度]
	int   m_ForwardShadingPriority = 0;
};
