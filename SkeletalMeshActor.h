#pragma once
#include "Actor.h"
#include "SkeletalMeshComponent.h"

// ============================================================
//  ASkeletalMeshActor
//  ASkeletalMeshActor (Animation/SkeletalMeshActor.h) に相当。
//  USkeletalMeshComponent を Root に持つだけの配置アクター。
//  メッシュ / アニメーション / テクスチャ / トランスフォーム / ラベルは
//  レベルロード相当 (GameManager) 側か、派生クラス (AManny) で構成する。
//  アニメーションの更新はコンポーネントの TickComponent が行う
//  (アクター自身は Tick しない)。
// ============================================================

class ASkeletalMeshActor : public AActor
{
private:
	USkeletalMeshComponent* m_SkeletalMeshComponent = nullptr;

public:
	ASkeletalMeshActor();

	USkeletalMeshComponent* GetSkeletalMeshComponent() const { return m_SkeletalMeshComponent; }
};
