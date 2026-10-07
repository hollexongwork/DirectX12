#pragma once
#include "LocalLightSceneProxy.h"

class URectLightComponent;

// ============================================================
//  FRectLightSceneProxy
//  FRectLightSceneProxy に相当。
//  矩形の面光源。発光面は前方 (+Z) を向き、幅は +X、高さは +Y。
//  常に逆二乗フォールオフ。
//  シェーダへは SourceRadius = 半幅、SourceLength = 半高、
//  Tangent = 上方向 (高さ軸) で渡す。
// ============================================================
class FRectLightSceneProxy : public FLocalLightSceneProxy
{
public:
	explicit FRectLightSceneProxy(const URectLightComponent* Component);

	bool IsRectLight() const override { return true; }

	void GetLightShaderParameters(FLightRenderParameters& OutLightParameters) const override;

	float GetSourceWidth() const { return m_SourceWidth; }
	float GetSourceHeight() const { return m_SourceHeight; }
	// クランプ済みのバーンドア開き角 [度] (0..88。88 = 全開)
	float GetBarnDoorAngle() const { return m_BarnDoorAngle; }
	float GetBarnDoorLength() const { return m_BarnDoorLength; }

protected:
	float m_SourceWidth = 0.64f;	// [m]
	float m_SourceHeight = 0.64f;	// [m]
	float m_BarnDoorAngle = 88.0f;	// [度]
	float m_BarnDoorLength = 0.2f;	// [m]
};
