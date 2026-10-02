#include "Main.h"
#include "SpotLightComponent.h"
#include "SpotLightSceneProxy.h"

// コーンを包む最小の球 (FMath::ComputeBoundingSphereForCone)。
// https://bartwronski.com/2017/04/13/cull-that-cone/
static FSphere ComputeBoundingSphereForCone(const XMFLOAT3& ConeOrigin, const XMFLOAT3& ConeDirection,
	float ConeRadius, float CosConeAngle, float SinConeAngle)
{
	const float COS_PI_OVER_4 = 0.70710678118f;

	if (CosConeAngle < COS_PI_OVER_4)
	{
		// 広いコーン (半角 > 45 度): 底面の円を包む球
		const float t = ConeRadius * CosConeAngle;
		return FSphere(
			XMFLOAT3(ConeOrigin.x + ConeDirection.x * t, ConeOrigin.y + ConeDirection.y * t, ConeOrigin.z + ConeDirection.z * t),
			ConeRadius * SinConeAngle);
	}

	const float BoundingRadius = ConeRadius / (2.0f * CosConeAngle);
	return FSphere(
		XMFLOAT3(ConeOrigin.x + ConeDirection.x * BoundingRadius, ConeOrigin.y + ConeDirection.y * BoundingRadius, ConeOrigin.z + ConeDirection.z * BoundingRadius),
		BoundingRadius);
}

// ============================================================
//  FSpotLightSceneProxy
// ============================================================

FSpotLightSceneProxy::FSpotLightSceneProxy(const USpotLightComponent* Component)
	: FPointLightSceneProxy(Component)
{
	const float ClampedInnerConeAngle = fmaxf(fminf(Component->GetInnerConeAngle(), 89.0f), 0.0f) * XM_PI / 180.0f;
	const float ClampedOuterConeAngle = fmaxf(
		fminf(Component->GetOuterConeAngle() * XM_PI / 180.0f, 89.0f * XM_PI / 180.0f + 0.001f),
		ClampedInnerConeAngle + 0.001f);

	m_OuterConeAngle = ClampedOuterConeAngle;
	m_CosOuterCone = cosf(ClampedOuterConeAngle);
	m_SinOuterCone = sinf(ClampedOuterConeAngle);
	m_CosInnerCone = cosf(ClampedInnerConeAngle);
	m_InvCosConeDifference = 1.0f / (m_CosInnerCone - m_CosOuterCone);
}

// FSpotLightSceneProxy::GetLightShaderParameters 相当
void FSpotLightSceneProxy::GetLightShaderParameters(FLightRenderParameters& OutLightParameters) const
{
	FPointLightSceneProxy::GetLightShaderParameters(OutLightParameters);

	OutLightParameters.SpotAngles = { m_CosOuterCone, m_InvCosConeDifference };
}

bool FSpotLightSceneProxy::AffectsBounds(const FBoxSphereBounds& Bounds) const
{
	if (!FLocalLightSceneProxy::AffectsBounds(Bounds))
	{
		return false;
	}

	// 球 vs コーン (FSpotLightSceneProxy::AffectsBounds と同じ判定)
	const XMVECTOR origin = XMLoadFloat3(&Bounds.Origin);
	const XMFLOAT3 lightOrigin = GetOrigin();
	const XMFLOAT3 lightDirection = GetDirection();
	const XMVECTOR O = XMLoadFloat3(&lightOrigin);
	const XMVECTOR Dir = XMLoadFloat3(&lightDirection);

	const XMVECTOR U = O - (Bounds.SphereRadius / m_SinOuterCone) * Dir;
	XMVECTOR D = origin - U;
	float dsqr = XMVectorGetX(XMVector3Dot(D, D));
	float E = XMVectorGetX(XMVector3Dot(Dir, D));

	if (E > 0.0f && E * E >= dsqr * m_CosOuterCone * m_CosOuterCone)
	{
		D = origin - O;
		dsqr = XMVectorGetX(XMVector3Dot(D, D));
		E = -XMVectorGetX(XMVector3Dot(Dir, D));
		if (E > 0.0f && E * E >= dsqr * m_SinOuterCone * m_SinOuterCone)
		{
			return dsqr <= Bounds.SphereRadius * Bounds.SphereRadius;
		}
		return true;
	}

	return false;
}

FSphere FSpotLightSceneProxy::GetBoundingSphere() const
{
	return ComputeBoundingSphereForCone(GetOrigin(), GetDirection(), m_Radius, m_CosOuterCone, m_SinOuterCone);
}

// ============================================================
//  USpotLightComponent
// ============================================================

FLightSceneProxy* USpotLightComponent::CreateSceneProxy() const
{
	return new FSpotLightSceneProxy(this);
}

float USpotLightComponent::GetHalfConeAngle() const
{
	const float ClampedInnerConeAngle = fmaxf(fminf(m_InnerConeAngle, 89.0f), 0.0f) * XM_PI / 180.0f;
	const float ClampedOuterConeAngle = fmaxf(
		fminf(m_OuterConeAngle * XM_PI / 180.0f, 89.0f * XM_PI / 180.0f + 0.001f),
		ClampedInnerConeAngle + 0.001f);
	return ClampedOuterConeAngle;
}

float USpotLightComponent::GetCosHalfConeAngle() const
{
	return cosf(GetHalfConeAngle());
}

float USpotLightComponent::ComputeLightBrightness() const
{
	float LightBrightness = ULightComponent::ComputeLightBrightness();

	if (m_bUseInverseSquaredFalloff)
	{
		if (m_IntensityUnits == ELightUnits::Candelas)
		{
			// cd はそのまま
		}
		else if (m_IntensityUnits == ELightUnits::Lumens)
		{
			// コーンの立体角 2π(1 - cosθ) で割って cd にする
			LightBrightness *= 1.0f / (2.0f * XM_PI * (1.0f - GetCosHalfConeAngle()));
		}
		else if (m_IntensityUnits == ELightUnits::EV)
		{
			LightBrightness = EV100ToLuminance(LightBrightness);
		}
		else
		{
			LightBrightness *= 1.0f / 625.0f;
		}
	}

	return LightBrightness;
}

void USpotLightComponent::SetLightBrightness(float InBrightness)
{
	if (m_bUseInverseSquaredFalloff && m_IntensityUnits == ELightUnits::Lumens)
	{
		SetIntensity(InBrightness * (2.0f * XM_PI * (1.0f - GetCosHalfConeAngle())));
		return;
	}

	UPointLightComponent::SetLightBrightness(InBrightness);
}

FSphere USpotLightComponent::GetBoundingSphere() const
{
	const float ConeAngle = GetHalfConeAngle();
	return ComputeBoundingSphereForCone(GetComponentLocation(), GetDirection(), m_AttenuationRadius, cosf(ConeAngle), sinf(ConeAngle));
}

void USpotLightComponent::SetInnerConeAngle(float NewInnerConeAngle)
{
	if (NewInnerConeAngle != m_InnerConeAngle)
	{
		m_InnerConeAngle = NewInnerConeAngle;
		MarkRenderStateDirty();
	}
}

void USpotLightComponent::SetOuterConeAngle(float NewOuterConeAngle)
{
	if (NewOuterConeAngle != m_OuterConeAngle)
	{
		m_OuterConeAngle = NewOuterConeAngle;
		MarkRenderStateDirty();
	}
}
