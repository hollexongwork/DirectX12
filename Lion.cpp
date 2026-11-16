#include "Main.h"
#include "Lion.h"

ALion::ALion()
{
	SetActorLocation({ 2.0f, 3.75f, 1.0f });
	SetActorRotation({ XMConvertToRadians(90.0f), 0.0f, 0.0f });
	SetActorScale3D({ 7.0f, 7.0f, 7.0f });

	UStaticMeshComponent* mesh = GetStaticMeshComponent();
	mesh->SetStaticMesh(MeshPath);
	mesh->SetBaseColorTexture(0, "Asset/Texture/lion_head_diff_4k.dds");
	mesh->SetNormalTexture(0, "Asset/Texture/lion_head_nor_dx_4k.dds");
	mesh->SetARMTexture(0, "Asset/Texture/lion_head_arm_4k.dds");
}
