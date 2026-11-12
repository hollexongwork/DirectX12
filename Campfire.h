#pragma once
#include "Actor.h"
#include "StaticMeshComponent.h"
#include "HeterogeneousVolumeComponent.h"
#include "PointLightComponent.h"

// ============================================================
//  ACampfire
//  焚き火。3 つのコンポーネントを 1 つのアクターにまとめる:
//    C0 (Root) UStaticMeshComponent          : 薪 (組んだ丸太) と囲みの石。
//                                              プロシージャルメッシュ (スロット 0 = 薪, 1 = 石)
//    C1        UHeterogeneousVolumeComponent : 炎。ソース = ProceduralFire
//                                              (密度 + 熱を毎フレームベイクし、黒体放射で光る)
//    C2        UPointLightComponent          : 炎の明かり。Tick で強度をゆらす
//  アクターの位置が火床の中心 (地面)。炎とライトは Root の子なので
//  アクターを動かせば一緒に動く。
//
//  ライトのゆらぎは「今の Intensity / 前回の係数 x 今回の係数」で掛けるので、
//  Details で Intensity を編集してもゆらぎの中心がそのまま追従する。
// ============================================================

class ACampfire : public AActor
{
private:
	UStaticMeshComponent*          m_WoodComponent = nullptr;
	UHeterogeneousVolumeComponent* m_FlameComponent = nullptr;
	UPointLightComponent*          m_LightComponent = nullptr;

	// ライトのゆらぎ (強度 x (1 ± FlickerAmount))
	float m_FlickerAmount = 0.30f;
	float m_FlickerSpeed = 7.0f;	// [rad/s] (基本周波数。倍音を 2 つ重ねる)

	float m_FlickerTime = 0.0f;
	float m_LastFlicker = 1.0f;

public:
	ACampfire();

	void Tick(float DeltaTime) override;

	UStaticMeshComponent*          GetWoodComponent() const { return m_WoodComponent; }
	UHeterogeneousVolumeComponent* GetFlameComponent() const { return m_FlameComponent; }
	UPointLightComponent*          GetLightComponent() const { return m_LightComponent; }

	void  SetFlickerAmount(float Value) { m_FlickerAmount = (Value < 0.0f) ? 0.0f : ((Value > 0.95f) ? 0.95f : Value); }
	float GetFlickerAmount() const { return m_FlickerAmount; }

	void  SetFlickerSpeed(float Value) { m_FlickerSpeed = (Value < 0.0f) ? 0.0f : Value; }
	float GetFlickerSpeed() const { return m_FlickerSpeed; }

	// ---- INI 保存 / 適用の補助 (SettingsManager) ----
	// ライトの Intensity はゆらぎ込みの値で毎フレーム変わるので、保存する時はゆらぎの中心
	// (= Intensity / GetCurrentFlicker()) を書く。外から Intensity を入れ直した直後は
	// ResetFlicker() で「入れた値 = 中心」に戻す
	float GetCurrentFlicker() const { return m_LastFlicker; }
	void  ResetFlicker() { m_LastFlicker = 1.0f; }
};
