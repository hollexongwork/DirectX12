#pragma once
#include "RectLight.h"

// ============================================================
//  APanelLight
//  寒色のパネルライト (ARectLight 派生)。1500 lm / 1.5 x 1.0 m / 9000 K。
//  -Z を向けてシーン中央へ当てる。
// ============================================================

class APanelLight : public ARectLight
{
public:
	APanelLight();
};
