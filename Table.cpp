#include "Main.h"
#include "RenderManager.h"
#include "Table.h"

ATable::ATable()
{
	m_MeshComponent = CreateDefaultSubobject<UStaticMeshComponent>();

	m_MeshComponent->SetStaticMesh("Asset/Model/wooden_picnic_table_4k.fbx");
	m_MeshComponent->SetNumMaterialSlots(2);

	SetActorLocation({ 0.0f, 0.0f, 0.0f });
	SetActorRotation({ XMConvertToRadians(90.0f), XMConvertToRadians(90.0f), 0.0f });
	SetActorScale3D({ 5.0f, 5.0f, 5.0f });

	m_MeshComponent->SetBaseColorTexture(0, "Asset/Texture/wooden_picnic_table_bottom_diff_4k.dds");
	m_MeshComponent->SetNormalTexture(0, "Asset/Texture/wooden_picnic_table_bottom_nor_dx_4k.dds");
	m_MeshComponent->SetARMTexture(0, "Asset/Texture/wooden_picnic_table_bottom_arm_4k.dds");

	m_MeshComponent->SetBaseColorTexture(1, "Asset/Texture/wooden_picnic_table_top_diff_4k.dds");
	m_MeshComponent->SetNormalTexture(1, "Asset/Texture/wooden_picnic_table_top_nor_dx_4k.dds");
	m_MeshComponent->SetARMTexture(1, "Asset/Texture/wooden_picnic_table_top_arm_4k.dds");

	for (unsigned int i = 0; i < 2; ++i)
	{
		Material& material = m_MeshComponent->GetMaterial(i);
		material.Params.BaseColor = { 0.5f, 0.0f, 0.5f, 1.0f };
		material.Params.EmissionColor = { 0.0f, 0.0f, 0.0f, 0.0f };
		material.Params.Metallic = 0.0f;
		material.Params.Specular = 0.0f;
		material.Params.Roughness = 1.0f;
		material.Params.NormalWeight = 1.0f;
		material.Params.Unlit = FALSE;
	}
}
