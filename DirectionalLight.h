#pragma once
#include "Light.h"

// ============================================================
//  ADirectionalLight
//  UDirectionalLightComponent をRoot に持つ。発光方向はアクター
//  の前方 (+Z)。強度は lux。
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