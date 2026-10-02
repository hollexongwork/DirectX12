#pragma once
#include "LightComponent.h"

// ELightUnits に相当。ローカルライト (Point / Spot / Rect) の Intensity の物理単位。
// 逆二乗フォールオフのときだけ意味を持つ。
enum class ELightUnits : int
{
	Unitless = 0,	// 単位なし (1 unitless = 1/625 cd)
	Candelas,		// cd (= lm/sr)。1000 cd の光は 1 m の距離で 1000 lux
	Lumens,			// lm。光の立体角で割って cd にする (Point = 4π、Spot = 2π(1 - cosθ)、Rect = π)
	EV,				// EV100。1.2 * 2^Intensity cd
};

// EV100 -> 輝度 (EV100ToLuminance)
inline float EV100ToLuminance(float EV100)
{
	return 1.2f * powf(2.0f, EV100);
}

// ============================================================
//  ULocalLightComponent
//  ULocalLightComponent (Engine/Classes/Components/LocalLightComponent.h) に相当。
//  減衰半径を持つライト (Point / Spot / Rect) の共通基底。
// ============================================================
class ULocalLightComponent : public ULightComponent
{
protected:
	// Intensity の単位。UE のクラス既定は Unitless だが、本エンジンは物理単位 (lm) を既定にする [PORT]
	ELightUnits m_IntensityUnits = ELightUnits::Lumens;

	// ライトが届く範囲 [m] (UE 既定 1000 cm)
	float       m_AttenuationRadius = 10.0f;

	// 半径の変更をレンダー側へ送る。影を落とすライトは作り直し、落とさないライトは軽量経路
	void PushRadiusToRenderThread();

public:
	ULocalLightComponent();

	void  SetAttenuationRadius(float NewRadius);
	float GetAttenuationRadius() const { return m_AttenuationRadius; }

	void        SetIntensityUnits(ELightUnits NewIntensityUnits);
	ELightUnits GetIntensityUnits() const { return m_IntensityUnits; }

	// 単位間の換算係数 (ULocalLightComponent::GetUnitsConversionFactor)。
	// CosHalfConeAngle はスポットライトのコーン半角の cos (-1 = 全球)。
	// EV は線形の係数で表せないので 1 を返す
	static float GetUnitsConversionFactor(ELightUnits SrcUnits, ELightUnits TargetUnits, float CosHalfConeAngle = -1.0f);

	// (位置, 1)
	XMFLOAT4 GetLightPosition() const override;
	// 位置を中心とする減衰半径の球
	FSphere  GetBoundingSphere() const override;
};
