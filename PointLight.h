#pragma once
#include "Light.h"
#include "PointLightComponent.h"

// ============================================================
//  APointLight
//  APointLight (Engine/Classes/Engine/PointLight.h) に相当。
//  UPointLightComponent を Root に持つ。
//  逆二乗フォールオフ + AttenuationRadius の窓関数。
//  SourceRadius / SourceLength で面光源 (球 / チューブ) になる。
// ============================================================

class APointLight : public ALight
{
private:
	UPointLightComponent* m_PointLightComponent = nullptr;

public:
	APointLight();

	UPointLightComponent* GetPointLightComponent() const { return m_PointLightComponent; }

	// 減衰半径 [m]
	void SetRadius(float NewRadius);
	void SetLightFalloffExponent(float NewLightFalloffExponent);
};
