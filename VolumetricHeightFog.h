#pragma once
#include "ExponentialHeightFog.h"

// ============================================================
//  AVolumetricHeightFog
//  シーンの高さフォグ (AExponentialHeightFog 派生)。
//  UE の既定 (bEnableVolumetricFog = false) と違い、Volumetric Fog を有効にして置く
//  (FogVolume の Volume マテリアルはこれが無いと描かれない)。
// ============================================================

class AVolumetricHeightFog : public AExponentialHeightFog
{
public:
	AVolumetricHeightFog();
};
