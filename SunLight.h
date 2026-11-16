#pragma once
#include "DirectionalLight.h"

// ============================================================
//  ASunLight
//  シーンの太陽 (ADirectionalLight 派生)。
//  方向は旧 FScene 既定値 (3, 5, -3) の逆 = 発光方向 (-3, -5, 3)、強度 3 lux。
// ============================================================

class ASunLight : public ADirectionalLight
{
public:
	ASunLight();
};
