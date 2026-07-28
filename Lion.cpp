#include "Main.h"
#include "RenderManager.h"
#include "Lion.h"

ALion::ALion()
{
	m_MeshComponent = CreateDefaultSubobject<UStaticMeshComponent>();

	m_MeshComponent->SetStaticMesh("Asset/Model/lion_head_4k.fbx");

	SetActorLocation({ 2.0f, 3.75f, 1.0f });
	SetActorRotation({ XMConvertToRadians(90.0f), 0.0f, 0.0f });
	SetActorScale3D({ 7.0f, 7.0f, 7.0f });

	m_MeshComponent->SetBaseColorTexture(0, "Asset/Texture/lion_head_diff_4k.dds");
	m_MeshComponent->SetNormalTexture(0, "Asset/Texture/lion_head_nor_dx_4k.dds");
	m_MeshComponent->SetARMTexture(0, "Asset/Texture/lion_head_arm_4k.dds");

	// マテリアルはスロット既定値 (旧 Lion と同じくデフォルトのまま)
}
