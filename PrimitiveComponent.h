#pragma once
#include "SceneComponent.h"

// ============================================================
//  UPrimitiveComponent
//  描画されるコンポーネントの基底 (UPrimitiveComponent)。
//  OnRegister / OnUnregister で FScene に自身を登録し、FScene が
//  CreateSceneProxy() でレンダー側ミラー (FPrimitiveSceneProxy) を
//  生成・所有する。描画本体はプロキシへ移設済みで、レンダラは
//  ゲーム側のこのクラスにはもう触れない。
//  ShadowMap 実装時も深度パスは同じプロキシ列を巡回する。
//
//  フラスタムカリング: SendRenderTransform が UpdateBounds ->
//  SetTransform でワールド境界をプロキシへプッシュし、
//  FSceneRenderer::ComputeViewVisibility が判定する。
//  MinDrawDistance / CachedMaxDrawDistance による距離カリングも
//  同じ場所で行われる (0 = 無制限)。
// ============================================================

class RenderManager;
class FPrimitiveSceneProxy;

class UPrimitiveComponent : public USceneComponent
{
protected:
	bool m_Visible = true;

	// シャドウマップに影を落とすか (CastShadow)。
	// 空ドームのようなシーン全体を覆うプリミティブは false にすること。
	bool m_bCastShadow = true;

	// Distance Field (SDF レイマーチ) に寄与するか
	// (bAffectDistanceFieldLighting)。空ドームは false にすること。
	bool m_bAffectDistanceFieldLighting = true;

	// ---- 描画距離カリング (MinDrawDistance / LDMaxDrawDistance) ----
	// カメラから境界中心までの距離で判定する。0 = 無制限。
	// ComputeViewVisibility がフラスタム判定の前に距離で棄却する。
	float m_MinDrawDistance = 0.0f;
	float m_CachedMaxDrawDistance = 0.0f;

	// ---- TranslucencySortPriority (UPrimitiveComponent 同名) ----
	// 低い優先度は高い優先度の後ろに描かれる。同値内はバウンズ原点
	// 基準の後→前ソート。不透明では無視される。既定 0。
	int m_TranslucencySortPriority = 0;

	// レンダー側ミラー (所有は FScene::FPrimitiveSceneInfo。ここはキャッシュ)
	FPrimitiveSceneProxy* m_SceneProxy = nullptr;

	// レンダーステート (マテリアル / メッシュ / テクスチャ) 変更フラグ。
	// 立っている = FScene のレンダーステートダーティリストに
	// エンキュー済み (登録中のみ)。次の
	// FScene::UpdateAllPrimitiveSceneInfos でプロキシがその場で
	// 再生成される (描画順は変わらない)。
	bool m_RenderStateDirty = false;

public:
	void OnRegister() override;
	void OnUnregister() override;

	// レンダー側ミラーの生成 (UPrimitiveComponent::CreateSceneProxy)。
	// nullptr を返すと描画されない。
	virtual FPrimitiveSceneProxy* CreateSceneProxy() { return nullptr; }

	FPrimitiveSceneProxy* GetSceneProxy() const { return m_SceneProxy; }
	void SetSceneProxy(FPrimitiveSceneProxy* Proxy) { m_SceneProxy = Proxy; }

	// ---- レンダーステート更新 (MarkRenderStateDirty) ----
	// GetMaterial() 経由で直接マテリアルを書き換えた場合は呼ぶこと。
	// (Set*Texture / SetStaticMesh 等のセッターは内部で呼ぶ)
	// フラグを立て、登録済みなら FScene のダーティリストへ自分を積む
	// (プッシュ型更新。フラグが既に立っていれば積まない = 二重登録防止)
	void MarkRenderStateDirty();
	bool IsRenderStateDirty() const { return m_RenderStateDirty; }
	void ClearRenderStateDirty() { m_RenderStateDirty = false; }

	// ---- レンダートランスフォーム更新 (プッシュ型) ----
	// フラグを立てて FScene のトランスフォームダーティリストへ自分を
	// 積んだ後、基底実装でアタッチ子へ再帰伝搬する。
	void MarkRenderTransformDirty() override;

	// トランスフォーム + 境界 + 可視性をプロキシへプッシュ
	// (SendRenderTransform。境界は UpdateBounds で再計算してから送る)
	void SendRenderTransform();

	// 可視性 / ソート優先度は SendRenderTransform でプロキシへ届くため、
	// 変更時はトランスフォームダーティで再プッシュを予約する
	void SetVisibility(bool Visible) { m_Visible = Visible; MarkRenderTransformDirty(); }
	bool IsVisible() const { return m_Visible; }

	// シャドウキャスト (変更はプロキシ再生成で反映される)
	void SetCastShadow(bool bCastShadow) { m_bCastShadow = bCastShadow; MarkRenderStateDirty(); }
	bool GetCastShadow() const { return m_bCastShadow; }

	// Distance Field への寄与 (変更はプロキシ再生成で反映される)
	void SetAffectDistanceFieldLighting(bool bAffect) { m_bAffectDistanceFieldLighting = bAffect; MarkRenderStateDirty(); }
	bool GetAffectDistanceFieldLighting() const { return m_bAffectDistanceFieldLighting; }

	// ---- 描画距離カリング (変更はプロキシ再生成で反映される) ----
	void  SetMinDrawDistance(float Distance) { m_MinDrawDistance = Distance; MarkRenderStateDirty(); }
	float GetMinDrawDistance() const { return m_MinDrawDistance; }

	void  SetCachedMaxDrawDistance(float Distance) { m_CachedMaxDrawDistance = Distance; MarkRenderStateDirty(); }
	float GetCachedMaxDrawDistance() const { return m_CachedMaxDrawDistance; }

	// ---- トランスルーセンシーソート優先度 ----
	// (SetTranslucentSortPriority 相当。SendRenderTransform 経由で
	//  プロキシへ反映されるため、変更時はトランスフォームダーティで
	//  再プッシュを予約する)
	void SetTranslucentSortPriority(int NewPriority) { m_TranslucencySortPriority = NewPriority; MarkRenderTransformDirty(); }
	int  GetTranslucentSortPriority() const { return m_TranslucencySortPriority; }
};
