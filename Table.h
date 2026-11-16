#pragma once
#include "StaticMeshActor.h"

// ============================================================
//  ATable
//  ピクニックテーブル (AStaticMeshActor 派生, wooden_picnic_table_4k)。
//  スロット 0 = 脚 / 1 = 天板。シーン中央に 5 倍で置く
//  (天板の高さ ≒ 3.7 m、ベンチまで含めて ±8 x ±3.5 m)。
// ============================================================

class ATable : public AStaticMeshActor
{
public:
	static constexpr const char* MeshPath = "Asset/Model/wooden_picnic_table_4k.fbx";

	ATable();
};
