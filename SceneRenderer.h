#pragma once
#include "RenderManager.h"
#include "SceneTextures.h"
#include "ConvexVolume.h"

class FScene;
class FShadowSceneRenderer;
class FLightSceneProxy;
struct FSceneView;

// ============================================================
//  FSceneRenderer
//  FDeferredShadingSceneRenderer に相当するフレーム
//  オーケストレータ。RHI 層 (RenderManager) の上に乗り、
//  FScene を巡回して 1 フレームを以下のパス列で描画する:
//
//    BeginFrame            : RHI フレーム準備 + G-Buffer オープン
//    RenderBasePass        : ビュー/環境定数 + ComputeViewVisibility
//                            (フラスタム/距離カリング) + 可視プリミティブ -> G-Buffer
//    RenderShadowDepths    : CSM + ローカルシャドウ深度 -> シャドウマップ
//    RenderLighting        : ライトグリッド構築 (タイルドライトカリング)
//                            + LinearDepth + デファードライティング -> SceneColor
//    RenderTranslucency    : Translucent / Additive プリミティブを
//                            後→前ソートで SceneColor へフォワード合成
//    RenderPostProcessing  : DOF -> AutoExposure -> Bloom -> LUT -> Tonemap
//    EndFrame              : ImGui 描画 + Present
//
//  ポストプロセス設定はゲーム側 (UWorld::CalcSceneView) が
//  APostProcessVolume から FSceneView::FinalPostProcessSettings へ
//  解決済み。レンダラはそれを m_FinalSettings (レンダラ専有
//  コピー) へ受け取り、パス内の一時変更 (テクセルサイズ / DofPad)
//  はこのコピーにのみ行われる — ボリューム側は不変。
//
//  ビュー情報も同様に FSceneView (値スナップショット) 経由で
//  受け取る。レンダラが UCameraComponent / APostProcessVolume を
//  直接読むことはない。
// ============================================================

class FSceneRenderer
{
private:
	RenderManager* m_RHI = nullptr;

	// スクリーンサイズのシーンテクスチャ群 (FSceneTextures)
	FSceneTextures m_SceneTextures;

	// フルスクリーンクアッド (スクリーンパス用 VB)
	std::unique_ptr<VERTEX_BUFFER> m_ScreenQuad;

	// ---- 定数バッファ (3 分割) ----
	// VIEW (b0)          : カメラ + 代表ディレクショナルライト。
	// FORWARD_LIGHT (b3) : ローカルライト有効数。
	// POST_PROCESS (b4)  : 解決済み PP 設定。パス内のテクセルサイズ
	//                      切替時はこれだけを再アップロードする。
	VIEW_CONSTANT          m_ViewConstant{};
	FORWARD_LIGHT_CONSTANT m_ForwardLightConstant{};

	// ボリュームから毎フレーム解決した実効設定 (レンダラ専有コピー)。
	PP_SETTINGS m_FinalSettings{};

	// ---- Bloom mip chain (half-res down to BLOOM_MIPS) ----
	static const int BLOOM_MIPS = 5;
	std::unique_ptr<RENDER_TARGET> m_BloomMip[BLOOM_MIPS]; // ping/down chain
	std::unique_ptr<RENDER_TARGET> m_BloomUp[BLOOM_MIPS];  // upsample accumulators
	int  m_BloomMipW[BLOOM_MIPS]{};
	int  m_BloomMipH[BLOOM_MIPS]{};

	// ---- Depth of Field (Gaussian, half-res) ----
	// m_DOFPrep : CoC(A) + premultiplied color(RGB) at half res
	// m_DOFPing / m_DOFPong : separable Gaussian ping-pong buffers
	// m_DOFBlur : final half-res blur (bound as t12 in the composite)
	std::unique_ptr<RENDER_TARGET> m_DOFPrep;
	std::unique_ptr<RENDER_TARGET> m_DOFPing;
	std::unique_ptr<RENDER_TARGET> m_DOFPong;
	std::unique_ptr<RENDER_TARGET> m_DOFBlur;
	// Temp full-res copy of SceneColor so the composite can read the
	// sharp scene while writing the composited result back to SceneColor.
	std::unique_ptr<RENDER_TARGET> m_DOFSharp;
	int m_DOFWidth = 0;
	int m_DOFHeight = 0;

	// IBL
	std::unique_ptr<TEXTURE> m_EnvironmentTexture;
	std::unique_ptr<class IBLBaker> m_IBLBaker;

