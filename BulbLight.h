#pragma once
#include "PointLight.h"

// ============================================================
//  ABulbLight
//  暖色の電球 (APointLight 派生)。800 lm (≒ 60 W 白熱球) / 3000 K。
// ============================================================

class ABulbLight : public APointLight
{
public:
	ABulbLight();
};
