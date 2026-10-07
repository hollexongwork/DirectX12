#include "Main.h"
#include "LightComponentBase.h"

// ============================================================
//  ULightComponentBase
// ============================================================

void ULightComponentBase::SetCastShadows(bool bNewValue)
{
	if (m_CastShadows != bNewValue)
	{
		m_CastShadows = bNewValue;
		MarkRenderStateDirty();
	}
}

void ULightComponentBase::SetCastVolumetricShadow(bool bNewValue)
{
	if (m_bCastVolumetricShadow != bNewValue)
	{
		m_bCastVolumetricShadow = bNewValue;
		MarkRenderStateDirty();
	}
}

void ULightComponentBase::SetAffectGlobalIllumination(bool bNewValue)
{
	if (m_bAffectGlobalIllumination != bNewValue)
	{
		m_bAffectGlobalIllumination = bNewValue;
		MarkRenderStateDirty();
	}
}

void ULightComponentBase::SetAffectsWorld(bool bNewValue)
{
	if (m_bAffectsWorld != bNewValue)
	{
		m_bAffectsWorld = bNewValue;
		MarkRenderStateDirty();
	}
}

void ULightComponentBase::SetCastDynamicShadows(bool bNewValue)
{
	if (m_CastDynamicShadows != bNewValue)
	{
		m_CastDynamicShadows = bNewValue;
		MarkRenderStateDirty();
	}
}

XMFLOAT3 ULightComponentBase::DirectionToRotator(const XMFLOAT3& Direction)
{
	XMFLOAT3 d;
	XMStoreFloat3(&d, XMVector3Normalize(XMLoadFloat3(&Direction)));

	// 前方 +Z 規約: forward = (cosP * sinY, -sinP, cosP * cosY)
	// (XMMatrixRotationRollPitchYaw に行ベクトルを掛けた結果)
	float pitch = asinf(-d.y);
	float yaw = atan2f(d.x, d.z);
	return XMFLOAT3(pitch, yaw, 0.0f);
}
