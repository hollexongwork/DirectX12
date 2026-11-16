#pragma once
#include "SkeletalMeshActor.h"

// ============================================================
//  AProceduralChain
//  プロシージャルなチェーンのリグ (ASkeletalMeshActor 派生)。
//  リグ付きアセットがリポジトリに無いので、FProceduralSkeletalMeshBuilder の
//  チェーン + 波打つ / 待機のアニメーションをアセットとして FAssetManager へ登録し
//  (名前で共有されるので 2 体目以降は再利用)、UE と同じ単一ノード再生
//  (PlayAnimation) で波打たせる。
// ============================================================

class AProceduralChain : public ASkeletalMeshActor
{
public:
	static constexpr const char* MeshName = "ProceduralChain";
	static constexpr const char* WaveSequenceName = "ProceduralChain#Wave";
	static constexpr const char* IdleSequenceName = "ProceduralChain#Idle";

	AProceduralChain();
};
