#pragma once
#include "StaticMeshActor.h"

// ============================================================
//  AHorse
//  馬の像 (AStaticMeshActor 派生, horse_statue_01_4k)。テーブルの天板の上。
// ============================================================

class AHorse : public AStaticMeshActor
{
public:
	static constexpr const char* MeshPath = "Asset/Model/horse_statue_01_4k.fbx";

	AHorse();
};
