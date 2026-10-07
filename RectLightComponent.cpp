#include "Main.h"
#include "RectLightComponent.h"
#include "RectLightSceneProxy.h"

// ============================================================
//  FRectLightSceneProxy
// ============================================================

FRectLightSceneProxy::FRectLightSceneProxy(const URectLightComponent* Component)
	: FLocalLightSceneProxy(Component)
	, m_SourceWidth(Component->GetSourceWidth())
	, m_SourceHeight(Component->GetSourceHeight())
	, m_BarnDoorAngle(fmaxf(fminf(Component->GetBarnDoorAngle(), GetRectLightBarnDoorMaxAngle()), 0.0f))
	// 1 mm を下限にする
	, m_BarnDoorLength(fmaxf(0.001f, Component->GetBarnDoorLength()))
{
}

// FRectLightSceneProxy::GetLightShaderParameters 相当
void FRectLightSceneProxy::GetLightShaderParameters(FLightRenderParameters& OutLightParameters) const
{
	const XMFLOAT3 direction = GetDirection();

	// 強度 -> 発光面の放射輝度 (シェーダは RectIrradianceLambert で面を積分する)。
	// LightColor /= 0.5f * SourceWidth * SourceHeight (単位は m)。
	// 面積 0 の退化は NaN を避けるため下限を置く
	XMFLOAT3 LightColor = GetColor();
	const float RadianceScale = 1.0f / fmaxf(0.5f * m_SourceWidth * m_SourceHeight, 1e-6f);
	LightColor.x *= RadianceScale;
	LightColor.y *= RadianceScale;
	LightColor.z *= RadianceScale;

	OutLightParameters.WorldPosition = GetOrigin();
	OutLightParameters.InvRadius = m_InvRadius;
	OutLightParameters.Color = LightColor;
	OutLightParameters.FalloffExponent = 0.0f;

	OutLightParameters.Direction = { -direction.x, -direction.y, -direction.z };
	OutLightParameters.Tangent = GetUpVector();
	OutLightParameters.SpotAngles = { -2.0f, 1.0f };
	OutLightParameters.SpecularScale = m_SpecularScale;
	OutLightParameters.DiffuseScale = m_DiffuseScale;
	OutLightParameters.SourceRadius = m_SourceWidth * 0.5f;
	OutLightParameters.SoftSourceRadius = 0.0f;
	OutLightParameters.SourceLength = m_SourceHeight * 0.5f;
	OutLightParameters.RectLightBarnCosAngle = cosf(XMConvertToRadians(m_BarnDoorAngle));
	OutLightParameters.RectLightBarnLength = m_BarnDoorLength;
	OutLightParameters.bAffectsTranslucentLighting = m_bAffectTranslucentLighting ? 1u : 0u;
}

// ============================================================
//  URectLightComponent
// ============================================================

FLightSceneProxy* URectLightComponent::CreateSceneProxy() const
{
	return new FRectLightSceneProxy(this);
}

float URectLightComponent::ComputeLightBrightness() const
{
	float LightBrightness = ULightComponent::ComputeLightBrightness();

	// レクトライトは常に逆二乗
	if (m_IntensityUnits == ELightUnits::Candelas)
	{
		// cd はそのまま
	}
	else if (m_IntensityUnits == ELightUnits::Lumens)
	{
		// 半球コサイン分布の π で割って cd にする
		LightBrightness *= 1.0f / XM_PI;
	}
	else if (m_IntensityUnits == ELightUnits::EV)
	{
		LightBrightness = EV100ToLuminance(LightBrightness);
	}
	else
	{
		LightBrightness *= 1.0f / 625.0f;
	}

	return LightBrightness;
}

void URectLightComponent::SetLightBrightness(float InBrightness)
{
	if (m_IntensityUnits == ELightUnits::Candelas)
	{
		SetIntensity(InBrightness);
	}
	else if (m_IntensityUnits == ELightUnits::Lumens)
	{
		SetIntensity(InBrightness * XM_PI);
	}
	else if (m_IntensityUnits == ELightUnits::EV)
	{
		SetIntensity(log2f(fmaxf(InBrightness, 1.0e-8f) / 1.2f));
	}
	else
	{
		SetIntensity(InBrightness * 625.0f);
	}
}

void URectLightComponent::SetSourceWidth(float NewValue)
{
	if (m_SourceWidth != NewValue)
	{
		m_SourceWidth = NewValue;
		MarkRenderStateDirty();
	}
}

void URectLightComponent::SetSourceHeight(float NewValue)
{
	if (m_SourceHeight != NewValue)
	{
		m_SourceHeight = NewValue;
		MarkRenderStateDirty();
	}
}

void URectLightComponent::SetBarnDoorAngle(float NewValue)
{
	if (m_BarnDoorAngle != NewValue)
	{
		m_BarnDoorAngle = NewValue;
		MarkRenderStateDirty();
	}
}

void URectLightComponent::SetBarnDoorLength(float NewValue)
{
	if (m_BarnDoorLength != NewValue)
	{
		m_BarnDoorLength = NewValue;
		MarkRenderStateDirty();
	}
}
