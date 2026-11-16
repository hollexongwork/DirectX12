#pragma once
#include "StaticMeshActor.h"

// ============================================================
//  ALion
//  ライオンの頭像 (AStaticMeshActor 派生, lion_head_4k)。テーブルの天板の上。
//  マテリアルはスロットの既定値のまま。
// ============================================================

class ALion : public AStaticMeshActor
{
public:
	static constexpr const char* MeshPath = "Asset/Model/lion_head_4k.fbx";

	ALion();
};
