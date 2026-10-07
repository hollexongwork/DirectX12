#pragma once
#include <string>
#include <vector>
#include "RenderManager.h"
#include "PostProcessSettings.h"
#include "SceneTextures.h"
#include "ConvexVolume.h"
#include "AntiAliasingSettings.h"
#include "ScreenPercentage.h"
#include "SceneViewState.h"
#include "LightRendering.h"

class FScene;
class FShadowSceneRenderer;
class FLightSceneProxy;
class FLumenSceneData;
class FFogSceneRenderer;
class FScreenshotCapture;
class FDefaultTemporalUpscaler;
class FPrimitiveSceneProxy;
struct FLumenFrameInputs;
struct FSceneView;

// ============================================================
//  FTemporalAAStats
//  アンチエイリアシング / スクリーンパーセンテージの毎フレーム統計
//  (ImGui "Anti-Aliasing" ウィンドウとテストドライバのログが読む)。
//  各フィールドは書き手のパスが今フレームの値で上書きする。
// ============================================================
struct FTemporalAAStats
{
	int              NumVelocityDraws = 0;           // RenderVelocities の描画数
	int              NumResizes = 0;                 // ResizeRenderTargets の累計実行数
	bool             bTAARanThisFrame = false;       // TAA パスを実行した
	bool             bHistoryValidThisFrame = false; // TAA が前フレーム履歴を読んだ
	DirectX::XMUINT2 HistoryExtent{};                // 今フレーム書いた TAA 履歴の実寸
	DXGI_FORMAT      HistoryFormat = DXGI_FORMAT_UNKNOWN;
	DirectX::XMUINT2 PostExtent{};                   // 後段 (AutoExposure / Bloom / Tonemap) が実際に処理した入力の実寸
	bool             bUpscaleMerged = false;         // トーンマップとアップスケールを統合した
	const char*      UpscalePipeline = nullptr;      // 一次空間アップスケールパスの PSO 名 (走らなかった / 統合時は nullptr)
	bool             bFallbackMissingPSO = false;    // TAA の PSO 欠落で AA 無しへフォールバックした
	bool             bLumenHistoryValid = false;     // RenderLumenScene が Lumen へ渡した bHistoryValid
	bool             bFogHistoryValid = false;       // RenderLighting が Volumetric Fog へ渡した bHistoryValid
	int              LastSelfTestFailures = -1;      // 直近の自己テストの失敗数 (-1 = 未実行)
};

