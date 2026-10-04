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
//  データフロー (一方向・プッシュ型):
//    - トランスフォーム / 境界 / 可視性:
//        MarkRenderTransformDirty -> FScene のダーティリスト ->
//        UWorld::SendAllEndOfFrameUpdates ->
//        UPrimitiveComponent::SendRenderTransform -> SetTransform
//        (変更されたコンポーネントだけがプッシュされる)
//    - マテリアル / メッシュ / テクスチャ変更:
//        MarkRenderStateDirty -> ダーティリスト経由で次フレーム頭に
//        プロキシ再生成 (FScene::UpdateAllPrimitiveSceneInfos)
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
	// コンポーネントのワールド行列 (転置前)。ダーティ時に SendRenderTransform が更新する (プッシュ型)。
	XMFLOAT4X4 m_LocalToWorld;

	// ワールド境界 (FPrimitiveSceneProxy が FPrimitiveSceneInfo 経由で
	// 保持する Bounds に相当)。ダーティ時に SendRenderTransform が更新する (プッシュ型)。
	FBoxSphereBounds m_Bounds;

	// UPrimitiveComponent::TranslucencySortPriority のミラー
	int m_TranslucencySortPriority = 0;

	bool m_Visible = true;
	bool m_bCastShadow = true;	// シャドウ深度パスに参加するか (生成時スナップショット)
	bool m_bAffectDistanceField = true;	// Distance Field に寄与するか (生成時スナップショット)
	bool m_bRenderVelocity = true;	// ベロシティパスに参加するか (生成時スナップショット)

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
	bool RendersVelocity() const { return m_bRenderVelocity; }

	// フラスタム / 距離カリング用アクセサ
	const FBoxSphereBounds& GetBounds() const { return m_Bounds; }

	// ---- トランスルーセンシーソート優先度 (TranslucencySortPriority) ----
	// 低い値が奥、高い値が手前に描かれる。同値内は後→前ソート。
	// ダーティ時に SendRenderTransform がコンポーネントからプッシュする。
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

	// トランスルーセンシーパスの描画モード
	// (FSceneRenderer::RenderTranslucency の深度プリパス方式が使用)
	enum class ETranslucencyDrawMode
	{
		Standard,		// 従来: 1 パス合成 (深度テストのみ。"Translucency" 系 PSO)。
		// (現在の RenderTranslucency は渡さない。DrawTranslucency の既定引数用)
		DepthPrepass,	// 深度のみ: プリミティブの最前面を深度へ焼く
		// (BLEND_Translucent のみ。Additive は何も描かない)
		ColorEqual,		// 着色: EQUAL 比較で最前面のみ合成
		// (Additive はここで従来 PSO のまま描く)
	};

	// トランスルーセンシーパス (FSceneRenderer::RenderTranslucency)
	// から呼ばれる描画。Translucent / Additive サブセットのみ描く。
	// 既定は何も描かない (不透明専用プリミティブ)。
	virtual void DrawTranslucency(RenderManager* RHI,
		ETranslucencyDrawMode Mode = ETranslucencyDrawMode::Standard) const {
	}

	// シャドウ深度パス (FShadowSceneRenderer::RenderShadowDepthMaps) から
	// 呼ばれる深度のみの描画。マテリアル / テクスチャはバインドしない。
	// 既定は何も描かない (2D オーバーレイなどは影を落とさない)。
	virtual void DrawShadowDepth(RenderManager* RHI) const {}

	// Lumen カードキャプチャパス (FLumenSceneData::RenderCardCaptures) から
	// 呼ばれる描画。b0 にはカードビュー (ローカル空間オルソ) が積まれて
	// いるため、b1 へ単位行列を積んでローカル空間のままマテリアル付きで
	// 描くこと (Opaque / Masked サブセットのみ)。
	// 既定は何も描かない (SDF を持つ FStaticMeshSceneProxy が実装する)。
	virtual void DrawCardCapture(RenderManager* RHI) const {}

	// ベロシティパス (FSceneRenderer::RenderVelocities, VelocityRendering.cpp) から
	// 呼ばれる描画。前フレームから動いたプリミティブだけが呼ばれる。
	// b1 に LocalToWorld + PreviousLocalToWorld を積み、Opaque / Masked サブセットを
	// Velocity* PSO で描くこと (Translucent / Additive は描かない)。
	// 既定は何も描かない (2D オーバーレイなど)。
	virtual void DrawVelocity(RenderManager* RHI, const XMFLOAT4X4& PreviousLocalToWorld) const {}

	// 変換が変わらなくても毎フレーム速度を描くか (AlwaysHasVelocity)。
	// 将来のスキニング / WPO 用のフック。現状はすべて false
	virtual bool AlwaysHasVelocity() const { return false; }

	// Responsive AA マスクパス (FSceneRenderer::RenderResponsiveAAMask) から呼ばれる描画。
	// RenderTranslucency の最後 (半透明深度プリパスの深度を DSV にバインドしたまま) に、
	// 後→前ソート順で呼ばれる。b1 を積み、マテリアルが bEnableResponsiveAA の
	// Translucent / Additive サブセット (bForceAll なら全 Translucent / Additive サブセット) だけを
	// ResponsiveAA[TwoSided] PSO (GeometryVS / ResponsiveAAPS, R8_UNORM, LESS_EQUAL 書き込み無し) で描くこと
	// (ステンシルの代わりにマスクへ描く)。既定は何も描かない
	virtual void DrawResponsiveAA(RenderManager* RHI, bool bForceAll) const {}

	// DrawResponsiveAA が何か描くか (Responsive の Translucent / Additive サブセットを持つか)。
	// 1 つも無いフレームはマスクのクリアもせず、TAA は Responsive 無効 (ダミー) で走る
	virtual bool HasResponsiveAATranslucency(bool bForceAll) const { return false; }

protected:
	// PRIMITIVE 定数 (b1, FPrimitiveUniformShaderParameters 相当) へ
	// プロキシのワールド行列を転置してアップロードする共通処理。
	// PreviousLocalToWorld (転置前) を渡すとベロシティ用の前フレーム行列として書く。
	// nullptr (既定) なら前フレーム行列にも今の LocalToWorld を書く。
	void UploadPrimitiveConstant(RenderManager* RHI, const XMFLOAT4X4* PreviousLocalToWorld = nullptr) const;
};
