#pragma once
#include "HeterogeneousVolume.h"

// ============================================================
//  ACloud
//  単体の積雲 (AHeterogeneousVolume 派生, ソース = ProceduralCloud)。
//  密度はプロシージャル (球群 + Worley / Perlin) をコンピュートで Texture3D へベイクし、
//  太陽 + 天空光でレイマーチする。スケール = 半幅 [m] (16 x 7 x 11 m)。
// ============================================================

class ACloud : public AHeterogeneousVolume
{
public:
	ACloud();
};
