#pragma once
#include "Light.h"
#include "DirectionalLightComponent.h"

// ============================================================
//  ADirectionalLight
//  ADirectionalLight (Engine/Classes/Engine/DirectionalLight.h) に相当。
//  UDirectionalLightComponent を Root に持つ。発光方向はアクターの
//  前方 (+Z)。強度は lux。
//  向きの指定は SetActorRotation か、方向ベクトルからの変換に
//  ULightComponentBase::DirectionToRotator を使う。
// ============================================================

class ADirectionalLight : public ALight
{
private:
	UDirectionalLightComponent* m_DirectionalLightComponent = nullptr;

public:
	ADirectionalLight();

	UDirectionalLightComponent* GetDirectionalLightComponent() const { return m_DirectionalLightComponent; }
};
