#pragma once
#include "StaticMeshActor.h"

// ============================================================
//  AFogVolume
//  Material Domain = Volume のデモ (AStaticMeshActor 派生)。
//  単位ボックスのプロシージャルメッシュに Volume マテリアルを載せる。メッシュ自体は
//  どのサーフェスパスにも描かれず、境界 (OBB) が Volumetric Fog へボクセル化される
//  (AVolumetricHeightFog など Volumetric Fog が有効なフォグが必要)。
//  アクターのスケール = 半幅 [m]。テーブルの天板 (y = 3.7 m) の上に 9 x 1.8 x 4.4 m。
// ============================================================

class AFogVolume : public AStaticMeshActor
{
public:
	AFogVolume();
};