// ============================================================
//  FSceneRenderer
//  FDeferredShadingSceneRenderer に相当するフレーム
//  オーケストレータ。RHI 層 (RenderManager) の上に乗り、
//  FScene を巡回して 1 フレームを以下のパス列で描画する:
//
//    BeginFrame            : (先頭) 有効フラグのリセット / スクリーンショット書き出し /
//                            自己テスト要求の実行 / ビューファミリ決定 (PrepareViewRectsForRendering:
//                            レンダー / ポスト解像度が変われば ResizeRenderTargets で再確保)
//                            -> RHI フレーム準備 + G-Buffer オープン
//    RenderBasePass        : ビュー状態 (PrepareViewStateForVisibility: カメラカット /
//                            前フレームのスナップショット / b0 行列) + ビュー/環境定数
//                            + ComputeViewVisibility (ジッタ無し行列でフラスタム/距離カリング)
//                            + 可視プリミティブ -> G-Buffer
//    RenderVelocities      : 前フレームから動いたプリミティブのベロシティ -> Velocity
//                            (TAA 有効時 / ベロシティを読むデバッグ表示時のみ。VelocityRendering.cpp)
//    RenderShadowDepths    : CSM + ローカルシャドウ深度 -> シャドウマップ
//    RenderLumenScene      : Lumen カードキャプチャ + Surface Cache ライティング
//    RenderLighting        : ライトグリッド構築 (タイルドライトカリング)
//                            + Volumetric Fog (froxel 積分) + LinearDepth
//                            + Lumen スクリーン GI
//                            + デファードライティング -> SceneColor
//                            + Exponential Height Fog パス (SceneColor へ合成)
//    RenderTranslucency    : Translucent / Additive プリミティブを
//                            後→前ソートで SceneColor へフォワード合成
//                            (フォグはサーフェス位置で直接評価)
//                            + Responsive AA マスク (TAA 有効時, bEnableResponsiveAA の半透明のみ)
//    RenderPostProcessing  : SceneColor 履歴 (Lumen) -> DOF (R) -> Temporal AA (R -> H, TemporalAA.h)
//                            -> AutoExposure -> Bloom -> LUT
//                            -> Tonemap (ポスト解像度 P) [-> 一次空間アップスケール P -> O、
//                            またはトーンマップ統合 (バイリニア)] [-> 入出力分割表示 / Temporal AA デバッグ表示]
//                            (+ 要求時はスクリーンショットのコピー。書き出しは次の BeginFrame 先頭)
//                            -> CommitViewState (前フレーム情報の確定。フレームの最後)
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
	// VIEW (b0)          : カメラ + 代表ディレクショナルライト + Temporal AA
	//                      (ジッタ / 前フレーム VP / ClipToPrevClip / ミップバイアス, 448 B)。
	// FORWARD_LIGHT (b3) : ローカルライト有効数。
	// POST_PROCESS (b4)  : 解決済み PP 設定。パス内のテクセルサイズ
	//                      切替時はこれだけを再アップロードする。
	VIEW_CONSTANT          m_ViewConstant{};
	FORWARD_LIGHT_CONSTANT m_ForwardLightConstant{};

	// ボリュームから毎フレーム解決した実効設定 (レンダラ専有コピー)。
	PP_SETTINGS m_FinalSettings{};

	// ---- Bloom mip chain (出力解像度 O の半分から BLOOM_MIPS 段) ----
	// O/2 固定 (ハローの画面上の大きさをスクリーンパーセンテージ不変に保つ)。再確保しない
	static const int BLOOM_MIPS = 5;
	std::unique_ptr<RENDER_TARGET> m_BloomMip[BLOOM_MIPS]; // ping/down chain
	std::unique_ptr<RENDER_TARGET> m_BloomUp[BLOOM_MIPS];  // upsample accumulators
	int  m_BloomMipW[BLOOM_MIPS]{};
	int  m_BloomMipH[BLOOM_MIPS]{};

	// ---- トーンマップ出力 (一次空間アップスケールの入力。RGBA8, 実際のポスト入力サイズ) ----
	// ポスト解像度 P != 出力 O かつ統合しないフレームだけ使う。EnsureTonemapOutput がサイズ
	// 不一致時に作り直し (遅延解放)、P の変更時は ResizeRenderTargets が破棄する。常駐 PSR
	std::unique_ptr<RENDER_TARGET> m_TonemapOutput;

	// ---- Depth of Field (Gaussian, half-res。レンダー解像度 R 基準) ----
	// R が変わると ResizeRenderTargets が InitDOF(R) で作り直す
	// m_DOFPrep : CoC(A) + premultiplied color(RGB) at half res
	// m_DOFPing : separable Gaussian の水平ブラー出力 (垂直ブラーは m_DOFBlur へ書く)
	// m_DOFBlur : final half-res blur (bound as t12 in the composite)
	std::unique_ptr<RENDER_TARGET> m_DOFPrep;
	std::unique_ptr<RENDER_TARGET> m_DOFPing;
	std::unique_ptr<RENDER_TARGET> m_DOFBlur;
	// Temp full-res copy of SceneColor so the composite can read the
	// sharp scene while writing the composited result back to SceneColor.
	std::unique_ptr<RENDER_TARGET> m_DOFSharp;
	int m_DOFWidth = 0;
	int m_DOFHeight = 0;

	// IBL
	std::unique_ptr<class IBLBaker> m_IBLBaker;

	// Color grading LUT baker (compute, re-bakes only on change).
	std::unique_ptr<class ColorGradingLUTBaker> m_ColorGradingLUTBaker;

	// Auto exposure / eye adaptation (histogram).
	std::unique_ptr<class AutoExposure> m_AutoExposure;

	// ---- ライト GPU バッファ (StructuredBuffer<FLocalLightData>, t13 ForwardLightBuffer) ----
	// ComputeLightGrid が視界内のライトを毎フレーム詰め直すアップロードヒープ
	// ([0, NumLocalLights) がローカルライト、続けてディレクショナルライト)。
	// 2 フレームインフライト (Present の待ち方) に合わせて
	// ダブルバッファ化し、GPU が読んでいる方への上書きを避ける。
	ComPtr<ID3D12Resource>          m_LightBuffer[2];
	struct FForwardLocalLightData* m_LightBufferPointer[2] = {};	// 永続 Map 先
	unsigned int                    m_LightBufferSRVIndex[2] = {};
	unsigned int                    m_LightBufferFrame = 0;

	// ---- Lumen 用ライトバッファ ----
	// Surface Cache の直接光は画面外のライトも要るので、ビューのライトバッファとは別に
	// 「描画距離内 かつ bAffectGlobalIllumination」のライト (ディレクショナル含む) を
	// IndirectLightingScale を掛けて積む。m_LightBufferFrame と同じ面を使う
	ComPtr<ID3D12Resource>          m_LumenLightBuffer[2];
	struct FForwardLocalLightData* m_LumenLightBufferPointer[2] = {};
	unsigned int                    m_LumenLightBufferSRVIndex[2] = {};
	unsigned int                    m_NumLumenLights = 0;

	// ---- システムテクスチャ (LTC テーブル t37/t38。SystemTextures.h) ----
	std::unique_ptr<class FSystemTextures> m_SystemTextures;

	// ---- ソート済みライト (GatherAndSortLights の結果。LightRendering.h) ----
	FSortedLightSetSceneInfo m_SortedLightSet;

	// ---- ライトグリッド (タイルドライトカリング, LightGridInjection.h) ----
	// ComputeLightGrid が b3 のグリッドパラメータを解決し、
	// RenderLighting 先頭で Injection -> Compact の 2 パスを記録する。
	// デファードパスは結果を t19/t20 で読む。
	std::unique_ptr<class FLightGridInjection> m_LightGrid;

	// ---- シャドウ (FShadowSceneRenderer, ShadowRendering.h) ----
	// ComputeLightGrid が集めた今フレームのプロキシ列。
	// m_FrameLocalLights はライトバッファ (t13) と同順 =
	// シャドウパラメータ (t16) との 1:1 対応を保証する。
	// CSM を持つディレクショナルライトは m_ViewInfo.SelectedForwardDirectionalLightProxy
	std::unique_ptr<FShadowSceneRenderer> m_ShadowRenderer;
	std::vector<const FLightSceneProxy*>  m_FrameLocalLights;

	// ---- Lumen Surface Cache (FLumenSceneData, LumenScene.h) ----
	// RenderLumenScene がカードキャプチャ + Surface Cache ライティングを
	// 記録し、RenderLighting のデファードパスが b6 + t24-t27 で
	// スクリーン GI (エミッシブ光源化を含む) を読む。
	std::unique_ptr<FLumenSceneData> m_LumenScene;

	// ---- Exponential Height Fog / Volumetric Fog (FFogSceneRenderer, FogRendering.h) ----
	// RenderBasePass の InitFogConstants が FScene の ExponentialFogs[0] と
	// ビュー / 太陽光から FOG 定数 (b7) を解決し、RenderLighting が
	// Volumetric Fog のコンピュート (ライトグリッド後) とフォグパス
	// (デファード直後) を記録する。トランスルーセンシーは b7 + t33/t34 を
	// バインドしてサーフェス位置で直接評価する。
	std::unique_ptr<FFogSceneRenderer> m_FogRenderer;

	// ---- スクリーンショット (FScreenshotCapture, ScreenshotCapture.h) ----
	// RequestScreenshot で要求 -> RenderPostProcessing 末尾で UI 描画前の
	// バックバッファを READBACK へコピー -> 次フレームの BeginFrame 先頭
	// (または FlushScreenshots) で BMP を書き出す。
	// テストドライバ (-taatest) と F9 キーが使う。
	std::unique_ptr<FScreenshotCapture> m_Screenshot;

	// F9 (パス指定なし) 用の自動パス: Saved/Screenshots/<YYYYMMDD_HHMMSS>_<frame>_<SP>_<method>_<pass>_Q<q>.bmp
	std::string MakeAutoScreenshotPath() const;

	// ---- アンチエイリアシング / スクリーンパーセンテージ / ビュー状態 (FViewInfo / FSceneViewState) ----
	// m_AAParams      : 永続化設定 (SettingsManager の [AntiAliasing])。ImGui / テストドライバが書く
	// m_TAADebug      : 非永続のデバッグ設定 (ワンショット要求を含む)
	// m_ViewFamily    : BeginFrame 先頭 (PrepareViewRectsForRendering) で決まる今フレームの解像度 / AA 構成
	// m_ViewInfo      : RenderBasePass 先頭 (PrepareViewStateForVisibility) で毎フレーム再構築するビュー。
	//                   PrevViewInfo は前フレームのスナップショット (フレーム中不変) で、
	//                   Lumen / Volumetric Fog の前フレーム行列はここだけから読む
	// m_ViewState     : フレームを跨いで永続する状態。CommitViewState (RenderPostProcessing の最後) で確定
	// 前フレーム行列は PrevViewInfo からのみ読む (同フレーム内で新旧を取り違えないため)
	FAntiAliasingParams      m_AAParams;
	FTemporalAADebugSettings m_TAADebug;
	FViewFamilyInfo          m_ViewFamily;
	FViewInfo                m_ViewInfo;
	FSceneViewState          m_ViewState;
	FTemporalAAStats         m_TAAStats;

	// フレーム毎の有効フラグ。BeginFrame の最初の文でどちらも false に戻し、
	// 今フレームにクリア + 書き込みを行ったパスだけが true にする
	// (書き手は RenderVelocities / RenderResponsiveAAMask)。
	// 再確保直後の未クリアのテクスチャが有効として読まれることはない
	bool m_bVelocityValid = false;
	bool m_bResponsiveMaskValid = false;

	// 1 回だけ出すログの済みフラグ (FDefaultTemporalUpscaler::m_LoggedMissingPSOMask と同じ形)
	mutable uint32_t m_UpscaleFallbackLoggedMask = 0;	// SelectPrimaryUpscalePipeline (PostProcessUpscale.cpp) のフォールバック
	bool m_bLoggedMissingVisualizePSO = false;			// AddVisualizeTemporalAAPass の PSO 欠落

	// ---- Temporal AA / TAAU (FDefaultTemporalUpscaler, TemporalAA.h) ----
	// RenderPostProcessing の DOF の後で AddPasses を呼ぶ。PSO (.cso) が揃っていなければ
	// PrepareViewRectsForRendering が AA 無しの構成へフォールバックする (IsReady)。
	// そのダミー (1x1 RGBA16F (0,0,0,1), RD 常駐) は可視化パスの t35 / t36 の代替にも使う
	std::unique_ptr<FDefaultTemporalUpscaler> m_TemporalUpscaler;

	// 自己テスト (RunTemporalAASelfTests) の出力行。テストドライバがログへ移す
	std::vector<std::string> m_SelfTestLog;

	// ---- 確保済みの解像度 (§5.1: 確保済みと今フレームの値を比べる遅延再確保) ----
	// m_AllocatedRenderExtent : レンダー解像度 R のターゲット群 (シーンテクスチャ / 深度 / DOF /
	//                           Lumen スクリーンテクスチャ / Volumetric Fog ボリューム) の寸法
	// m_AllocatedPostExtent   : ポスト解像度 P (P の変更で一次アップスケール用のトーンマップ出力を破棄する)
	// コンストラクタはどちらもバックバッファ解像度 (R = P = O) で確保する
	XMUINT2 m_AllocatedRenderExtent{ 0u, 0u };
	XMUINT2 m_AllocatedPostExtent{ 0u, 0u };

	// BeginFrame 先頭: FViewFamilyInfo (解像度 / AA 構成) を決め (TAA の PSO が欠落していれば
	// AA 無しの構成へフォールバック)、確保済みの R / P と異なれば (または bRequestReallocate)
	// ResizeRenderTargets で作り直し、ライトグリッドの今フレームの次元と RHI の既定ビューポート (= R) を設定する
	void PrepareViewRectsForRendering();
	// レンダー / ポスト解像度のターゲットを作り直す (§5.2。順序が規範):
	//   (0) FlushAndResetCommandList (記録済み未実行のコマンドを実行して GPU をアイドルに)
	//   (1) 旧リソースを遅延削除キューへ  (2) WaitGPU (実解放 = VRAM ピーク抑制)  (3) 新サイズで生成
	// Lumen / Volumetric Fog の履歴は PrevFrameViewInfo.ViewRectSize = 0 で 1 フレーム無効化する。
	// m_TAADebug.bRequestReallocate (ワンショット) は同一サイズでも再確保する (同一サイズ再確保 / リーク検査用)
	void ResizeRenderTargets(const FViewFamilyInfo& F);
	// RenderBasePass 先頭: FViewInfo を再構築し (カメラカット / TAA ジッタ / 大移動 /
	// 前フレームのスナップショット / ClipToPrevClip / ミップバイアス)、VIEW 定数 (b0) の
	// 光源以外の全フィールドを書く
	void PrepareViewStateForVisibility(const FSceneView& View);
	// RenderPostProcessing の最後: 今フレームのビュー情報 (+ TAA 履歴) を
	// m_ViewState.PrevFrameViewInfo へ確定し、StateFrameIndex を進める
	void CommitViewState(const FTemporalAAHistory& OutputHistory);
	// 既定ビューポート (= レンダー解像度 R。PrepareViewRectsForRendering が毎フレーム設定) を
	// 現在のコマンドリストへ即時適用する (RenderTranslucency 先頭など)
	void ApplyRenderViewport();
	// Lumen / Volumetric Fog のスクリーン履歴 (レンダー解像度) を今フレーム使えるか (§4.9)
	// = 前フレーム情報が有効 && 大移動リセット無し && 前フレームの ViewRectSize == R
	bool IsScreenHistoryValid() const;
	// TAA より前の段階を置き換えるデバッグ表示 (Lumen / LightGrid の DebugMode) が有効か。
	// 有効な間は TAA の履歴を使わない (CB bCameraCut = 1 のみ。ビュー状態はカット扱いしない)
	bool IsPreTAADebugViewActive() const;

	// SceneColor -> PrevSceneColor / LinearDepth -> PrevLinearDepth コピー (テクスチャのみ)。
	// 前フレーム行列の確定は CommitViewState が行う
	void CopySceneColorHistory();

	// Lumen へ渡すフレーム入力 (ビュー / ライト / シーンテクスチャ SRV)
	// を今フレームの解決済み状態から構築する。前フレーム行列は
	// m_ViewInfo.PrevViewInfo (ジッタ込み) から取る
	FLumenFrameInputs MakeLumenFrameInputs() const;

	// ---- ビュー可視性 (ComputeViewVisibility / FrustumCull 相当) ----
	// FViewInfo::ViewFrustum + FSceneBitArray PrimitiveVisibilityMap。
	// RenderBasePass 先頭でカメラの ViewProjection からフラスタムを構築し、
	// FScene のプロキシ列を距離 -> 球 -> ボックスの順で判定する。
	// シャドウ深度パスはこのマップを使わず、各シャドウビューの
	// フラスタムで独立にカリングする (視界外のキャスターでも影は落ちる)。
	FConvexVolume              m_ViewFrustum;
	std::vector<unsigned char> m_PrimitiveVisibilityMap;	// 登録順 1:1 (1 = 可視)

	// フラスタムを凍結してカリング挙動を可視化する
	FConvexVolume m_FrozenViewFrustum;
	XMFLOAT3      m_FrozenViewOrigin = { 0.0f, 0.0f, 0.0f };
	bool          m_bHasFrozenView = false;

	// ---- 初期化 ----
	void InitScreenQuad();
	void InitIBL();
	void InitPostProcess();		// ColorGradingLUT / AutoExposure (Bloom / DOF は解像度付きで別に作る)
	// Bloom チェーン: mip0 = (max(1, OutputWidth/2), max(1, OutputHeight/2))、以降半分ずつ (切り捨て, >= 1)
	void InitBloom(unsigned int OutputWidth, unsigned int OutputHeight);
	// DOF: ハーフ解像度 ((W+1)/2, (H+1)/2) の Prep/Ping/Blur + フル解像度 W x H の Sharp
	void InitDOF(unsigned int Width, unsigned int Height);
	void InitLightBuffer();

	// ---- パス内部 ----
	// フラスタム + 距離カリング (FSceneRenderer::ComputeViewVisibility)。
	// VIEW 定数解決後の RenderBasePass 先頭で毎フレーム実行し、
	// m_PrimitiveVisibilityMap と m_CullingStats を更新する。
	void ComputeViewVisibility(FScene* Scene);

	// ---- ライト (LightRendering.cpp / LightGridInjection.cpp) ----
	// ライトの可視判定 (フラスタム / 描画距離) -> m_ViewInfo.VisibleLightInfos。
	// ComputeViewVisibility (フラスタム構築) の後に呼ぶ
	void ComputeLightVisibility(FScene* Scene);
	// 視界内のライトを集めてソートキーの昇順に並べる
	void GatherAndSortLights(FScene* Scene, FSortedLightSetSceneInfo& OutSortedLights);
	// ソート済みライト -> ライトバッファ (t13) + FORWARD_LIGHT 定数 (b3) + VIEW 定数の代表ライト
	// + フォワードディレクショナルライトの選択 + Lumen 用ライトバッファ。
	// ライトグリッド本体の構築 (コンピュート) は RenderLighting 先頭の FLightGridInjection::Dispatch
	void ComputeLightGrid(FScene* Scene, const FSortedLightSetSceneInfo& SortedLightSet);
	// GatherAndSortLights + ComputeLightGrid (FSceneRenderer::GatherLightsAndComputeLightGrid)。
	// RenderBasePass で毎フレーム実行
	void GatherLightsAndComputeLightGrid(FScene* Scene);
	// FSceneView の解決済み設定 -> m_FinalSettings (フル解像度テクセル付き)
	void ResolvePostProcessSettings(const FSceneView& View);
	// m_FinalSettings を POST_PROCESS 定数 (b4) にアップロード
	void UploadPostProcessConstant();
	// m_FinalSettings のテクセルサイズを更新 (パス内の解像度切替用)
	void SetTexelSize(int Width, int Height);

	// フルスクリーンクアッドを 1 枚描く (DrawScreenPass)
	void DrawScreenPass();

	// デファード / トランスルーセンシー共通のフォワードライティング入力
	// (IBL t6-t8 / ローカルライト t13 / ライトグリッド t19-t20 / シャドウ)
	void BindForwardLightingResources();
	// LUMEN 定数 (b6) の解決 + アップロード (m_LumenScene 非 null 前提)
	void UploadLumenConstant();

	// Gaussian Depth of Field. Reads SceneColor + linear depth,
	// produces the half-res blur in m_DOFBlur then composites the
	// sharp+blurred result back into SceneColor.
	// レンダー解像度 R で走る。ボケ半径 (ハーフ解像度テクセル単位) は MaxBlurSize x R.x/O.x で
	// 出力画素換算を一定に保つ (スクリーンパーセンテージ不変)
	void RenderDOF();

	// Bloom pass (bright-pass + down/up chain) writing m_BloomUp[0].
	// Input (PSR) を UV で読む (チェーンは O/2 固定。しきい値パスの 4 タップボックスが入力を前置フィルタする)。
	// 最後にビューポート / テクセルサイズを RestoreExtent (実際のポスト入力サイズ) へ戻す
	void RenderBloom(RENDER_TARGET* Input, XMUINT2 RestoreExtent);

	// ---- トーンマップ / 一次空間アップスケール (§4.7, §6.7) ----
	// 既存のトーンマップ描画 (PSO PostProcessTonemap, t0 = Input, t9 Bloom, t10 LUT, t11 露出)。
	// レンダーターゲット / ビューポートは呼び出し側が設定する
	void DrawTonemap(RENDER_TARGET* Input);
	// m_TonemapOutput を Extent (RGBA8) で用意する (null かサイズ違いの時だけ作り直す。状態 PSR)
	void EnsureTonemapOutput(XMUINT2 Extent);
	// 一次空間アップスケールの PSO 名。欠落時は Bilinear、それも無ければ nullptr (統合経路へ)。
	// フォールバックは 1 回だけログ (PostProcessUpscale.cpp)
	const char* SelectPrimaryUpscalePipeline() const;
	// AddUpscalePass: In (PSR, InExtent) -> 現在の RTV (バックバッファ) へ出力ビューポートで描く
	// (PostProcessUpscale.cpp)
	void AddPrimaryUpscalePass(RENDER_TARGET* In, XMUINT2 InExtent, XMUINT2 OutputExtent, const char* PipelineName);
	// バックバッファの状態遷移 (1 行のラッパ)
	void TransitionBackBuffer(ID3D12GraphicsCommandList* CommandList, D3D12_RESOURCE_STATES Before, D3D12_RESOURCE_STATES After);

	// ---- Temporal AA デバッグ表示 (§6.8, VisualizeTemporalAAPS) ----
	// 現在バインド中のバックバッファ RTV へ出力ビューポート O で上書きする (RenderPostProcessing の
	// トーンマップ / アップスケール / 入出力分割の後、スクリーンショットの前)。PSO "VisualizeTemporalAA" は
	// オプション (欠落時はログ 1 回でスキップ)。
	// t35 = m_bVelocityValid ? Velocity : ダミー。t36 = TAA DebugOutput (モード 5..13。TAA が今フレーム
	// 走らなかったら描かない) / PostInput = 後段の入力 (TAA 出力, モード 4 TemporalUpscalerIO) / ダミー
	void AddVisualizeTemporalAAPass(RENDER_TARGET* PostInput, bool bTAARan);

	// ---- Responsive AA マスク (§4.6。RenderTranslucency の最後、最終バリアの前) ----
	// レンダー解像度の R8_UNORM マスク (FSceneTextures::ResponsiveAAMask) へ bEnableResponsiveAA の
	// Translucent / Additive サブセット (m_TAADebug.bForceResponsiveAA なら全半透明) を 1 で描く。
	// 半透明深度プリパスの深度を DSV にバインドしたまま LESS_EQUAL (書き込み無し) で描くので、
	// 最前面の Translucent 層 + その手前の Additive だけが残る。描いたフレームだけ
	// m_bResponsiveMaskValid = true (TAA は無効なら t5 にダミーを束縛しフラグを立てない)。
	// SortedTranslucent は RenderTranslucency の後→前ソート順の可視半透明プロキシ
	void RenderResponsiveAAMask(const std::vector<const FPrimitiveSceneProxy*>& SortedTranslucent);