	// Color grading LUT baker (compute, re-bakes only on change).
	std::unique_ptr<class ColorGradingLUTBaker> m_ColorGradingLUTBaker;

	// Auto exposure / eye adaptation (histogram).
	std::unique_ptr<class AutoExposure> m_AutoExposure;

	// ---- ローカルライト GPU バッファ (StructuredBuffer, t13) ----
	// FScene のライトプロキシから毎フレーム詰め直すアップロードヒープ。
	// 2 フレームインフライト (Present の待ち方) に合わせて
	// ダブルバッファ化し、GPU が読んでいる方への上書きを避ける。
	ComPtr<ID3D12Resource>          m_LightBuffer[2];
	struct FLightShaderParameters* m_LightBufferPointer[2] = {};	// 永続 Map 先
	unsigned int                    m_LightBufferSRVIndex[2] = {};
	unsigned int                    m_LightBufferFrame = 0;

	// ---- ライトグリッド (タイルドライトカリング, LightGridInjection.h) ----
	// SetupLightConstants が b3 のグリッドパラメータを解決し、
	// RenderLighting 先頭で Injection -> Compact の 2 パスを記録する。
	// デファードパスは結果を t19/t20 で読む。
	std::unique_ptr<class FLightGridInjection> m_LightGrid;

	// ---- シャドウ (FShadowSceneRenderer, ShadowRendering.h) ----
	// SetupLightConstants が集めた今フレームのプロキシ列。
	// m_FrameLocalLights はライトバッファ (t13) と同順 =
	// シャドウパラメータ (t16) との 1:1 対応を保証する。
	std::unique_ptr<FShadowSceneRenderer> m_ShadowRenderer;
	std::vector<const FLightSceneProxy*>  m_FrameLocalLights;
	const FLightSceneProxy* m_FrameDirectionalLight = nullptr;

	// ---- ビュー可視性 (ComputeViewVisibility / FrustumCull 相当) ----
	// FViewInfo::ViewFrustum + FSceneBitArray PrimitiveVisibilityMap。
	// RenderBasePass 先頭でカメラの ViewProjection からフラスタムを構築し、
	// FScene のプロキシ列を距離 -> 球 -> ボックスの順で判定する。
	// シャドウ深度パスはこのマップを使わず、各シャドウビューの
	// フラスタムで独立にカリングする (視界外のキャスターでも影は落ちる)。
	FConvexVolume              m_ViewFrustum;
	std::vector<unsigned char> m_PrimitiveVisibilityMap;	// 登録順 1:1 (1 = 可視)

	// r.FreezeRendering 相当: フラスタムを凍結してカリング挙動を可視化する
	FConvexVolume m_FrozenViewFrustum;
	XMFLOAT3      m_FrozenViewOrigin = { 0.0f, 0.0f, 0.0f };
	bool          m_bHasFrozenView = false;

	// ---- 初期化 ----
	void InitScreenQuad();
	void InitIBL();
	void InitPostProcess();
	void InitBloom();
	void InitDOF();
	void InitLightBuffer();

	// ---- パス内部 ----
	// フラスタム + 距離カリング (FSceneRenderer::ComputeViewVisibility)。
	// VIEW 定数解決後の RenderBasePass 先頭で毎フレーム実行し、
	// m_PrimitiveVisibilityMap と m_CullingStats を更新する。
	void ComputeViewVisibility(FScene* Scene);

	// FScene のライトリスト -> VIEW 定数 (directional) +
	// FORWARD_LIGHT 定数 + ライトバッファ (local, t13)。
	// RenderBasePass の先頭で毎フレーム実行。
	void SetupLightConstants(FScene* Scene);
	// FSceneView の解決済み設定 -> m_FinalSettings (フル解像度テクセル付き)
	void ResolvePostProcessSettings(const FSceneView& View);
	// m_FinalSettings を POST_PROCESS 定数 (b4) にアップロード
	void UploadPostProcessConstant();
	// m_FinalSettings のテクセルサイズを更新 (パス内の解像度切替用)
	void SetTexelSize(int Width, int Height);

	// フルスクリーンクアッドを 1 枚描く (DrawScreenPass)
	void DrawScreenPass();

	// Gaussian Depth of Field. Reads SceneColor + linear depth,
	// produces the half-res blur in m_DOFBlur then composites the
	// sharp+blurred result back into SceneColor.
	void RenderDOF();

