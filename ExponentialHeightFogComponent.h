#pragma once
#include <DirectXMath.h>
#include "SceneComponent.h"

using namespace DirectX;

// ============================================================
//  UExponentialHeightFogComponent
//  UExponentialHeightFogComponent (Engine/Classes/Components/
//  ExponentialHeightFogComponent.h) に相当。高さに応じて密度が
//  指数関数的に変化するフォグ (2 層) と、その上に乗る Volumetric Fog
//  (froxel ボリューム) のパラメータを持つ。
//
//  結合は ULightComponent と同じ一方向データフロー (プッシュ型):
//    OnRegister (可視時)   -> FScene::AddExponentialHeightFog
//                             (FExponentialHeightFogSceneInfo を生成)
//    プロパティ / トランスフォーム変更 -> MarkRenderStateDirty ->
//                             ダーティリスト経由で次フレームに再スナップショット
//    SetVisibility(false)  -> FScene::RemoveExponentialHeightFog
//
//  ---- 単位 ----
//    - FogDensity / FogHeightFalloff : コンポーネント値 / 10 = [1/m] (FExponentialHeightFogSceneInfo で換算)
//    - 距離系 (StartDistance / FogCutoffDistance / DirectionalInscattering
//      StartDistance / *InscatteringColorDistance / VolumetricFog*) は
//      メートル。
//    - フォグの基準高さ = コンポーネントのワールド Y (UE は Z)
//    - InscatteringColorCubemap はテクスチャ参照ではなく
//      「IBL の空キューブ (prefilter) を使うか」のフラグ
// ============================================================

// FExponentialHeightFogData (第 2 層。SecondFogData)
struct FExponentialHeightFogData
{
	float FogDensity = 0.0f;			// 第 2 層の密度 (0 = 無効)
	float FogHeightFalloff = 0.2f;		// 高さ減衰
	float FogHeightOffset = 0.0f;		// 第 1 層の高さからのオフセット [m]
};

class UExponentialHeightFogComponent : public USceneComponent
{
protected:
	// ---- Exponential Height Fog ----
	float    m_FogDensity = 0.02f;					// 密度 (既定 0.02)
	float    m_FogHeightFalloff = 0.2f;				// 高さ減衰 (既定 0.2)
	FExponentialHeightFogData m_SecondFogData;		// 第 2 層
	XMFLOAT3 m_FogInscatteringLuminance = { 0.447f, 0.638f, 1.0f };	// インスキャッタ色 (輝度)

	// Inscattering Color Cubemap 
	bool     m_bInscatteringColorCubemap = false;
	float    m_InscatteringColorCubemapAngle = 0.0f;	// [度]
	XMFLOAT3 m_InscatteringTextureTint = { 1.0f, 1.0f, 1.0f };
	float    m_FullyDirectionalInscatteringColorDistance = 1000.0f;	// [m] (100000cm)
	float    m_NonDirectionalInscatteringColorDistance = 10.0f;		// [m] (1000cm)

	// Directional Inscattering
	float    m_DirectionalInscatteringExponent = 4.0f;
	float    m_DirectionalInscatteringStartDistance = 100.0f;		// [m] (10000cm)
	XMFLOAT3 m_DirectionalInscatteringLuminance = { 0.25f, 0.25f, 0.125f };

	// 距離
	float    m_FogMaxOpacity = 1.0f;
	float    m_StartDistance = 0.0f;				// [m]
	float    m_EndDistance = 0.0f;					// [m] (0 = 無効)
	float    m_FogCutoffDistance = 0.0f;			// [m] (0 = 無効)

	// ---- Volumetric Fog ----
	bool     m_bEnableVolumetricFog = false;
	float    m_VolumetricFogScatteringDistribution = 0.2f;	// HG 位相関数の g
	XMFLOAT3 m_VolumetricFogAlbedo = { 1.0f, 1.0f, 1.0f };
	XMFLOAT3 m_VolumetricFogEmissive = { 0.0f, 0.0f, 0.0f };
	float    m_VolumetricFogExtinctionScale = 1.0f;
	float    m_VolumetricFogDistance = 60.0f;				// [m] (6000cm)
	float    m_VolumetricFogStartDistance = 0.0f;			// [m]
	float    m_VolumetricFogNearFadeInDistance = 0.0f;		// [m]
	float    m_VolumetricFogStaticLightingScatteringIntensity = 1.0f;
	bool     m_bOverrideLightColorsWithFogInscatteringColors = false;

