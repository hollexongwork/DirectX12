#include "Main.h"
#include "Manny.h"
#include "AssetManager.h"

AManny::AManny()
{
	// 基底 ASkeletalMeshActor が USkeletalMeshComponent を Root として生成済み。
	USkeletalMeshComponent* mesh = GetSkeletalMeshComponent();

	// ---- 既定メッシュ (FAssetManager のキャッシュで共有) ----
	mesh->SetSkeletalMesh(MeshPath);

	// ---- 既定トランスフォーム: cm / Z-up -> m / Y-up, 正面 -Z ----
	SetActorRotation({ XMConvertToRadians(90.0f), XMConvertToRadians(180.0f), 0.0f });
	SetActorScale3D({ ImportUniformScale, ImportUniformScale, ImportUniformScale });

	ApplyDefaultMaterials();
}

void AManny::ApplyDefaultMaterials()
{
	USkeletalMeshComponent* mesh = GetSkeletalMeshComponent();

	// マネキンの素材 (テクスチャ無しの灰色のプラスチック)
	for (unsigned int i = 0; i < mesh->GetNumMaterialSlots(); ++i)
	{
		Material& material = mesh->GetMaterial(i);
		material.Params.BaseColor = { 0.6f, 0.62f, 0.65f, 1.0f };
		material.Params.EmissionColor = { 0.0f, 0.0f, 0.0f, 0.0f };
		material.Params.Metallic = 0.0f;
		material.Params.Specular = 0.5f;
		material.Params.Roughness = 0.6f;
		material.Params.NormalWeight = 1.0f;
		material.Params.Unlit = FALSE;
	}

	// GetMaterial() 直接書き換えの反映 (次フレームでプロキシ再生成)
	mesh->MarkRenderStateDirty();
}

bool AManny::PlayAnimationFromFile(const char* FilePath, unsigned int Index, bool bLooping)
{
	USkeletalMeshComponent* mesh = GetSkeletalMeshComponent();
	const std::shared_ptr<USkeletalMesh>& skeletalMesh = mesh->GetSkeletalMesh();
	if (!skeletalMesh || !skeletalMesh->IsLoaded())
	{
		return false;
	}

	std::shared_ptr<UAnimSequence> animation =
		FAssetManager::GetInstance()->LoadAnimSequence(FilePath, Index, skeletalMesh->GetSkeleton());
	if (!animation)
	{
		char msg[512];
		sprintf_s(msg, "[AManny] animation %u not found in %s\n", Index, FilePath);
		OutputDebugStringA(msg);
		return false;
	}

	mesh->PlayAnimation(animation, bLooping);
	return mesh->GetCurrentAnimation() == animation;
}
