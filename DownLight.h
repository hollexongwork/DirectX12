#pragma once
#include "SpotLight.h"

// ============================================================
//  ADownLight
//  真上からのダウンライト (ASpotLight 派生)。2000 lm / 外角 30 度 / 内角 15 度。
//  向きは ASpotLight の既定 (真下)。
// ============================================================

class ADownLight : public ASpotLight
{
public:
	ADownLight();
};