	// ---- 可視性 (UActorComponent::bVisible。AExponentialHeightFog::bEnabled が制御) ----
	bool     m_bVisible = true;

	// レンダーステート変更フラグ。立っている = FScene のダーティリストに
	// エンキュー済み (登録中のみ)。次の
	// FScene::UpdateAllExponentialHeightFogSceneInfos で再スナップショットされる。
	bool     m_RenderStateDirty = false;

	// FScene に登録中か (可視 + 登録済み)
	bool     m_bAddedToScene = false;

	void AddToScene();
	void RemoveFromScene();

public:
	UExponentialHeightFogComponent()
	{
		bCanEverTick = false;
	}

	void OnRegister() override;		// FScene::AddExponentialHeightFog
	void OnUnregister() override;	// FScene::RemoveExponentialHeightFog

	// ---- ダーティ通知 (プッシュ型更新) ----
	// フラグを立て、登録済みなら FScene のダーティリストへ自分を積む
	// (フラグが既に立っていれば積まない = 二重登録防止)
	void MarkRenderStateDirty();
	bool IsRenderStateDirty() const { return m_RenderStateDirty; }
	void ClearRenderStateDirty() { m_RenderStateDirty = false; }

	// フォグの基準高さはワールド Y なので、トランスフォーム変更も
	// レンダーステート更新として扱う (UE の SendRenderTransform 相当)
	void MarkRenderTransformDirty() override;

	// ---- 可視性 (SetVisibility) ----
	void SetVisibility(bool bNewVisibility);
	bool IsVisible() const { return m_bVisible; }

	// ---- Exponential Height Fog プロパティ (セッターは全て再スナップショットをトリガー) ----
	void  SetFogDensity(float Value) { m_FogDensity = Value; MarkRenderStateDirty(); }
	float GetFogDensity() const { return m_FogDensity; }

	void  SetFogHeightFalloff(float Value) { m_FogHeightFalloff = Value; MarkRenderStateDirty(); }
	float GetFogHeightFalloff() const { return m_FogHeightFalloff; }

	void  SetSecondFogDensity(float Value) { m_SecondFogData.FogDensity = Value; MarkRenderStateDirty(); }
	void  SetSecondFogHeightFalloff(float Value) { m_SecondFogData.FogHeightFalloff = Value; MarkRenderStateDirty(); }
	void  SetSecondFogHeightOffset(float Value) { m_SecondFogData.FogHeightOffset = Value; MarkRenderStateDirty(); }
	const FExponentialHeightFogData& GetSecondFogData() const { return m_SecondFogData; }

	void            SetFogInscatteringLuminance(const XMFLOAT3& Value) { m_FogInscatteringLuminance = Value; MarkRenderStateDirty(); }
	const XMFLOAT3& GetFogInscatteringLuminance() const { return m_FogInscatteringLuminance; }

	void  SetInscatteringColorCubemap(bool bUse) { m_bInscatteringColorCubemap = bUse; MarkRenderStateDirty(); }
	bool  GetInscatteringColorCubemap() const { return m_bInscatteringColorCubemap; }

	void  SetInscatteringColorCubemapAngle(float AngleDegrees) { m_InscatteringColorCubemapAngle = AngleDegrees; MarkRenderStateDirty(); }
	float GetInscatteringColorCubemapAngle() const { return m_InscatteringColorCubemapAngle; }

	void            SetInscatteringTextureTint(const XMFLOAT3& Value) { m_InscatteringTextureTint = Value; MarkRenderStateDirty(); }
	const XMFLOAT3& GetInscatteringTextureTint() const { return m_InscatteringTextureTint; }

	void  SetFullyDirectionalInscatteringColorDistance(float Value) { m_FullyDirectionalInscatteringColorDistance = Value; MarkRenderStateDirty(); }
	float GetFullyDirectionalInscatteringColorDistance() const { return m_FullyDirectionalInscatteringColorDistance; }

	void  SetNonDirectionalInscatteringColorDistance(float Value) { m_NonDirectionalInscatteringColorDistance = Value; MarkRenderStateDirty(); }
	float GetNonDirectionalInscatteringColorDistance() const { return m_NonDirectionalInscatteringColorDistance; }