	// Bloom pass (bright-pass + down/up chain) writing m_BloomUp[0].
	void RenderBloom();

public:
	FSceneRenderer(RenderManager* RHI);
	~FSceneRenderer();

	// ---- フレームパス列 (GameManager::Draw から呼ばれる) ----
	// View はゲーム側 (UWorld::CalcSceneView) がフレーム先頭で構築した
	// 値スナップショット。View.bValid = false (カメラ不在) のフレームは
	// ビュー定数を更新せず、CSM もスキップする (従来挙動と同じ)。
	void BeginFrame();
	void RenderBasePass(FScene* Scene, const FSceneView& View);
	// シャドウ深度パス (RenderShadowDepthMaps)。RenderBasePass の後、
	// RenderLighting の前に呼ぶこと (SetupLightConstants の結果を使う)。
	void RenderShadowDepths(FScene* Scene, const FSceneView& View);
	void RenderLighting();
	// トランスルーセンシーパス (RenderTranslucency 相当)。
	// RenderLighting の後 (SceneColor 確定後)、RenderPostProcessing の
	// 前に呼ぶこと。可視トランスルーセントプリミティブを
	// TranslucentSortPolicy::SortByDistance (境界原点のカメラ距離) で
	// 後→前にソートし、SceneColor へフォワードシェーディングで合成する。
	void RenderTranslucency(FScene* Scene);
	void RenderPostProcessing();
	void EndFrame();

	// ---- アクセサ ----
	FSceneTextures* GetSceneTextures() { return &m_SceneTextures; }

	// Color grading LUT baker (for ImGui to flag a re-bake on edits).
	class ColorGradingLUTBaker* GetColorGradingLUTBaker() { return m_ColorGradingLUTBaker.get(); }

	// Auto exposure system (for ImGui parameter control).
	class AutoExposure* GetAutoExposure() { return m_AutoExposure.get(); }

	// Light grid (タイルドライトカリング。ImGui のデバッグ制御用)
	class FLightGridInjection* GetLightGrid() { return m_LightGrid.get(); }

	// Shadow renderer (ImGui のシャドウカリング統計表示用)
	FShadowSceneRenderer* GetShadowRenderer() { return m_ShadowRenderer.get(); }

	// ---- フラスタムカリング制御 (ImGui デバッグ用) ----
	struct FCullingParams
	{
		bool bEnableFrustumCulling = true;	// false = 全プリミティブを描画 (距離カリングも停止)
		bool bFreezeFrustum = false;		// r.FreezeRendering: フラスタムを凍結してカリングを可視化
	};

	// ---- カリング統計 (毎フレーム ComputeViewVisibility が更新) ----
	struct FCullingStats
	{
		int NumProcessed = 0;		// 判定対象プリミティブ数 (プロキシ保有分)
		int NumVisible = 0;			// 可視 (ベースパスで描画される) 数
		int NumFrustumCulled = 0;	// フラスタムで棄却された数
		int NumDistanceCulled = 0;	// Min/MaxDrawDistance で棄却された数
	};

	FCullingParams& GetCullingParams() { return m_CullingParams; }
	const FCullingStats& GetCullingStats() const { return m_CullingStats; }

	// ---- トランスルーセンシーソートポリシー (ETranslucentSortPolicy) ----
	// プロジェクト設定 Translucent Sort Policy 相当。
	//   SortByDistance   : カメラ→境界原点の距離 (既定。全方位で自然)
	//   SortByProjectedZ : ビュー空間 Z (視線方向の奥行き。地面のような
	//                      大きな面と小物の前後関係が原点距離で逆転する
	//                      ケースに強い)
	//   SortAlongAxis    : 固定軸への射影 (2D / 見下ろし向け。
	//                      Translucent Sort Axis を併用)
	enum class ETranslucentSortPolicy : int
	{
		SortByDistance = 0,
		SortByProjectedZ = 1,
		SortAlongAxis = 2,
	};

	struct FTranslucencyParams
	{
		ETranslucentSortPolicy SortPolicy = ETranslucentSortPolicy::SortByDistance;
		XMFLOAT3 SortAxis = { 0.0f, 0.0f, 1.0f };	// SortAlongAxis 用 (TranslucentSortAxis)
	};

	FTranslucencyParams& GetTranslucencyParams() { return m_TranslucencyParams; }

private:
	FCullingParams m_CullingParams;
	FTranslucencyParams m_TranslucencyParams;
	FCullingStats  m_CullingStats;
};
