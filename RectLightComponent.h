#pragma once
#include "LocalLightComponent.h"

// バーンドアの最大開き角 [度] (GetRectLightBarnDoorMaxAngle)。この角度で全開 (ドア無しと同じ)
inline float GetRectLightBarnDoorMaxAngle()
{
	return 88.0f;
}

// ============================================================
//  URectLightComponent
//  URectLightComponent (Engine/Classes/Components/RectLightComponent.h) に相当。
//  矩形の面光源。発光はコンポーネント +Z、幅は +X、高さは +Y。
//  常に逆二乗フォールオフ。
// ============================================================
class URectLightComponent : public ULocalLightComponent
{
protected:
	float m_SourceWidth = 0.64f;		// 発光面の幅 [m] (UE 64 cm)
	float m_SourceHeight = 0.64f;		// 発光面の高さ [m] (UE 64 cm)
	float m_BarnDoorAngle = 88.0f;		// バーンドアの開き角 [度]。88 = 全開
	float m_BarnDoorLength = 0.2f;		// バーンドアの長さ [m] (UE 20 cm)

public:
	ELightComponentType GetLightType() const override { return LightType_Rect; }
	FLightSceneProxy* CreateSceneProxy() const override;

	// Lumens は半球コサイン分布の π で割って cd にする。他は UPointLightComponent と同じ換算
	float ComputeLightBrightness() const override;
	void  SetLightBrightness(float InBrightness);

	void  SetSourceWidth(float NewValue);
	float GetSourceWidth() const { return m_SourceWidth; }

	void  SetSourceHeight(float NewValue);
	float GetSourceHeight() const { return m_SourceHeight; }

	void  SetBarnDoorAngle(float NewValue);
	float GetBarnDoorAngle() const { return m_BarnDoorAngle; }

	void  SetBarnDoorLength(float NewValue);
	float GetBarnDoorLength() const { return m_BarnDoorLength; }
};
