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

// ============================================================
//  FPrimitiveSceneInfo
//  FPrimitiveSceneInfo に相当。ゲーム側コンポーネントと
//  レンダー側プロキシの対をシーンの登録簿として保持する。
//  レンダラ (FSceneRenderer) が読むのは Proxy のみ。Component は
//  UpdateAllPrimitiveSceneInfos (ゲーム側フェーズ) だけが触る。
// ============================================================

struct FPrimitiveSceneInfo
{
	UPrimitiveComponent*                  Component = nullptr;
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
// ============================================================

class FScene
{
private:
	std::vector<FPrimitiveSceneInfo>  m_Primitives;

	// ライト登録簿 
	// Directional / Point / Spot / Rect すべてここに登録され、
	// FSceneRenderer::SetupLightConstants が毎フレーム
	// ENV 定数 (directional) + ライトバッファ (local) に解決する。
	std::vector<FLightSceneInfo>      m_Lights;

	UCameraComponent*                 m_ActiveCamera = nullptr;
	APostProcessVolume*               m_PostProcessVolume = nullptr;

public:
	// 登録時に CreateSceneProxy() でレンダー側ミラーを生成・所有する
	void AddPrimitive(UPrimitiveComponent* Primitive);
	void RemovePrimitive(UPrimitiveComponent* Primitive);

	// ダーティなプロキシの再生成 (その場・描画順不変) と
	// 全プリミティブのトランスフォーム / 可視性プッシュ。
	// UWorld::SendAllEndOfFrameUpdates から毎フレーム呼ばれる。
	// (FScene::UpdateAllPrimitiveSceneInfos)
	void UpdateAllPrimitiveSceneInfos();

	const std::vector<FPrimitiveSceneInfo>& GetPrimitives() const { return m_Primitives; }

	// ---- ライト (FScene::AddLight / RemoveLight) ----
	// 登録時に CreateLightSceneProxy() でレンダー側ミラーを生成・所有する
	void AddLight(ULightComponent* Light);
	void RemoveLight(ULightComponent* Light);

	// ダーティなライトプロキシの再生成と全ライトのトランスフォーム
	// プッシュ。UWorld::SendAllEndOfFrameUpdates から毎フレーム呼ばれる。
	void UpdateAllLightSceneInfos();

	const std::vector<FLightSceneInfo>& GetLights() const { return m_Lights; }

	void              SetActiveCamera(UCameraComponent* Camera) { m_ActiveCamera = Camera; }
	UCameraComponent* GetActiveCamera() const { return m_ActiveCamera; }

	void                SetPostProcessVolume(APostProcessVolume* Volume) { m_PostProcessVolume = Volume; }
	APostProcessVolume* GetPostProcessVolume() const { return m_PostProcessVolume; }
};
