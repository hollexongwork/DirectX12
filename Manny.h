#pragma once
#include "SkeletalMeshActor.h"

// ============================================================
//  AManny
//  ASkeletalMeshActor 派生のマネキン (SKM_Manny.FBX)。
//  ACat と同じく、アセットパスと既定マテリアル / 既定トランスフォームを
//  コンストラクタで焼き込む (FObjectFinder のパス指定に相当)。
//
//  UE のアセットは cm 単位 / Z-up。assimp の aiProcess_ConvertToLeftHanded
//  後は「上」が -Z になるため、既定で
//    スケール 0.01 (cm -> m)、回転 (ピッチ +90 度, ヨー 180 度, ロール 0)
//  を与えて Y-up / 正面 -Z に合わせる (インポータは軸 / 単位を変換しない)。
//  INI ([Actor.N]) に保存されたトランスフォームがあればそちらが上書きする。
//
//  SKM_Manny.FBX 自体にはアニメーションが無い。別ファイルのアニメーションは
//  PlayAnimationFromFile (ボーン名が一致するスケルトンのもの) で再生できる。
// ============================================================

class AManny : public ASkeletalMeshActor
{
public:
	static constexpr const char* MeshPath = "Asset/Model/SKM_Manny.FBX";

	// cm -> m
	static constexpr float ImportUniformScale = 0.01f;

	AManny();

	// 既定マテリアル (全スロット) をコンストラクタの値に戻す (MarkRenderStateDirty 込み)
	void ApplyDefaultMaterials();

	// 別の FBX / glTF の Index 番目のアニメーションを読み込んでループ再生する。
	// スケルトンが一致しない / 無ければ false
	bool PlayAnimationFromFile(const char* FilePath, unsigned int Index = 0, bool bLooping = true);
};
