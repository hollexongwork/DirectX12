#pragma once
#include "PointLightComponent.h"

// ============================================================
//  USpotLightComponent
//  USpotLightComponent に相当。
//  コンポーネントの前方 (+Z) へ Inner / OuterConeAngle のコーンで照射する。
// ============================================================
class USpotLightComponent : public UPointLightComponent
{
protected:
	float m_InnerConeAngle = 0.0f;		// 度 (既定 0)。この内側は減衰なし
	float m_OuterConeAngle = 44.0f;		// 度 (既定 44)。この外側は光が届かない

public:
	ELightComponentType GetLightType() const override { return LightType_Spot; }
	FLightSceneProxy* CreateSceneProxy() const override;

	// Lumens はコーンの立体角 2π(1 - cosθ) で割って cd にする。他は UPointLightComponent と同じ
	float ComputeLightBrightness() const override;
	void  SetLightBrightness(float InBrightness) override;

	// コーンを包む最小の球
	FSphere GetBoundingSphere() const override;

	void  SetInnerConeAngle(float NewInnerConeAngle);
	float GetInnerConeAngle() const { return m_InnerConeAngle; }

	void  SetOuterConeAngle(float NewOuterConeAngle);
	float GetOuterConeAngle() const { return m_OuterConeAngle; }

	// クランプ済みの外側コーン半角 [rad]。
	// 内側は 0..89 度、外側は (内側 + 0.001 rad)..(89 度 + 0.001 rad)
	float GetHalfConeAngle() const;
	float GetCosHalfConeAngle() const;
};
