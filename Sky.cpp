#include "Main.h"
#include "World.h"
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

	// カメラ追従 (平行移動のみ) のため速度は書かない。深度が 1 にクランプされるので
	// TAA は遠方画素の回転のみ再投影 (d = Q) で正しい動きを再構成する (UE の空は速度を書かない)
	mesh->SetRenderVelocity(false);

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

void ASky::Tick(float DeltaTime)
{
	if (!m_Camera)
	{
		m_Camera = GetWorld()->GetActorOfClass<ACameraActor>();

		if (!m_Camera)
		{
			return;
		}
	}

	XMFLOAT3 pos = m_Camera->GetActorLocation();
	SetActorLocation(pos);
}
