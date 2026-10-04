#pragma once
#include "LightSceneProxy.h"

class ULocalLightComponent;

// ============================================================
//  FLocalLightSceneProxy
//  FLocalLightSceneProxy に相当。
//  減衰半径を持つライト (Point / Spot / Rect) の共通基底。
// ============================================================
class FLocalLightSceneProxy : public FLightSceneProxy
{
public:
	explicit FLocalLightSceneProxy(const ULocalLightComponent* Component);

	// 影を落とさないライトの半径変更の軽量経路 (ULocalLightComponent::PushRadiusToRenderThread)
	void UpdateRadius_GameThread(float ComponentRadius) { UpdateRadius(ComponentRadius); }

	float GetMaxDrawDistance() const final override { return m_MaxDrawDistance; }
	// MaxDrawDistance が 0 (無制限) ならフェードもしない
	float GetFadeRange() const final override { return (m_MaxDrawDistance == 0.0f) ? 0.0f : m_FadeRange; }

	float GetRadius() const override { return m_Radius; }
	float GetInvRadius() const { return m_InvRadius; }

	bool    AffectsBounds(const FBoxSphereBounds& Bounds) const override;
	FSphere GetBoundingSphere() const override;
	bool    IsLocalLight() const override { return true; }

protected:
	// 半径とその逆数を更新する
	void UpdateRadius(float ComponentRadius);

	float m_Radius = 0.0f;			// 減衰半径 [m]
	float m_InvRadius = 0.0f;
	float m_MaxDrawDistance = 0.0f;	// [m] (0 = 無制限)
	float m_FadeRange = 0.0f;		// [m]
};
