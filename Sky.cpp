#include "Main.h"
#include "RenderManager.h"
#include "GameManager.h"
#include "Sky.h"
#include "Camera.h"

ASky::ASky()
{
	m_MeshComponent = CreateDefaultSubobject<UStaticMeshComponent>();

	m_MeshComponent->SetStaticMesh("Asset/Model/Dome.fbx");
	m_MeshComponent->SetBaseColorTexture(0, "Asset/Texture/kloppenheim_06_puresky_4k.dds");

	// 空ドームはシーン全体を覆うためシャドウを落とさない
	m_MeshComponent->SetCastShadow(false);
	m_MeshComponent->SetAffectDistanceFieldLighting(false);

	SetActorScale3D({ 10000.0f, 10000.0f, 10000.0f });

	Material& material = m_MeshComponent->GetMaterial(0);
	material.Params.BaseColor = { 1.0f, 1.0f, 1.0f, 1.0f };
	material.Params.EmissionColor = { 0.0f, 0.0f, 0.0f, 0.0f };
	material.Params.Metallic = 0.0f;
	material.Params.Specular = 0.0f;
	material.Params.Roughness = 0.0f;
	material.Params.NormalWeight = 0.0f;
	material.Params.Unlit = TRUE;
}

void ASky::BeginPlay()
{
	UWorld* world = GameManager::GetInstance()->GetWorld();
	m_Camera = world->GetActorOfClass<ACameraActor>();
}

void ASky::Tick(float DeltaTime)
{
	XMFLOAT3 pos = m_Camera->GetActorLocation();
	SetActorLocation(pos);
}
