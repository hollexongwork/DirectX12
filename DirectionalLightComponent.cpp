#include "Main.h"
#include "DirectionalLightComponent.h"
#include "DirectionalLightSceneProxy.h"

// ============================================================
//  FDirectionalLightSceneProxy
// ============================================================

FDirectionalLightSceneProxy::FDirectionalLightSceneProxy(const UDirectionalLightComponent* Component)
	: FLightSceneProxy(Component)
	, m_WholeSceneDynamicShadowRadius(Component->GetDynamicShadowDistanceMovableLight())
	, m_DynamicShadowCascades(Component->GetDynamicShadowCascades())
	, m_CascadeDistributionExponent(Component->GetCascadeDistributionExponent())
	, m_ShadowDistanceFadeoutFraction(Component->GetShadowDistanceFadeoutFraction())
	// DF シャドウを使わないライトはカバー距離 0 (UE と同じ)
	, m_DistanceFieldShadowDistance(Component->GetUseRayTracedDistanceFieldShadows() ? Component->GetDistanceFieldShadowDistance() : 0.0f)
	// UE は 1000..1000000 cm にクランプする
	, m_TraceDistance(fmaxf(fminf(Component->GetTraceDistance(), 10000.0f), 10.0f))
	, m_LightSourceAngle(Component->GetLightSourceAngle())
	, m_LightSourceSoftAngle(Component->GetLightSourceSoftAngle())
	, m_ForwardShadingPriority(Component->GetForwardShadingPriority())
{
}

// FDirectionalLightSceneProxy::GetLightShaderParameters 相当
void FDirectionalLightSceneProxy::GetLightShaderParameters(FLightRenderParameters& OutLightParameters) const
{
	const XMFLOAT3 direction = GetDirection();

	OutLightParameters.WorldPosition = { 0.0f, 0.0f, 0.0f };
	OutLightParameters.InvRadius = 0.0f;
	OutLightParameters.Color = GetColor();
	OutLightParameters.FalloffExponent = 0.0f;

	OutLightParameters.Direction = { -direction.x, -direction.y, -direction.z };
	OutLightParameters.Tangent = { -direction.x, -direction.y, -direction.z };

	OutLightParameters.SpotAngles = { 0.0f, 0.0f };
	OutLightParameters.SpecularScale = m_SpecularScale;
	OutLightParameters.DiffuseScale = m_DiffuseScale;
	// 光源は円盤。半径は見かけの半角の sin (単位ベクトル基準)
	OutLightParameters.SourceRadius = sinf(0.5f * XMConvertToRadians(m_LightSourceAngle));
	OutLightParameters.SoftSourceRadius = sinf(0.5f * XMConvertToRadians(m_LightSourceSoftAngle));
	OutLightParameters.SourceLength = 0.0f;
	OutLightParameters.RectLightBarnCosAngle = 0.0f;
	OutLightParameters.RectLightBarnLength = 0.0f;
	OutLightParameters.bAffectsTranslucentLighting = m_bAffectTranslucentLighting ? 1u : 0u;
}

// ============================================================
//  UDirectionalLightComponent
// ============================================================

UDirectionalLightComponent::UDirectionalLightComponent()
{
	m_Intensity = 10.0f;				// 既定 10 lux
	m_bCastVolumetricShadow = true;		// ディレクショナルライトは既定で Volumetric Fog に影を落とす
}

FLightSceneProxy* UDirectionalLightComponent::CreateSceneProxy() const
{
	return new FDirectionalLightSceneProxy(this);
}

// UDirectionalLightComponent::GetLightPosition: 無限遠 (-方向 x WORLD_MAX, w = 0)
XMFLOAT4 UDirectionalLightComponent::GetLightPosition() const
{
	const float WorldMax = 2.0f * HALF_WORLD_MAX;
	const XMFLOAT3 direction = GetDirection();
	return XMFLOAT4(-direction.x * WorldMax, -direction.y * WorldMax, -direction.z * WorldMax, 0.0f);
}

void UDirectionalLightComponent::SetDynamicShadowDistanceMovableLight(float NewValue)
{
	if (m_DynamicShadowDistanceMovableLight != NewValue)
	{
		m_DynamicShadowDistanceMovableLight = NewValue;
		MarkRenderStateDirty();
	}
}

void UDirectionalLightComponent::SetDynamicShadowCascades(int NewValue)
{
	if (m_DynamicShadowCascades != NewValue)
	{
		m_DynamicShadowCascades = NewValue;
		MarkRenderStateDirty();
	}
}

void UDirectionalLightComponent::SetCascadeDistributionExponent(float NewValue)
{
	if (m_CascadeDistributionExponent != NewValue)
	{
		m_CascadeDistributionExponent = NewValue;
		MarkRenderStateDirty();
	}
}

void UDirectionalLightComponent::SetShadowDistanceFadeoutFraction(float NewValue)
{
	if (m_ShadowDistanceFadeoutFraction != NewValue)
	{
		m_ShadowDistanceFadeoutFraction = NewValue;
		MarkRenderStateDirty();
	}
}

void UDirectionalLightComponent::SetDistanceFieldShadowDistance(float NewValue)
{
	if (m_DistanceFieldShadowDistance != NewValue)
	{
		m_DistanceFieldShadowDistance = NewValue;
		MarkRenderStateDirty();
	}
}

void UDirectionalLightComponent::SetTraceDistance(float NewValue)
{
	if (m_TraceDistance != NewValue)
	{
		m_TraceDistance = NewValue;
		MarkRenderStateDirty();
	}
}

void UDirectionalLightComponent::SetLightSourceAngle(float NewValue)
{
	if (m_LightSourceAngle != NewValue)
	{
		m_LightSourceAngle = NewValue;
		MarkRenderStateDirty();
	}
}

void UDirectionalLightComponent::SetLightSourceSoftAngle(float NewValue)
{
	if (m_LightSourceSoftAngle != NewValue)
	{
		m_LightSourceSoftAngle = NewValue;
		MarkRenderStateDirty();
	}
}

void UDirectionalLightComponent::SetForwardShadingPriority(int NewValue)
{
	if (m_ForwardShadingPriority != NewValue)
	{
		m_ForwardShadingPriority = NewValue;
		MarkRenderStateDirty();
	}
}

void UDirectionalLightComponent::SetAtmosphereSunLight(bool bNewValue)
{
	if (m_bAtmosphereSunLight != bNewValue)
	{
		m_bAtmosphereSunLight = bNewValue;
		MarkRenderStateDirty();
	}
}

void UDirectionalLightComponent::SetAtmosphereSunLightIndex(int NewValue)
{
	// 0 = 太陽、1 = 月 (NUM_ATMOSPHERE_LIGHTS)
	const unsigned char clamped = (unsigned char)((NewValue < 0) ? 0 : ((NewValue >= (int)NUM_ATMOSPHERE_LIGHTS) ? (int)NUM_ATMOSPHERE_LIGHTS - 1 : NewValue));
	if (m_AtmosphereSunLightIndex != clamped)
	{
		m_AtmosphereSunLightIndex = clamped;
		MarkRenderStateDirty();
	}
}
