#pragma once
#include "StaticMeshActor.h"

// ============================================================
//  ACat
//  AStaticMeshActor 派生の猫の石像 (concrete_cat_statue_4k)。
// ============================================================

class ACat : public AStaticMeshActor
{
public:
	// アセットパス (FObjectFinder のパス指定に相当)
	static constexpr const char* MeshPath = "Asset/Model/concrete_cat_statue_4k.fbx";
	static constexpr const char* BaseColorPath = "Asset/Texture/concrete_cat_statue_diff_4k.dds";
	static constexpr const char* NormalPath = "Asset/Texture/concrete_cat_statue_nor_dx_4k.dds";
	static constexpr const char* ARMPath = "Asset/Texture/concrete_cat_statue_arm_4k.dds";

	ACat();

	// マテリアル (スロット 0)。直接書き換えた場合は
	// GetStaticMeshComponent()->MarkRenderStateDirty() を呼ぶこと
	Material& GetStatueMaterial() { return GetStaticMeshComponent()->GetMaterial(0); }
};
