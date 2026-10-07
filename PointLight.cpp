#include "Main.h"
#include "PointLight.h"

APointLight::APointLight()
{
	m_PointLightComponent = CreateDefaultSubobject<UPointLightComponent>();
	m_LightComponent = m_PointLightComponent;
}

void APointLight::SetRadius(float NewRadius)
{
	m_PointLightComponent->SetAttenuationRadius(NewRadius);
}

void APointLight::SetLightFalloffExponent(float NewLightFalloffExponent)
{
	m_PointLightComponent->SetLightFalloffExponent(NewLightFalloffExponent);
}
