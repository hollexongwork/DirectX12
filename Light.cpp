#include "Main.h"
#include "Light.h"

void ALight::SetEnabled(bool bEnabled)
{
	if (m_LightComponent)
	{
		m_LightComponent->SetAffectsWorld(bEnabled);
	}
}

bool ALight::IsEnabled() const
{
	return m_LightComponent ? m_LightComponent->GetAffectsWorld() : false;
}

void ALight::SetBrightness(float Brightness)
{
	if (m_LightComponent)
	{
		m_LightComponent->SetIntensity(Brightness);
	}
}

float ALight::GetBrightness() const
{
	return m_LightComponent ? m_LightComponent->GetIntensity() : 0.0f;
}

void ALight::SetLightColor(const XMFLOAT4& Color)
{
	if (m_LightComponent)
	{
		m_LightComponent->SetLightColor(Color);
	}
}

XMFLOAT4 ALight::GetLightColor() const
{
	return m_LightComponent ? m_LightComponent->GetLightColor() : XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f);
}
