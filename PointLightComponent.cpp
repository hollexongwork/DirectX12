#include "Main.h"
#include "PointLightComponent.h"
#include "PointLightSceneProxy.h"

// ============================================================
//  FPointLightSceneProxy
// ============================================================

FPointLightSceneProxy::FPointLightSceneProxy(const UPointLightComponent* Component)
	: FLocalLightSceneProxy(Component)
	, m_FalloffExponent(Component->GetLightFalloffExponent())
	, m_SourceRadius(Component->GetSourceRadius())
	, m_SoftSourceRadius(Component->GetSoftSourceRadius())
	, m_SourceLength(Component->GetSourceLength())
	, m_bInverseSquared(Component->GetUseInverseSquaredFalloff())
{
}

// FPointLightSceneProxy::GetLightShaderParameters 相当
void FPointLightSceneProxy::GetLightShaderParameters(FLightRenderParameters& OutLightParameters) const
{
	const XMFLOAT3 direction = GetDirection();

	OutLightParameters.WorldPosition = GetOrigin();
	OutLightParameters.InvRadius = m_InvRadius;
	OutLightParameters.Color = GetColor();
	OutLightParameters.FalloffExponent = m_FalloffExponent;

	OutLightParameters.Direction = { -direction.x, -direction.y, -direction.z };
	OutLightParameters.Tangent = GetUpVector();
	// cos(Outer) = -2 でコーン減衰が常に 1 (全方位)
	OutLightParameters.SpotAngles = { -2.0f, 1.0f };
	OutLightParameters.SpecularScale = m_SpecularScale;
	OutLightParameters.DiffuseScale = m_DiffuseScale;
	OutLightParameters.SourceRadius = m_SourceRadius;
	OutLightParameters.SoftSourceRadius = m_SoftSourceRadius;
	OutLightParameters.SourceLength = m_SourceLength;
	OutLightParameters.RectLightBarnCosAngle = 0.0f;
	OutLightParameters.RectLightBarnLength = 0.0f;
	OutLightParameters.bAffectsTranslucentLighting = m_bAffectTranslucentLighting ? 1u : 0u;
}

// ============================================================
//  UPointLightComponent
// ============================================================

FLightSceneProxy* UPointLightComponent::CreateSceneProxy() const
{
	return new FPointLightSceneProxy(this);
}

float UPointLightComponent::ComputeLightBrightness() const
{
	float LightBrightness = ULightComponent::ComputeLightBrightness();

	if (m_bUseInverseSquaredFalloff)
	{
		if (m_IntensityUnits == ELightUnits::Candelas)
		{
			// cd はそのまま (距離はメートルなので cm^2 -> m^2 の換算は掛けない)
		}
		else if (m_IntensityUnits == ELightUnits::Lumens)
		{
			// 全球 4π sr で割って cd にする
			LightBrightness *= 1.0f / (4.0f * XM_PI);
		}
		else if (m_IntensityUnits == ELightUnits::EV)
		{
			LightBrightness = EV100ToLuminance(LightBrightness);
		}
		else
		{
			// 旧来の係数 16 (cm^2 基準) -> 16 / 10000
			LightBrightness *= 1.0f / 625.0f;
		}
	}

	return LightBrightness;
}

void UPointLightComponent::SetLightBrightness(float InBrightness)
{
	float NewIntensity = InBrightness;

	if (m_bUseInverseSquaredFalloff)
	{
		if (m_IntensityUnits == ELightUnits::Candelas)
		{
			NewIntensity = InBrightness;
		}
		else if (m_IntensityUnits == ELightUnits::Lumens)
		{
			NewIntensity = InBrightness * (4.0f * XM_PI);
		}
		else if (m_IntensityUnits == ELightUnits::EV)
		{
			NewIntensity = log2f(fmaxf(InBrightness, 1.0e-8f) / 1.2f);
		}
		else
		{
			NewIntensity = InBrightness * 625.0f;
		}
	}

	SetIntensity(NewIntensity);
}

void UPointLightComponent::SetUseInverseSquaredFalloff(bool bNewValue)
{
	if (m_bUseInverseSquaredFalloff != bNewValue)
	{
		m_bUseInverseSquaredFalloff = bNewValue;
		MarkRenderStateDirty();
	}
}

void UPointLightComponent::SetLightFalloffExponent(float NewLightFalloffExponent)
{
	if (NewLightFalloffExponent != m_LightFalloffExponent)
	{
		m_LightFalloffExponent = NewLightFalloffExponent;
		MarkRenderStateDirty();
	}
}

void UPointLightComponent::SetSourceRadius(float NewValue)
{
	if (m_SourceRadius != NewValue)
	{
		m_SourceRadius = NewValue;
		MarkRenderStateDirty();
	}
}

void UPointLightComponent::SetSoftSourceRadius(float NewValue)
{
	if (m_SoftSourceRadius != NewValue)
	{
		m_SoftSourceRadius = NewValue;
		MarkRenderStateDirty();
	}
}

void UPointLightComponent::SetSourceLength(float NewValue)
{
	if (m_SourceLength != NewValue)
	{
		m_SourceLength = NewValue;
		MarkRenderStateDirty();
	}
}
