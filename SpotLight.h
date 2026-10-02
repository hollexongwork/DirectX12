#pragma once
#include "Light.h"
#include "SpotLightComponent.h"

// ============================================================
//  ASpotLight
//  ASpotLight (Engine/Classes/Engine/SpotLight.h) に相当。ALight 直下。
//  USpotLightComponent を Root に持ち、アクターの前方 (+Z) へ
//  Inner / OuterConeAngle のコーンで照射する。
// ============================================================

class ASpotLight : public ALight
{
private:
	USpotLightComponent* m_SpotLightComponent = nullptr;

public:
	ASpotLight();

	USpotLightComponent* GetSpotLightComponent() const { return m_SpotLightComponent; }

	// コーン角 [度]
	void SetInnerConeAngle(float NewInnerConeAngle);
	void SetOuterConeAngle(float NewOuterConeAngle);
};
