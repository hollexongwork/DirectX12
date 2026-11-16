#include "Main.h"
#include "FogVolume.h"
#include "ProceduralMeshGenerator.h"

AFogVolume::AFogVolume()
{
	SetActorLocation({ 0.0f, 4.6f, 0.0f });
	SetActorScale3D({ 4.5f, 0.9f, 2.2f });

	UStaticMeshComponent* mesh = GetStaticMeshComponent();
	FProceduralMeshSection box = FProceduralMeshBuilder::CreateBox({ 1.0f, 1.0f, 1.0f });
	box.bAffectDistanceField = false;	// 媒質なので SDF には入れない
	mesh->SetProceduralMesh("FogVolumeUnitBox", FProceduralMeshData::FromSection(box));
	mesh->SetCastShadow(false);
	mesh->SetAffectDistanceFieldLighting(false);

	Material& material = mesh->GetMaterial(0);
	material.SetMaterialDomain(EMaterialDomain::MD_Volume);
	material.SetVolumeShape(EVolumeShape::Box);
	material.VolumeParams.Albedo = { 1.0f, 1.0f, 1.0f, 1.0f };
	material.VolumeParams.Emissive = { 0.0f, 0.0f, 0.0f, 0.0f };
	// グラフの既定 (Octaves 3 / Lacunarity 2 / Gain 0.5 / x1.5 / Power 5)。
	// ノイズ格子 (1 / Scale) は霧の厚み (1.8 m) と同程度にする。視線が体積を通る間に
	// 何周期も平均されると模様が消えるため
	material.VolumeParams.ExtinctionParams = { 4.0f, 1.5f, 5.0f, 0.3f };		// Extinction / Multiply / Power / EdgeFalloff
	material.VolumeParams.NoiseScale = { 0.5f, 0.5f, 0.5f, 0.5f };			// Scale [1/m] (格子 2 m) / Gain
	material.VolumeParams.NoiseOffsetSpeed = { 0.3f, 0.05f, 0.15f, 0.2f };	// OffsetSpeed [m/s] / EvolutionSpeed
	material.VolumeParams.NoiseParams = { 3.0f, 2.0f, 4.0f, 1.0f };			// Octaves / Lacunarity / 4D (Evolution) / UseNoise
	mesh->MarkRenderStateDirty();	// GetMaterial() 直接書き換えの反映
}
