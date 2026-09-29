#pragma once
#include <DirectXMath.h>
#include <vector>
#include <memory>
#include "PrimitiveSceneProxy.h"	// unique_ptr の破棄に完全型が必要
#include "LightSceneProxy.h"

using namespace DirectX;

class UPrimitiveComponent;
class UCameraComponent;
class ULightComponent;
class APostProcessVolume;
class UExponentialHeightFogComponent;

// ============================================================
//  FPrimitiveSceneInfo
//  FPrimitiveSceneInfo に相当。ゲーム側コンポーネントと
//  レンダー側プロキシの対をシーンの登録簿として保持する。
//  レンダラ (FSceneRenderer) が読むのは Proxy のみ。Component は
//  UpdateAllPrimitiveSceneInfos (ゲーム側フェーズ) だけが触る。
// ============================================================

struct FPrimitiveSceneInfo
{
	UPrimitiveComponent* Component = nullptr;
	std::unique_ptr<FPrimitiveSceneProxy> Proxy;
};

// ============================================================
//  FLightSceneInfo
//  FLightSceneInfo に相当。ゲーム側ライトコンポーネントと
//  レンダー側プロキシ (FLightSceneProxy) の対を保持する。
//  レンダラ (FSceneRenderer::SetupLightConstants) が読むのは
//  Proxy のみ。
// ============================================================

struct FLightSceneInfo
{
	ULightComponent* Component = nullptr;
	std::unique_ptr<FLightSceneProxy> Proxy;
};

// ============================================================
//  FExponentialHeightFogSceneInfo
//  FExponentialHeightFogSceneInfo (ScenePrivate.h) に相当する
//  Exponential Height Fog のレンダー側スナップショット。
//  UExponentialHeightFogComponent から FScene::AddExponentialHeightFog
//  で生成され、FFogSceneRenderer (FogRendering.h) が毎フレーム
//  FOG 定数 (b7) / Volumetric Fog のパラメータへ解決する。
//
//  単位はメートル系に換算済み (コンストラクタは FogRendering.cpp):
//    - Density / HeightFalloff : コンポーネント値 / 10 [1/m]
//    - 高さは Y。FogData[0].Height = コンポーネントのワールド Y
//    - VolumetricFogEmissive : コンポーネント値 / 100 [/m]
// ============================================================

struct FExponentialHeightFogSceneInfo
{
	// 2 層の指数フォグ (FogData[0] = 第 1 層, [1] = SecondFogData)
	struct FExponentialFogData
	{
		float Density = 0.0f;			// [1/m]
		float Height = 0.0f;			// [m] (ワールド Y)
		float HeightFalloff = 0.0f;		// [1/m]
	};

	const UExponentialHeightFogComponent* Component = nullptr;

	FExponentialFogData FogData[2];
	XMFLOAT3 FogColor = { 0.447f, 0.638f, 1.0f };	// FogInscatteringLuminance (キューブマップ使用時は InscatteringTextureTint)
	float    FogMaxOpacity = 1.0f;
	float    StartDistance = 0.0f;					// [m]
	float    EndDistance = 0.0f;					// [m] (0 = 無効)
	float    FogCutoffDistance = 0.0f;				// [m] (0 = 無効)

	// Directional Inscattering
	float    DirectionalInscatteringExponent = 4.0f;
	float    DirectionalInscatteringStartDistance = 100.0f;	// [m]
	XMFLOAT3 DirectionalInscatteringColor = { 0.25f, 0.25f, 0.125f };

	// Inscattering Color Cubemap
	bool     bInscatteringColorCubemap = false;		// true = キューブマップ (IBL prefilter) で色付け
	float    InscatteringColorCubemapAngle = 0.0f;	// [rad]
	float    FullyDirectionalInscatteringColorDistance = 1000.0f;	// [m]
	float    NonDirectionalInscatteringColorDistance = 10.0f;		// [m]

	// Volumetric Fog
	bool     bEnableVolumetricFog = false;
	float    VolumetricFogScatteringDistribution = 0.2f;	// HG の g (-0.99..0.99 にクランプ済み)
	XMFLOAT3 VolumetricFogAlbedo = { 1.0f, 1.0f, 1.0f };
	XMFLOAT3 VolumetricFogEmissive = { 0.0f, 0.0f, 0.0f };	// [/m] (換算済み)
	float    VolumetricFogExtinctionScale = 1.0f;
	float    VolumetricFogDistance = 60.0f;					// [m]
	float    VolumetricFogStartDistance = 0.0f;				// [m]
	float    VolumetricFogNearFadeInDistance = 0.0f;		// [m]
	float    VolumetricFogStaticLightingScatteringIntensity = 1.0f;
	bool     bOverrideLightColorsWithFogInscatteringColors = false;

	FExponentialHeightFogSceneInfo() = default;
	explicit FExponentialHeightFogSceneInfo(const UExponentialHeightFogComponent* InComponent);
};

// ============================================================
//  FScene
//  FScene に相当するレンダラ側のシーン表現。
//  プリミティブ (コンポーネント + プロキシ) / アクティブカメラ /
//  ライト / ポストプロセスボリュームの登録簿を持つ。
//  Phase 3: FPrimitiveSceneProxy 分離。ベースパスはプロキシ列だけを
//  読むため、ゲーム側とレンダー側のデータ所有が一方向に。 
//  (将来のレンダースレッド化の土台)
//  Phase 4: ライトリスト (FLightSceneInfo) を追加。
//  ディレクショナル / ポイント / スポット / レクトの各ライトが
//  プリミティブと同じプロキシパターンで登録される。
//  Phase 5 以降: ShadowMap 用の深度パス巡回もこの Primitives
//  リストを再利用する。
//  Exponential Height Fog: FScene::ExponentialFogs 相当の
//  FExponentialHeightFogSceneInfo 列を持つ (レンダラは先頭を使う)。
// ============================================================

