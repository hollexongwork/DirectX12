#include "Main.h"
#include "DirectionalLight.h"

ADirectionalLight::ADirectionalLight()
{
	m_DirectionalLightComponent = CreateDefaultSubobject<UDirectionalLightComponent>();
	m_LightComponent = m_DirectionalLightComponent;

	// Šù’è (Pitch -46 “x) Œ©‰º‚ë‚µ•ûŒü
	SetActorRotation({ XMConvertToRadians(46.0f), 0.0f, 0.0f });
}