#pragma once
#include "Actor.h"
#include "StaticMeshComponent.h"

// ============================================================
//  AStaticMeshActor
//  UE5 の AStaticMeshActor に相当。UStaticMeshComponent を
//  Root に持つだけの汎用スタティックメッシュ配置アクター。
//  メッシュ / テクスチャ / トランスフォーム / ラベルは
//  レベルロード相当 (GameManager) 側で構成する。
//  旧 ACat / AHorse / ALion / ATable (同型ボイラープレート) は
//  本クラスへ統合した。
// ============================================================

class AStaticMeshActor : public AActor
{
private:
	UStaticMeshComponent* m_StaticMeshComponent = nullptr;

public:
	AStaticMeshActor();

	UStaticMeshComponent* GetStaticMeshComponent() const { return m_StaticMeshComponent; }
};
