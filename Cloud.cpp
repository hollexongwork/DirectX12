#include "Main.h"
#include "Cloud.h"

ACloud::ACloud()
{
	SetActorLocation({ 3.0f, 14.0f, 14.0f });
	SetActorScale3D({ 8.0f, 3.5f, 5.5f });

	UHeterogeneousVolumeComponent* volume = GetHeterogeneousVolumeComponent();
	volume->SetVolumeSource(EHeterogeneousVolumeSource::ProceduralCloud);
	volume->SetVolumeResolution({ 256, 128, 192 });	// 6 cm ボクセル (密度 12 MB + キャッシュ 6 MB)

	// このシーンの太陽 (0.5 lux) は空のドーム (無光のテクスチャ) に対して暗く、物理どおりだと
	// 雲が空より暗い灰色になる。日なたが空より明るく見えるよう雲の側で太陽光を持ち上げる
	FHeterogeneousVolumeMaterial material = volume->GetVolumeMaterial();
	material.DirectionalLightIntensityScale = 10.0f;
	volume->SetVolumeMaterial(material);
}
