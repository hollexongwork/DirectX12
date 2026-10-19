#pragma once
#include "StaticMeshActor.h"

// ============================================================
//  ATree
//  AStaticMeshActor 派生の猫の石像 (concrete_cat_statue_4k)。
// ============================================================

class ATree : public AStaticMeshActor
{
public:
	// アセットパス (FObjectFinder のパス指定に相当)
	static constexpr const char* MeshPath = "Asset/Model/branch.fbx";
	static constexpr const char* BaseColorPath = "Asset/Texture/concrete_cat_statue_diff_4k.dds";

	ATree();

	// マテリアル (スロット 0)。直接書き換えた場合は
	// GetStaticMeshComponent()->MarkRenderStateDirty() を呼ぶこと
	Material& GetStatueMaterial() { return GetStaticMeshComponent()->GetMaterial(0); }
};
