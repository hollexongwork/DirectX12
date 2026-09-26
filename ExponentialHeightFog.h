#pragma once
#include "Actor.h"
#include "ExponentialHeightFogComponent.h"

// ============================================================
//  AExponentialHeightFog
//  AExponentialHeightFog (Engine/Classes/Engine/ExponentialHeightFog.h)
//  に相当。UExponentialHeightFogComponent を Root に持ち、
//  アクター位置の Y がフォグの基準高さになる。
//  bEnabled はコンポーネントの可視性 (FScene への登録) に委譲する。
//  ImGui の Details からは Root コンポーネントのプロパティを編集し、
//  SettingsManager が [Actor.N] へ永続化する。
// ============================================================

class AExponentialHeightFog : public AActor
{
private:
	UExponentialHeightFogComponent* m_FogComponent = nullptr;

	bool m_bEnabled = true;

public:
	AExponentialHeightFog();

	UExponentialHeightFogComponent* GetComponent() const { return m_FogComponent; }

	void SetEnabled(bool bEnabled);
	bool IsEnabled() const { return m_bEnabled; }
};
