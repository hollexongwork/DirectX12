#include "Main.h"
#include "RenderManager.h"
#include "GameManager.h"
#include "Sky.h"
#include "Camera.h"

ASky::ASky()
{
	// 空ドームはカメラ追従のため Tick を有効化する
	// (基底 AStaticMeshActor は Tick 無効)
	bCanEverTick = true;

	UStaticMeshComponent* mesh = GetStaticMeshComponent();

	mesh->SetStaticMesh("Asset/Model/Dome.fbx");
	mesh->SetBaseColorTexture(0, "Asset/Texture/kloppenheim_06_puresky_4k.dds");

	// 空ドームはシーン全体を覆うためシャドウを落とさない
	mesh->SetCastShadow(false);
	mesh->SetAffectDistanceFieldLighting(false);

	SetActorScale3D({ 10000.0f, 10000.0f, 10000.0f });

	Material& material = mesh->GetMaterial(0);
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
	if (!m_Camera)
	{
		UWorld* world = GameManager::GetInstance()->GetWorld();
		m_Camera = world->GetActorOfClass<ACameraActor>();

		if (!m_Camera)
		{
			return;
		}
	}

	XMFLOAT3 pos = m_Camera->GetActorLocation();
	SetActorLocation(pos);
}
