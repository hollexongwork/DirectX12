#pragma once
#include <memory>
#include "SkeletalMesh.h"
#include "AnimSequence.h"

// ============================================================
//  FProceduralSkeletalMeshBuilder
//  テスト / デモ用のプロシージャルなリグ付きメッシュとアニメーション。
//  リポジトリにリグ付きアセットが無いため、スケルタルメッシュ経路
//  (USkeletalMesh::Build / UAnimSequence / GPU スキニング) を
//  決定的に検証する入力として使う (UE には対応物が無い)。
//
//  チェーン: Y 軸に沿った NumBones 本のボーン (Root, Bone1, ...)。
//  Bone_i のリファレンスポーズは親から (0, BoneLength, 0)。
//  メッシュは y = 0..NumBones * BoneLength の先細りの筒 + 両端のキャップで、
//  関節 (y = j * BoneLength) の前後 BlendHalfWidth で隣接ボーンの重みを
//  smoothstep でブレンドする。UV: u = 周方向 (接線 = +U の規約),
//  v = 高さ方向。
// ============================================================

struct FProceduralSkeletalChainParams
{
	int          NumBones = 6;
	float        BoneLength = 0.5f;		// [m]
	float        Radius = 0.18f;		// 根元の半径 [m]
	float        TipRadiusScale = 0.45f;	// 先端の半径 = Radius * TipRadiusScale
	unsigned int Slices = 24;			// 周方向分割
	unsigned int RingsPerBone = 6;		// ボーンあたりのリング数
	float        BlendHalfWidth = 0.12f;	// 関節前後のブレンド幅 [m]
	XMFLOAT2     UVScale = { 1.0f, 2.0f };	// (周方向, 高さ方向) のタイル数
};

namespace FProceduralSkeletalMeshBuilder
{
	// チェーンのリグ付きメッシュ (1 セクション / MaterialIndex 0)
	void BuildChain(const FProceduralSkeletalChainParams& Params, FSkeletalMeshImportData& OutData);

	// 「波打つ」アニメーション: Bone_i (i >= 1) を Z 軸まわりに
	//   AmplitudeDeg * sin(2 pi t / Length + i * PhaseStepRad) で曲げ、
	//   X 軸まわりにも 0.5 倍の振幅で位相の異なる揺れを足す。先頭と末尾のキーが一致する (ループ可)
	std::shared_ptr<UAnimSequence> CreateWaveSequence(const std::shared_ptr<USkeleton>& Skeleton,
		const FProceduralSkeletalChainParams& Params, const std::string& Name = "Wave",
		float Length = 2.0f, float AmplitudeDeg = 30.0f, float PhaseStepRad = 0.6f, float FrameRate = 30.0f);

	// ゆっくり小さく揺れる待機アニメーション (CreateWaveSequence の小振幅版)
	std::shared_ptr<UAnimSequence> CreateIdleSequence(const std::shared_ptr<USkeleton>& Skeleton,
		const FProceduralSkeletalChainParams& Params, const std::string& Name = "Idle",
		float Length = 4.0f, float AmplitudeDeg = 6.0f, float FrameRate = 30.0f);
}
