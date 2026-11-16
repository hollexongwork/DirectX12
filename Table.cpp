#include "Main.h"
#include "Table.h"

ATable::ATable()
{
	SetActorLocation({ 0.0f, 0.0f, 0.0f });
	SetActorRotation({ XMConvertToRadians(90.0f), XMConvertToRadians(90.0f), 0.0f });
	SetActorScale3D({ 5.0f, 5.0f, 5.0f });

	UStaticMeshComponent* mesh = GetStaticMeshComponent();
	mesh->SetStaticMesh(MeshPath);
	mesh->SetNumMaterialSlots(2);

	mesh->SetBaseColorTexture(0, "Asset/Texture/wooden_picnic_table_bottom_diff_4k.dds");
	mesh->SetNormalTexture(0, "Asset/Texture/wooden_picnic_table_bottom_nor_dx_4k.dds");
	mesh->SetARMTexture(0, "Asset/Texture/wooden_picnic_table_bottom_arm_4k.dds");

	mesh->SetBaseColorTexture(1, "Asset/Texture/wooden_picnic_table_top_diff_4k.dds");
	mesh->SetNormalTexture(1, "Asset/Texture/wooden_picnic_table_top_nor_dx_4k.dds");
	mesh->SetARMTexture(1, "Asset/Texture/wooden_picnic_table_top_arm_4k.dds");

	for (unsigned int i = 0; i < 2; ++i)
	{
		ApplyDefaultLitMaterial(mesh->GetMaterial(i));
	}
	mesh->MarkRenderStateDirty();	// GetMaterial() 直接書き換えの反映
}
