#include "Main.h"
#include "Tree.h"

ATree::ATree()
{
	// 基底 AStaticMeshActor が UStaticMeshComponent を Root として
	// 生成済み (bCanEverTick = false)。ここでは既定アセットと
	// マテリアルを焼き込むのみ。
	UStaticMeshComponent* mesh = GetStaticMeshComponent();

	// ---- 既定メッシュ / テクスチャ (FObjectFinder 相当) ----
	mesh->SetStaticMesh(MeshPath);
	mesh->SetBaseColorTexture(0, BaseColorPath);

	// ---- 既定マテリアル (Lit / 粗面コンクリート) ----
	// 旧 ACat と同値。BaseColor はテクスチャ乗算前の定数。
	Material& material = mesh->GetMaterial(0);
	material.Params.BaseColor = { 0.5f, 0.0f, 0.5f, 1.0f };
	material.Params.EmissionColor = { 0.0f, 0.0f, 0.0f, 0.0f };
	material.Params.Metallic = 0.0f;
	material.Params.Specular = 0.0f;
	material.Params.Roughness = 1.0f;
	material.Params.NormalWeight = 1.0f;
	material.Params.Unlit = FALSE;

	// GetMaterial() 直接書き換えの反映 (次フレームでプロキシ再生成)
	mesh->MarkRenderStateDirty();
}
