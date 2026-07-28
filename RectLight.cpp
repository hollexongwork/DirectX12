#include "Main.h"
#include "RectLight.h"

ARectLight::ARectLight()
{
	m_RectLightComponent = CreateDefaultSubobject<URectLightComponent>();
	m_LightComponent = m_RectLightComponent;
}
