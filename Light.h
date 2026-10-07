#pragma once
#include "Actor.h"
#include "LightComponent.h"

// ============================================================
//  ALight
//  ALight に相当するライトアクターの共通基底。
//  ULightComponent を Root に持ち、有効化 / 明るさ / 色のユーティリティを
//  コンポーネントへ委譲する。
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
	// 有効 / 無効はコンポーネントの可視性 (SetVisibility) で切り替える。
	// bAffectsWorld はエディタ時の設定で、ランタイムの切り替えには使わない
	void SetEnabled(bool bSetEnabled);
	bool IsEnabled() const;
	void ToggleEnabled();

	void  SetBrightness(float NewBrightness);
	float GetBrightness() const;

	// 線形色
	void     SetLightColor(const XMFLOAT4& NewLightColor);
	XMFLOAT4 GetLightColor() const;

	void SetCastShadows(bool bNewValue);
	void SetAffectTranslucentLighting(bool bNewValue);
};
