#include "Main.h"
#include "LocalLightSceneProxy.h"
#include "LocalLightComponent.h"

FLocalLightSceneProxy::FLocalLightSceneProxy(const ULocalLightComponent* Component)
	: FLightSceneProxy(Component)
	, m_MaxDrawDistance(Component->GetMaxDrawDistance())
	, m_FadeRange(Component->GetMaxDistanceFadeRange())
{
	UpdateRadius(Component->GetAttenuationRadius());
}

// 境界球が減衰半径の球と交差するか
bool FLocalLightSceneProxy::AffectsBounds(const FBoxSphereBounds& Bounds) const
{
	const XMFLOAT3 origin = GetOrigin();
	const float dx = Bounds.Origin.x - origin.x;
	const float dy = Bounds.Origin.y - origin.y;
	const float dz = Bounds.Origin.z - origin.z;
	const float reach = m_Radius + Bounds.SphereRadius;

	if (dx * dx + dy * dy + dz * dz > reach * reach)
	{
		return false;
	}

	return FLightSceneProxy::AffectsBounds(Bounds);
}

FSphere FLocalLightSceneProxy::GetBoundingSphere() const
{
	return FSphere(GetOrigin(), GetRadius());
}

void FLocalLightSceneProxy::UpdateRadius(float ComponentRadius)
{
	m_Radius = ComponentRadius;

	// 0 除算 (InvRadius の NaN) を避ける下限
	m_InvRadius = 1.0f / fmaxf(1.0e-8f, ComponentRadius);
}