public:
	FSceneRenderer(RenderManager* RHI);
	~FSceneRenderer();

	// ---- フレームパス列 (GameManager::Draw から呼ばれる) ----
	// View はゲーム側 (UWorld::CalcSceneView) がフレーム先頭で構築した
	// 値スナップショット。View.bValid = false (カメラ不在) のフレームは
	// ビュー定数を更新せず、CSM もスキップする (従来挙動と同じ)。
	void BeginFrame();
	void RenderBasePass(FScene* Scene, const FSceneView& View);
	// ベロシティパス (RenderVelocities, VelocityRendering.cpp)。RenderBasePass の直後に呼ぶこと
	// (ベースパス深度 = DEPTH_WRITE をテストに使い、可視性マップ / b0 を再利用する)。
	// 必要な時 (TAA 有効 / ベロシティを読むデバッグ表示 / bForceVelocityPass) だけ Velocity をクリアして
	// 前フレームから動いたプリミティブを描き、m_bVelocityValid = true にする
	void RenderVelocities(FScene* Scene);
	// シャドウ深度パス (RenderShadowDepthMaps)。RenderBasePass の後、
	// RenderLighting の前に呼ぶこと (ComputeLightGrid の結果を使う)。
	void RenderShadowDepths(FScene* Scene, const FSceneView& View);
	// Lumen シーン更新 (カードキャプチャ + Surface Cache ライティング)。
	// RenderShadowDepths の後 / RenderLighting の前に呼ぶこと
	// (b0/b1 を上書きするため。カメラの b0 は RenderLighting 先頭で
	//  積み直される。ライトバッファは ComputeLightGrid の結果を使う)。
	void RenderLumenScene(FScene* Scene);
	void RenderLighting();
	// トランスルーセンシーパス (RenderTranslucency 相当)。
	// RenderLighting の後 (SceneColor 確定後)、RenderPostProcessing の
	// 前に呼ぶこと。可視トランスルーセントプリミティブを
	// TranslucencySortPriority + m_TranslucencyParams.SortPolicy (ETranslucentSortPolicy。既定 SortByDistance) で
	// 後→前にソートし、SceneColor へフォワードシェーディングで合成する。
	void RenderTranslucency(FScene* Scene);
	void RenderPostProcessing();
	void EndFrame();

	// ---- アクセサ ----
	FSceneTextures* GetSceneTextures() { return &m_SceneTextures; }

	// RHI (ImGui の統計表示: 空きデスクリプタ数 / 遅延解放キュー長 / VRAM 使用量)
	RenderManager* GetRHI() const { return m_RHI; }

	// Color grading LUT baker (ImGui / SettingsManager からの Artist LUT 読込 / 解除 / Weight 操作・INI 保存用)
	class ColorGradingLUTBaker* GetColorGradingLUTBaker() { return m_ColorGradingLUTBaker.get(); }

	// Auto exposure system (for ImGui parameter control).
	class AutoExposure* GetAutoExposure() { return m_AutoExposure.get(); }

	// Light grid (タイルドライトカリング。ImGui のデバッグ制御用)
	class FLightGridInjection* GetLightGrid() { return m_LightGrid.get(); }

	// Shadow renderer (ImGui のシャドウカリング統計表示用)
	FShadowSceneRenderer* GetShadowRenderer() { return m_ShadowRenderer.get(); }

	// Lumen Surface Cache (ImGui の Lumen ウィンドウ用)
	FLumenSceneData* GetLumenScene() { return m_LumenScene.get(); }

	// Exponential Height Fog / Volumetric Fog (ImGui の Details / SettingsManager 用)
	FFogSceneRenderer* GetFogRenderer() { return m_FogRenderer.get(); }

	// ---- スクリーンショット ----
	// 次の RenderPostProcessing でバックバッファ (UI 無し) を 24bit BMP に保存する。
	// Path が空なら自動パス (Saved/Screenshots/...)。F9 / テストドライバから呼ぶ。
	void RequestScreenshot(const std::string& Path);
	// 記録済み・未書き出しのキャプチャを WaitGPU して書き出す (テストドライバの終了前)
	void FlushScreenshots();
	// 書き出し件数 / 失敗件数の参照 (テストドライバの終了コード判定用)
	const FScreenshotCapture* GetScreenshotCapture() const { return m_Screenshot.get(); }

	// ---- アンチエイリアシング / スクリーンパーセンテージ (ImGui / SettingsManager / テストドライバ) ----
	FAntiAliasingParams&      GetAntiAliasingParams() { return m_AAParams; }
	const FAntiAliasingParams& GetAntiAliasingParams() const { return m_AAParams; }
	FTemporalAADebugSettings& GetTemporalAADebugSettings() { return m_TAADebug; }
	const FViewFamilyInfo&    GetViewFamily() const { return m_ViewFamily; }
	const FViewInfo&          GetViewInfo() const { return m_ViewInfo; }
	const FSceneViewState&    GetViewState() const { return m_ViewState; }
	const FTemporalAAStats&   GetTemporalAAStats() const { return m_TAAStats; }
	// Temporal AA 本体 (自己テストの GPU パリティ / ImGui の履歴サムネイル / R11G11B10 対応表示用)
	FDefaultTemporalUpscaler* GetTemporalUpscaler() { return m_TemporalUpscaler.get(); }
	// Responsive AA マスクを今フレーム描いたか (RenderResponsiveAAMask。ImGui のサムネイル表示用)
	bool IsResponsiveAAMaskValid() const { return m_bResponsiveMaskValid; }

	// ---- 自己テストの出力 (RunTemporalAASelfTests が書き、テストドライバがログへ移す) ----
	void AddSelfTestLogLine(const std::string& Line) { m_SelfTestLog.push_back(Line); }
	std::vector<std::string> TakeSelfTestLog() { std::vector<std::string> out; out.swap(m_SelfTestLog); return out; }

	// ---- フラスタムカリング制御 (ImGui デバッグ用) ----
	struct FCullingParams
	{
		bool bEnableFrustumCulling = true;	// false = 全プリミティブを描画 (距離カリングも停止)
		bool bFreezeFrustum = false;		// フラスタムを凍結してカリングを可視化
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

	// ---- ライト統計 (毎フレーム ComputeLightVisibility / ComputeLightGrid が更新) ----
	struct FLightStats
	{
		int NumSceneLights = 0;			// FScene::Lights に居るライト数
		int NumFrustumCulled = 0;		// フラスタムで棄却されたローカルライト数
		int NumDistanceCulled = 0;		// 描画距離 / 最小スクリーン半径で棄却されたローカルライト数
		int NumLocalLights = 0;			// ライトバッファに積んだローカルライト数
		int NumDirectionalLights = 0;	// ライトバッファに積んだディレクショナルライト数
		int NumLumenLights = 0;			// Lumen 用ライトバッファに積んだライト数
	};

	const FLightStats& GetLightStats() const { return m_LightStats; }

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
	FLightStats    m_LightStats;
};
