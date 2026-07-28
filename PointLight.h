#pragma once
#include "Light.h"

// ============================================================
//  APointLight
//  UPointLightComponent を Root に持つ。
//  逆二乗フォールオフ + AttenuationRadius 窓関数。
//  SourceRadius / SourceLength で面光源 (球 / チューブ) 化できる。
// ============================================================

class APointLight : public ALight
{
private:
	UPointLightComponent* m_PointLightComponent = nullptr;

public:
	APointLight();

	UPointLightComponent* GetPointLightComponent() const { return m_PointLightComponent; }
};