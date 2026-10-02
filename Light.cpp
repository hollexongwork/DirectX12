#include "Main.h"
#include "Light.h"

void ALight::SetEnabled(bool bSetEnabled)
{
	if (m_LightComponent)
	{
		m_LightComponent->SetVisibility(bSetEnabled);
	}
}

bool ALight::IsEnabled() const
{
	return m_LightComponent ? m_LightComponent->IsVisible() : false;
}

void ALight::ToggleEnabled()
{
	if (m_LightComponent)
	{
		m_LightComponent->ToggleVisibility();
	}
}

void ALight::SetBrightness(float NewBrightness)
{
	if (m_LightComponent)
	{
		m_LightComponent->SetIntensity(NewBrightness);
	}
}

float ALight::GetBrightness() const
{
	return m_LightComponent ? m_LightComponent->GetIntensity() : 0.0f;
}

void ALight::SetLightColor(const XMFLOAT4& NewLightColor)
{
	if (m_LightComponent)
	{
		m_LightComponent->SetLightColor(NewLightColor);
	}
}

XMFLOAT4 ALight::GetLightColor() const
{
	// ライト不在時は黒 (UE FLinearColor::Black)
	return m_LightComponent ? m_LightComponent->GetLightColor() : XMFLOAT4(0.0f, 0.0f, 0.0f, 1.0f);
}

void ALight::SetCastShadows(bool bNewValue)
{
	if (m_LightComponent)
	{
		m_LightComponent->SetCastShadows(bNewValue);
	}
}

void ALight::SetAffectTranslucentLighting(bool bNewValue)
{
	if (m_LightComponent)
	{
		m_LightComponent->SetAffectTranslucentLighting(bNewValue);
	}
}
