#include "Main.h"
#include "Horse.h"

AHorse::AHorse()
{
	SetActorLocation({ -1.0f, 3.7f, 0.0f });
	SetActorRotation({ 1.57f, 0.0f, 0.0f });
	SetActorScale3D({ 10.0f, 10.0f, 10.0f });

	UStaticMeshComponent* mesh = GetStaticMeshComponent();
	mesh->SetStaticMesh(MeshPath);
	mesh->SetBaseColorTexture(0, "Asset/Texture/horse_statue_01_diff_4k.dds");
	mesh->SetNormalTexture(0, "Asset/Texture/horse_statue_01_nor_dx_4k.dds");
	mesh->SetARMTexture(0, "Asset/Texture/horse_statue_01_arm_4k.dds");

	ApplyDefaultLitMaterial(mesh->GetMaterial(0));
	mesh->MarkRenderStateDirty();	// GetMaterial() 直接書き換えの反映
}
