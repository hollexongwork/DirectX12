#pragma once
#include <DirectXMath.h>
#include "BoxSphereBounds.h"

using namespace DirectX;

// ============================================================
//  FPrimitiveSceneProxy
//  レンダー側ミラー。
//  UPrimitiveComponent::CreateSceneProxy() が生成し、FScene
//  (FPrimitiveSceneInfo) が所有する。レンダラ (FSceneRenderer)
//  はこのプロキシだけを読み、ゲーム側オブジェクトには触れない。
//
//  データフロー (一方向):
//    - トランスフォーム / 境界 / 可視性:
//        UWorld::SendAllEndOfFrameUpdates ->
//        UPrimitiveComponent::SendRenderTransform -> SetTransform
//    - マテリアル / メッシュ / テクスチャ変更:
//        MarkRenderStateDirty -> 次フレーム頭でプロキシ再生成
//          (FScene::UpdateAllPrimitiveSceneInfos)
//
//  フラスタムカリング: m_Bounds (FBoxSphereBounds) を
//  FSceneRenderer::ComputeViewVisibility とシャドウ深度パスが読む。
//  Min/MaxDrawDistance は生成時スナップショット (距離カリング用)。
//
//  現状はシングルスレッドだが、レンダラが読むデータの所有権を
//  こちらに寄せてあるため、将来レンダースレッドを立てる場合は
//  コマンドキュー経由の SetTransform / 再生成に置き換えるだけでよい。
// ============================================================

class RenderManager;
class UPrimitiveComponent;
class FBXModel;

// ============================================================
//  FPrimitiveViewRelevance (最小形)
//  FPrimitiveViewRelevance / FMaterialRelevance に相当。
//  プロキシの GetViewRelevance() が返し、FSceneRenderer が
//  各メッシュパスへの参加可否を判定する:
//    bOpaque / bMasked   -> ベースパス (RenderBasePass, G-Buffer)
//    bNormalTranslucency -> トランスルーセンシーパス
//                           (RenderTranslucency, 後→前ソートで
//                            SceneColor へフォワード合成)
//  複数マテリアルスロットを持つプリミティブは全スロットの
//  Blend Mode を OR で集約する (両フラグが立ち得る)。
// ============================================================

struct FPrimitiveViewRelevance
{
	bool bOpaque = true;
	bool bMasked = false;
	bool bNormalTranslucency = false;

	bool HasOpaqueRelevance() const { return bOpaque || bMasked; }
	bool HasTranslucency() const { return bNormalTranslucency; }
};

class FPrimitiveSceneProxy
{
protected:
	// コンポーネントのワールド行列 (転置前)。SendRenderTransform が毎フレーム更新する。
	XMFLOAT4X4 m_LocalToWorld;

	// ワールド境界 (FPrimitiveSceneProxy が FPrimitiveSceneInfo 経由で
	// 保持する Bounds に相当)。SendRenderTransform が毎フレーム更新する。
	FBoxSphereBounds m_Bounds;

	// UPrimitiveComponent::TranslucencySortPriority のミラー
	int m_TranslucencySortPriority = 0;

	bool m_Visible = true;
	bool m_bCastShadow = true;	// シャドウ深度パスに参加するか (生成時スナップショット)
	bool m_bAffectDistanceField = true;	// Distance Field に寄与するか (生成時スナップショット)

	// 描画距離カリング (生成時スナップショット。0 = 無制限)
	float m_MinDrawDistance = 0.0f;
	float m_MaxDrawDistance = 0.0f;

public:
	// 生成時にコンポーネントからトランスフォーム / 境界 / 可視性を
	// スナップショットする
	FPrimitiveSceneProxy(const UPrimitiveComponent* Component);
	virtual ~FPrimitiveSceneProxy() = default;

	// トランスフォームとワールド境界を同時に更新する
	// (FPrimitiveSceneProxy::SetTransform 相当)。
	void SetTransform(const XMMATRIX& LocalToWorld, const FBoxSphereBounds& Bounds)
	{
		XMStoreFloat4x4(&m_LocalToWorld, LocalToWorld);
		m_Bounds = Bounds;
	}

	void SetVisibility(bool Visible) { m_Visible = Visible; }
	bool IsVisible() const { return m_Visible; }

	bool CastsShadow() const { return m_bCastShadow; }
	bool AffectsDistanceField() const { return m_bAffectDistanceField; }

	// フラスタム / 距離カリング用アクセサ
	const FBoxSphereBounds& GetBounds() const { return m_Bounds; }

	// ---- トランスルーセンシーソート優先度 (TranslucencySortPriority) ----
	// 低い値が奥、高い値が手前に描かれる。同値内は後→前ソート。
	// SendRenderTransform が毎フレームコンポーネントからプッシュする。
	void SetTranslucencySortPriority(int Priority) { m_TranslucencySortPriority = Priority; }
	int  GetTranslucencySortPriority() const { return m_TranslucencySortPriority; }
	float GetMinDrawDistance() const { return m_MinDrawDistance; }
	float GetMaxDrawDistance() const { return m_MaxDrawDistance; }

	// FShadowSceneRenderer::UpdateDistanceFieldObjects 用アクセサ (転置前)
	const XMFLOAT4X4& GetLocalToWorld() const { return m_LocalToWorld; }

	// Distance Field Shadows: 有効な SDF を持つメッシュを返す。
	// 既定は nullptr (= DF に寄与しない)。FStaticMeshSceneProxy が実装する。
	virtual const FBXModel* GetDistanceFieldMesh() const { return nullptr; }

	// FPrimitiveSceneProxy::GetViewRelevance 相当。
	// マテリアルの Blend Mode からパス参加可否を返す。
	// 既定は不透明のみ (マテリアルを持たないプリミティブ)。
	virtual FPrimitiveViewRelevance GetViewRelevance() const { return FPrimitiveViewRelevance{}; }

	// ベースパスから呼ばれる描画本体 (Opaque / Masked サブセットのみ。
	// Translucent / Additive サブセットは描かないこと)
	virtual void DrawPrimitive(RenderManager* RHI) const = 0;

	// トランスルーセンシーパス (FSceneRenderer::RenderTranslucency)
	// から呼ばれる描画。Translucent / Additive サブセットのみ描く。
	// 既定は何も描かない (不透明専用プリミティブ)。
	// トランスルーセンシーパスの描画モード
	// (FSceneRenderer::RenderTranslucency の深度プリパス方式が使用)
	enum class ETranslucencyDrawMode
	{
		Standard,		// 従来: 1 パス合成 (深度テストのみ)
		DepthPrepass,	// 深度のみ: プリミティブの最前面を深度へ焼く
						// (BLEND_Translucent のみ。Additive は何も描かない)
		ColorEqual,		// 着色: EQUAL 比較で最前面のみ合成
						// (Additive はここで従来 PSO のまま描く)
	};

	virtual void DrawTranslucency(RenderManager* RHI,
		ETranslucencyDrawMode Mode = ETranslucencyDrawMode::Standard) const {}

	// シャドウ深度パス (FShadowSceneRenderer::RenderShadowDepthMaps) から
	// 呼ばれる深度のみの描画。マテリアル / テクスチャはバインドしない。
	// 既定は何も描かない (2D オーバーレイなどは影を落とさない)。
	virtual void DrawShadowDepth(RenderManager* RHI) const {}

protected:
	// OBJECT 定数 (ワールド行列) を転置してアップロードする共通処理
	// PRIMITIVE 定数 (b1, FPrimitiveUniformShaderParameters 相当) を
	// プロキシのワールド行列からアップロードする。
	void UploadPrimitiveConstant(RenderManager* RHI) const;
};
