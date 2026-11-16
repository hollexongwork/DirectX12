#include "Main.h"
#include "ProceduralChain.h"
#include "ProceduralSkeletalMesh.h"
#include "AssetManager.h"

AProceduralChain::AProceduralChain()
{
	SetActorLocation({ -6.0f, 0.0f, -8.0f });

	// ---- アセット (初回だけ生成して FAssetManager へ登録) ----
	FAssetManager* assets = FAssetManager::GetInstance();
	std::shared_ptr<USkeletalMesh> mesh = assets->FindSkeletalMesh(MeshName);
	std::shared_ptr<UAnimSequence> wave = assets->FindAnimSequence(WaveSequenceName);
	if (!mesh || !wave)
	{
		FProceduralSkeletalChainParams chain;
		FSkeletalMeshImportData importData;
		FProceduralSkeletalMeshBuilder::BuildChain(chain, importData);

		mesh = assets->CreateSkeletalMesh(MeshName, importData);
		wave = FProceduralSkeletalMeshBuilder::CreateWaveSequence(mesh->GetSkeleton(), chain);
		std::shared_ptr<UAnimSequence> idle = FProceduralSkeletalMeshBuilder::CreateIdleSequence(mesh->GetSkeleton(), chain);
		assets->RegisterAnimSequence(WaveSequenceName, wave);
		assets->RegisterAnimSequence(IdleSequenceName, idle);
	}

	// ---- メッシュ / マテリアル ----
	USkeletalMeshComponent* component = GetSkeletalMeshComponent();
	component->SetSkeletalMesh(mesh);
	component->SetBaseColorTexture(0, "Asset/Texture/lion_head_diff_4k.dds");
	component->SetNormalTexture(0, "Asset/Texture/lion_head_nor_dx_4k.dds");
	component->SetARMTexture(0, "Asset/Texture/lion_head_arm_4k.dds");
	{
		Material& material = component->GetMaterial(0);
		material.Params.BaseColor = { 0.85f, 0.6f, 0.35f, 1.0f };
		material.Params.Metallic = 0.0f;
		material.Params.Specular = 0.5f;
		material.Params.Roughness = 0.7f;
		component->MarkRenderStateDirty();
	}

	// ---- 単一ノード再生 (登録時の InitAnim が保存済みの再生設定を引き継ぐ) ----
	component->PlayAnimation(wave, true);
}
