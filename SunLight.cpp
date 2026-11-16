#include "Main.h"
#include "SunLight.h"
#include "DirectionalLightComponent.h"

ASunLight::ASunLight()
{
	SetActorRotation(ULightComponentBase::DirectionToRotator({ -3.0f, -5.0f, 3.0f }));	// 発光方向
	GetLightComponent()->SetIntensity(3.0f);	// lux
}
