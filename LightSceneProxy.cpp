#include "Main.h"
#include "LightSceneProxy.h"
#include "LightComponent.h"

// ------------------------------------------------------------
//  共通プロパティのスナップショット (FLightSceneProxy::FLightSceneProxy)。
//  GetLightType() / GetColoredLightBrightness() は完全に構築済みの
//  コンポーネントに対する仮想呼び出しなので、種別ごとの
//  ComputeLightBrightness (単位換算) が正しくディスパッチされる。
//  トランスフォームは FScene::AddLight が直後に SetTransform で入れる。
// ------------------------------------------------------------
FLightSceneProxy::FLightSceneProxy(const ULightComponent* InLightComponent)
	: m_LightComponent(InLightComponent)
	, m_IndirectLightingScale(InLightComponent->GetIndirectLightingIntensity())
	, m_VolumetricScatteringIntensity(fmaxf(InLightComponent->GetVolumetricScatteringIntensity(), 0.0f))
	, m_SpecularScale(InLightComponent->GetSpecularScale())
	, m_DiffuseScale(InLightComponent->GetDiffuseScale())
	, m_ShadowBias(InLightComponent->GetShadowBias())
	, m_ShadowSlopeBias(InLightComponent->GetShadowSlopeBias())
	, m_ContactShadowLength(InLightComponent->GetContactShadowLength())
	, m_ContactShadowCastingIntensity(InLightComponent->GetContactShadowCastingIntensity())
	, m_ContactShadowNonCastingIntensity(InLightComponent->GetContactShadowNonCastingIntensity())
	, m_bContactShadowLengthInWS(InLightComponent->GetContactShadowLengthInWS())
	, m_bCastDynamicShadow(InLightComponent->GetCastShadows() && InLightComponent->GetCastDynamicShadows())
	, m_bCastVolumetricShadow(InLightComponent->GetCastVolumetricShadow())
	, m_bAffectGlobalIllumination(InLightComponent->GetAffectGlobalIllumination())
	, m_bAffectTranslucentLighting(InLightComponent->GetAffectTranslucentLighting())
	, m_bUseRayTracedDistanceFieldShadows(InLightComponent->GetUseRayTracedDistanceFieldShadows())
	, m_bUsedAsAtmosphereSunLight(InLightComponent->IsUsedAsAtmosphereSunLight())
	, m_AtmosphereSunLightIndex(InLightComponent->GetAtmosphereSunLightIndex())
	, m_LightType((unsigned char)InLightComponent->GetLightType())
{
	XMStoreFloat4x4(&m_LightToWorld, XMMatrixIdentity());
	XMStoreFloat4x4(&m_WorldToLight, XMMatrixIdentity());

	// 色 x 明るさ (単位換算 / 色温度込み)
	SetColor(InLightComponent->GetColoredLightBrightness());
}

FLightSceneProxy::~FLightSceneProxy() = default;

// FLightSceneProxy::SetTransform 相当
void FLightSceneProxy::SetTransform(const XMFLOAT4X4& InLightToWorld, const XMFLOAT4& InPosition)
{
	m_LightToWorld = InLightToWorld;
	XMStoreFloat4x4(&m_WorldToLight, XMMatrixInverse(nullptr, XMLoadFloat4x4(&InLightToWorld)));
	m_Position = InPosition;
}
