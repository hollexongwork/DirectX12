#pragma once
#include "Actor.h"
#include "LightComponent.h"

// ============================================================
//  ALight
//  ライトアクター共通基底。
//  ULightComponent を Root に持ち、強度 / 色 / 有効化の
//  ユーティリティをコンポーネントへ委譲する。
//  派生: ADirectionalLight / APointLight / ASpotLight / ARectLight
//  (ASpotLight は APointLight ではなく ALight 直下)
// ============================================================

class ALight : public AActor
{
protected:
	ULightComponent* m_LightComponent = nullptr;

public:
	ALight()
	{
		bCanEverTick = false;	// ライトは Tick 不要
	}

	ULightComponent* GetLightComponent() const { return m_LightComponent; }

	// ---- ユーティリティ ----
	void  SetEnabled(bool bEnabled);
	bool  IsEnabled() const;
	void  SetBrightness(float Brightness);
	float GetBrightness() const;
	void  SetLightColor(const XMFLOAT4& Color);
	XMFLOAT4 GetLightColor() const;
};
