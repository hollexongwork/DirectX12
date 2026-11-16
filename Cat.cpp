#include "Main.h"
#include "Cat.h"

ACat::ACat()
{
	// 基底 AStaticMeshActor が UStaticMeshComponent を Root として
	// 生成済み (bCanEverTick = false)。ここでは既定アセットと
	// マテリアルを焼き込むのみ。
	UStaticMeshComponent* mesh = GetStaticMeshComponent();

	// ---- 既定の配置 (テーブルの天板の上) ----
	SetActorLocation({ -1.0f, 3.75f, 0.0f });
	SetActorRotation({ 1.57f, 0.0f, 0.0f });
	SetActorScale3D({ 7.0f, 7.0f, 7.0f });

	// ---- 既定メッシュ / テクスチャ (FObjectFinder 相当) ----
	mesh->SetStaticMesh(MeshPath);
	mesh->SetBaseColorTexture(0, BaseColorPath);
	mesh->SetNormalTexture(0, NormalPath);
	mesh->SetARMTexture(0, ARMPath);

	// ---- 既定マテリアル (Lit / 粗面コンクリート) ----
	// 旧 ACat と同値。BaseColor はテクスチャ乗算前の定数。
	ApplyDefaultLitMaterial(mesh->GetMaterial(0));

	// GetMaterial() 直接書き換えの反映 (次フレームでプロキシ再生成)
	mesh->MarkRenderStateDirty();
}
