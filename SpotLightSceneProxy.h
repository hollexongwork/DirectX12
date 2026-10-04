#pragma once
#include "PointLightSceneProxy.h"

class USpotLightComponent;

// ============================================================
//  FSpotLightSceneProxy
//  FSpotLightSceneProxy に相当。
//  ポイントライト + コーン減衰。
// ============================================================
class FSpotLightSceneProxy : public FPointLightSceneProxy
{
public:
	explicit FSpotLightSceneProxy(const USpotLightComponent* Component);

	void GetLightShaderParameters(FLightRenderParameters& OutLightParameters) const override;

	// コーンを包む球との交差も見る
	bool    AffectsBounds(const FBoxSphereBounds& Bounds) const override;
	// コーンを包む最小の球 (FMath::ComputeBoundingSphereForCone)
	FSphere GetBoundingSphere() const override;

	float GetOuterConeAngle() const override { return m_OuterConeAngle; }
	float GetCosOuterCone() const { return m_CosOuterCone; }
	float GetSinOuterCone() const { return m_SinOuterCone; }

protected:
	float m_OuterConeAngle = 0.0f;			// クランプ済みの外側コーン半角 [rad]
	float m_CosOuterCone = 1.0f;
	float m_SinOuterCone = 0.0f;
	float m_CosInnerCone = 1.0f;
	float m_InvCosConeDifference = 1.0f;	// 1 / (cos(Inner) - cos(Outer))
};
