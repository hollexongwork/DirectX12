#include "Main.h"
#include "LocalLightComponent.h"
#include "LocalLightSceneProxy.h"

ULocalLightComponent::ULocalLightComponent()
{
	m_Intensity = 5000.0f;	// 既定 5000 (lm)
}

void ULocalLightComponent::SetAttenuationRadius(float NewRadius)
{
	if (NewRadius != m_AttenuationRadius)
	{
		m_AttenuationRadius = NewRadius;
		PushRadiusToRenderThread();
	}
}

void ULocalLightComponent::SetIntensityUnits(ELightUnits NewIntensityUnits)
{
	if (m_IntensityUnits != NewIntensityUnits)
	{
		m_IntensityUnits = NewIntensityUnits;
		UpdateColorAndBrightness();
	}
}

// ULocalLightComponent::PushRadiusToRenderThread 相当
void ULocalLightComponent::PushRadiusToRenderThread()
{
	if (m_CastShadows)
	{
		// 影を落とすライトはシャドウの範囲も変わるので作り直す
		MarkRenderStateDirty();
	}
	else if (m_SceneProxy)
	{
		static_cast<FLocalLightSceneProxy*>(m_SceneProxy)->UpdateRadius_GameThread(m_AttenuationRadius);
	}
}

float ULocalLightComponent::GetUnitsConversionFactor(ELightUnits SrcUnits, ELightUnits TargetUnits, float CosHalfConeAngle)
{
	CosHalfConeAngle = fmaxf(fminf(CosHalfConeAngle, 1.0f - 1.0e-4f), -1.0f);

	if (SrcUnits == TargetUnits || SrcUnits == ELightUnits::EV || TargetUnits == ELightUnits::EV)
	{
		return 1.0f;
	}

	// いったん cd へ (距離はメートルなので cm^2 -> m^2 の換算は掛けない)
	float CnvFactor = 1.0f;
	if (SrcUnits == ELightUnits::Candelas)
	{
		CnvFactor = 1.0f;
	}
	else if (SrcUnits == ELightUnits::Lumens)
	{
		CnvFactor = 1.0f / 2.0f / XM_PI / (1.0f - CosHalfConeAngle);
	}
	else
	{
		CnvFactor = 16.0f / 10000.0f;	// 旧来の係数 16 (cm^2 基準)
	}

	if (TargetUnits == ELightUnits::Candelas)
	{
		CnvFactor *= 1.0f;
	}
	else if (TargetUnits == ELightUnits::Lumens)
	{
		CnvFactor *= 2.0f * XM_PI * (1.0f - CosHalfConeAngle);
	}
	else
	{
		CnvFactor *= 10000.0f / 16.0f;
	}

	return CnvFactor;
}

XMFLOAT4 ULocalLightComponent::GetLightPosition() const
{
	const XMFLOAT3 location = GetComponentLocation();
	return XMFLOAT4(location.x, location.y, location.z, 1.0f);
}

FSphere ULocalLightComponent::GetBoundingSphere() const
{
	return FSphere(GetComponentLocation(), m_AttenuationRadius);
}
