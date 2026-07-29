#include "Main.h"
#include "DirectionalLight.h"

ADirectionalLight::ADirectionalLight()
{
	m_DirectionalLightComponent = CreateDefaultSubobject<UDirectionalLightComponent>();
	m_LightComponent = m_DirectionalLightComponent;

	// 既定 (Pitch -46 度) 見下ろし方向
	SetActorRotation({ XMConvertToRadians(46.0f), 0.0f, 0.0f });
}