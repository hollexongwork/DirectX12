#pragma once
#include "Actor.h"
#include "HeterogeneousVolumeComponent.h"

// ============================================================
//  AHeterogeneousVolume
//  AHeterogeneousVolume に相当。UHeterogeneousVolumeComponent を
//  Root に持つ。アクターの位置 / 回転がボリュームの中心と向き、
//  スケールが半幅 [m] (単位ボックス [-1,1]^3 を写す) になる。
//  ImGui の Details からは Root コンポーネントのプロパティを編集し、
//  SettingsManager が [Actor.N] へ永続化する。
// ============================================================

class AHeterogeneousVolume : public AActor
{
private:
	UHeterogeneousVolumeComponent* m_HeterogeneousVolumeComponent = nullptr;

public:
	AHeterogeneousVolume();

	UHeterogeneousVolumeComponent* GetHeterogeneousVolumeComponent() const { return m_HeterogeneousVolumeComponent; }
};