	void  SetDirectionalInscatteringExponent(float Value) { m_DirectionalInscatteringExponent = Value; MarkRenderStateDirty(); }
	float GetDirectionalInscatteringExponent() const { return m_DirectionalInscatteringExponent; }

	void  SetDirectionalInscatteringStartDistance(float Value) { m_DirectionalInscatteringStartDistance = Value; MarkRenderStateDirty(); }
	float GetDirectionalInscatteringStartDistance() const { return m_DirectionalInscatteringStartDistance; }

	void            SetDirectionalInscatteringLuminance(const XMFLOAT3& Value) { m_DirectionalInscatteringLuminance = Value; MarkRenderStateDirty(); }
	const XMFLOAT3& GetDirectionalInscatteringLuminance() const { return m_DirectionalInscatteringLuminance; }

	void  SetFogMaxOpacity(float Value) { m_FogMaxOpacity = Value; MarkRenderStateDirty(); }
	float GetFogMaxOpacity() const { return m_FogMaxOpacity; }

	void  SetStartDistance(float Value) { m_StartDistance = Value; MarkRenderStateDirty(); }
	float GetStartDistance() const { return m_StartDistance; }

	void  SetEndDistance(float Value) { m_EndDistance = Value; MarkRenderStateDirty(); }
	float GetEndDistance() const { return m_EndDistance; }

	void  SetFogCutoffDistance(float Value) { m_FogCutoffDistance = Value; MarkRenderStateDirty(); }
	float GetFogCutoffDistance() const { return m_FogCutoffDistance; }

	// ---- Volumetric Fog プロパティ ----
	void  SetVolumetricFog(bool bEnable) { m_bEnableVolumetricFog = bEnable; MarkRenderStateDirty(); }
	bool  GetVolumetricFog() const { return m_bEnableVolumetricFog; }

	void  SetVolumetricFogScatteringDistribution(float Value) { m_VolumetricFogScatteringDistribution = Value; MarkRenderStateDirty(); }
	float GetVolumetricFogScatteringDistribution() const { return m_VolumetricFogScatteringDistribution; }

	void            SetVolumetricFogAlbedo(const XMFLOAT3& Value) { m_VolumetricFogAlbedo = Value; MarkRenderStateDirty(); }
	const XMFLOAT3& GetVolumetricFogAlbedo() const { return m_VolumetricFogAlbedo; }

	void            SetVolumetricFogEmissive(const XMFLOAT3& Value) { m_VolumetricFogEmissive = Value; MarkRenderStateDirty(); }
	const XMFLOAT3& GetVolumetricFogEmissive() const { return m_VolumetricFogEmissive; }

	void  SetVolumetricFogExtinctionScale(float Value) { m_VolumetricFogExtinctionScale = Value; MarkRenderStateDirty(); }
	float GetVolumetricFogExtinctionScale() const { return m_VolumetricFogExtinctionScale; }

	void  SetVolumetricFogDistance(float Value) { m_VolumetricFogDistance = Value; MarkRenderStateDirty(); }
	float GetVolumetricFogDistance() const { return m_VolumetricFogDistance; }

	void  SetVolumetricFogStartDistance(float Value) { m_VolumetricFogStartDistance = Value; MarkRenderStateDirty(); }
	float GetVolumetricFogStartDistance() const { return m_VolumetricFogStartDistance; }

	void  SetVolumetricFogNearFadeInDistance(float Value) { m_VolumetricFogNearFadeInDistance = Value; MarkRenderStateDirty(); }
	float GetVolumetricFogNearFadeInDistance() const { return m_VolumetricFogNearFadeInDistance; }

	void  SetVolumetricFogStaticLightingScatteringIntensity(float Value) { m_VolumetricFogStaticLightingScatteringIntensity = Value; MarkRenderStateDirty(); }
	float GetVolumetricFogStaticLightingScatteringIntensity() const { return m_VolumetricFogStaticLightingScatteringIntensity; }

	void  SetOverrideLightColorsWithFogInscatteringColors(bool bOverride) { m_bOverrideLightColorsWithFogInscatteringColors = bOverride; MarkRenderStateDirty(); }
	bool  GetOverrideLightColorsWithFogInscatteringColors() const { return m_bOverrideLightColorsWithFogInscatteringColors; }
};
