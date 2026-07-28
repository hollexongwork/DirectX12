#pragma once
#include "Light.h"

// ============================================================
//  ARectLight
//  URectLightComponent を Root に持つ。
//  発光面はアクターの前方 (+Z) を向き、幅は +X / 高さは +Y。
//  バーンドア (BarnDoorAngle / BarnDoorLength) で照射範囲を絞れる。
// ============================================================

class ARectLight : public ALight
{
private:
	URectLightComponent* m_RectLightComponent = nullptr;

public:
	ARectLight();

	URectLightComponent* GetRectLightComponent() const { return m_RectLightComponent; }
};
