#include "Main.h"
#include "PointLight.h"

APointLight::APointLight()
{
	m_PointLightComponent = CreateDefaultSubobject<UPointLightComponent>();
	m_LightComponent = m_PointLightComponent;
}