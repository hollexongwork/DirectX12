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

	void              SetActiveCamera(UCameraComponent* Camera) { m_ActiveCamera = Camera; }
	UCameraComponent* GetActiveCamera() const { return m_ActiveCamera; }

	void                SetPostProcessVolume(APostProcessVolume* Volume) { m_PostProcessVolume = Volume; }
	APostProcessVolume* GetPostProcessVolume() const { return m_PostProcessVolume; }
};
