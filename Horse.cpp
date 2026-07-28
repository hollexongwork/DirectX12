#include "Main.h"
#include "RenderManager.h"
#include "Horse.h"

AHorse::AHorse()
{
	m_MeshComponent = CreateDefaultSubobject<UStaticMeshComponent>();

	m_MeshComponent->SetStaticMesh("Asset/Model/horse_statue_01_4k.fbx");

	SetActorLocation({ -1.0f, 3.7f, 0.0f });
	SetActorRotation({ 1.57f, 0.0f, 0.0f });
	SetActorScale3D({ 10.0f, 10.0f, 10.0f });

	m_MeshComponent->SetBaseColorTexture(0, "Asset/Texture/horse_statue_01_diff_4k.dds");
	m_MeshComponent->SetNormalTexture(0, "Asset/Texture/horse_statue_01_nor_dx_4k.dds");
	m_MeshComponent->SetARMTexture(0, "Asset/Texture/horse_statue_01_arm_4k.dds");

	Material& material = m_MeshComponent->GetMaterial(0);
	material.Params.BaseColor = { 0.5f, 0.0f, 0.5f, 1.0f };
	material.Params.EmissionColor = { 0.0f, 0.0f, 0.0f, 0.0f };
	material.Params.Metallic = 0.0f;
	material.Params.Specular = 0.0f;
	material.Params.Roughness = 1.0f;
	material.Params.NormalWeight = 1.0f;
	material.Params.Unlit = FALSE;
}