class FScene
{
private:
	std::vector<FPrimitiveSceneInfo>  m_Primitives;

	// ライト登録簿
	// Directional / Point / Spot / Rect すべてここに登録され、
	// FSceneRenderer::SetupLightConstants が毎フレーム
	// VIEW 定数 (directional) + ライトバッファ (local) に解決する。
	std::vector<FLightSceneInfo>      m_Lights;

	// ---- ダーティリスト (プッシュ型更新) ----
	// 全コンポーネントを毎フレームポーリングする代わりに、変更された
	// コンポーネントだけがセッター経由 (MarkRenderStateDirty /
	// MarkRenderTransformDirty) で自分をここへ積む。
	// 二重登録はコンポーネント側のダーティフラグで防止される
	// (フラグが既に立っていれば積まない)。
	// UpdateAll*SceneInfos がリストだけを処理してクリアする。
	std::vector<UPrimitiveComponent*> m_PrimitiveRenderStateDirtyList;
	std::vector<UPrimitiveComponent*> m_PrimitiveTransformDirtyList;
	std::vector<ULightComponent*>     m_LightRenderStateDirtyList;
	std::vector<ULightComponent*>     m_LightTransformDirtyList;

	// ---- Exponential Height Fog (FScene::ExponentialFogs) ----
	// 登録順に保持し、レンダラは先頭 (ExponentialFogs[0]) のみ使う
	// プロパティ / トランスフォーム変更はダーティリスト経由で
	// UpdateAllExponentialHeightFogSceneInfos が再スナップショットする。
	std::vector<FExponentialHeightFogSceneInfo>   m_ExponentialFogs;
	std::vector<UExponentialHeightFogComponent*>  m_FogRenderStateDirtyList;

	UCameraComponent* m_ActiveCamera = nullptr;
	APostProcessVolume* m_PostProcessVolume = nullptr;

public:
	// 登録時に CreateSceneProxy() でレンダー側ミラーを生成・所有する。
	// 初回フレームのトランスフォームプッシュも予約する。
	void AddPrimitive(UPrimitiveComponent* Primitive);
	void RemovePrimitive(UPrimitiveComponent* Primitive);

	// ダーティリストにあるプリミティブだけを処理する:
	// レンダーステートダーティ -> プロキシ再生成 (その場・描画順不変)、
	// トランスフォームダーティ -> トランスフォーム / 境界 / 可視性プッシュ。
	// UWorld::SendAllEndOfFrameUpdates から毎フレーム呼ばれる。
	// (FScene::UpdateAllPrimitiveSceneInfos)
	void UpdateAllPrimitiveSceneInfos();

	const std::vector<FPrimitiveSceneInfo>& GetPrimitives() const { return m_Primitives; }

	// ---- ダーティリストへのエンキュー ----
	// コンポーネント側の MarkRenderStateDirty / MarkRenderTransformDirty
	// だけが呼ぶこと (二重登録防止フラグはコンポーネント側が管理する)。
	void AddPrimitiveRenderStateDirty(UPrimitiveComponent* Primitive) { m_PrimitiveRenderStateDirtyList.push_back(Primitive); }
	void AddPrimitiveTransformDirty(UPrimitiveComponent* Primitive) { m_PrimitiveTransformDirtyList.push_back(Primitive); }
	void AddLightRenderStateDirty(ULightComponent* Light) { m_LightRenderStateDirtyList.push_back(Light); }
	void AddLightTransformDirty(ULightComponent* Light) { m_LightTransformDirtyList.push_back(Light); }

	// ---- ライト (FScene::AddLight / RemoveLight) ----
	// 登録時に CreateLightSceneProxy() でレンダー側ミラーを生成・所有する
	void AddLight(ULightComponent* Light);
	void RemoveLight(ULightComponent* Light);

	// ダーティリストにあるライトだけを処理する (プリミティブと同じ
	// プッシュ型)。UWorld::SendAllEndOfFrameUpdates から毎フレーム呼ばれる。
	void UpdateAllLightSceneInfos();

	const std::vector<FLightSceneInfo>& GetLights() const { return m_Lights; }

	// ---- Exponential Height Fog (FScene::AddExponentialHeightFog /
	//      RemoveExponentialHeightFog / HasAnyExponentialHeightFog) ----
	// 登録時に FExponentialHeightFogSceneInfo を生成して保持する
	void AddExponentialHeightFog(UExponentialHeightFogComponent* FogComponent);
	void RemoveExponentialHeightFog(UExponentialHeightFogComponent* FogComponent);
	bool HasAnyExponentialHeightFog() const { return !m_ExponentialFogs.empty(); }

	// コンポーネント側の MarkRenderStateDirty だけが呼ぶこと
	void AddExponentialHeightFogRenderStateDirty(UExponentialHeightFogComponent* FogComponent) { m_FogRenderStateDirtyList.push_back(FogComponent); }

	// ダーティリストにあるフォグだけ SceneInfo を再スナップショットする。
	// UWorld::SendAllEndOfFrameUpdates から毎フレーム呼ばれる。
	void UpdateAllExponentialHeightFogSceneInfos();

	const std::vector<FExponentialHeightFogSceneInfo>& GetExponentialFogs() const { return m_ExponentialFogs; }

	void              SetActiveCamera(UCameraComponent* Camera) { m_ActiveCamera = Camera; }
	UCameraComponent* GetActiveCamera() const { return m_ActiveCamera; }

	void                SetPostProcessVolume(APostProcessVolume* Volume) { m_PostProcessVolume = Volume; }
	APostProcessVolume* GetPostProcessVolume() const { return m_PostProcessVolume; }
};
