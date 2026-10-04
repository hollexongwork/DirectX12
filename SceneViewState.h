#pragma once
#include <memory>
#include <vector>
#include <DirectXMath.h>
#include "RenderManager.h"
#include "AntiAliasingSettings.h"
#include "ViewMatrices.h"

// ============================================================
//  SceneViewState
//  FViewInfo (1 フレーム分のビュー) / FSceneViewState (フレームを
//  跨いで永続するビュー状態) / FPreviousViewInfo (前フレームのビュー
//  情報 + TAA 履歴) に相当する型。
//
//  データフロー (§4.4 / §4.9):
//    PrepareViewStateForVisibility (RenderBasePass 先頭):
//      FViewInfo を毎フレーム再構築し、FSceneViewState::PrevFrameViewInfo を
//      FViewInfo::PrevViewInfo へスナップショットする (カット / 大移動の
//      リセットを適用後、フレーム中は不変)
//    フレーム中の全コンシューマ (Lumen / Volumetric Fog / 後の TAA /
//      ベロシティ) は FViewInfo::PrevViewInfo だけを読む
//    CommitViewState (RenderPostProcessing の最後):
//      今フレームの行列 / TAA 履歴 / ViewRectSize を PrevFrameViewInfo へ確定
// ============================================================

// ------------------------------------------------------------
//  FTAATexture: UAV 付き RENDER_TARGET + 追跡状態
// ------------------------------------------------------------
struct FTAATexture
{
	std::unique_ptr<RENDER_TARGET> RT;
	DirectX::XMUINT2      Extent{};
	DXGI_FORMAT           Format = DXGI_FORMAT_UNKNOWN;
	D3D12_RESOURCE_STATES State = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;   // CreateRenderTarget の初期状態

	bool Matches(DirectX::XMUINT2 E, DXGI_FORMAT F) const { return RT && Extent.x == E.x && Extent.y == E.y && Format == F; }
	// RT.reset() (遅延解放) + 生成 (UAV 付き), State = PSR
	void Allocate(RenderManager* RHI, DirectX::XMUINT2 E, DXGI_FORMAT F, const wchar_t* Name);
	void Release() { RT.reset(); Extent = {}; Format = DXGI_FORMAT_UNKNOWN; }
};

// ------------------------------------------------------------
//  FTemporalAAHistory (RT[0] のみ)
// ------------------------------------------------------------
struct FTemporalAAHistory
{
	int              RTSlot = -1;                    // FSceneViewState::TemporalAAHistoryPool の添字
	DirectX::XMUINT2 ReferenceBufferSize{};          // テクスチャ実寸 (exact-size)
	DirectX::XMUINT2 ViewportSize{};                 // = ReferenceBufferSize (ViewportRect.Min = 0)
	DXGI_FORMAT      Format = DXGI_FORMAT_UNKNOWN;

	bool IsValid() const { return RTSlot >= 0; }
	void SafeRelease() { *this = FTemporalAAHistory{}; }
};

// ------------------------------------------------------------
//  FPreviousViewInfo: 前フレームのビュー情報
// ------------------------------------------------------------
struct FPreviousViewInfo
{
	FViewMatrices      ViewMatrices;                 // 前フレームのジッタ込み + NoAA + ジッタ値
	FTemporalAAHistory TemporalAAHistory;
	DirectX::XMUINT2   ViewRectSize{};               // Lumen / Fog 履歴 (レンダー解像度) の有効判定用
	float              SceneColorPreExposure = 1.0f; // プリエクスポージャ無し
};

// ------------------------------------------------------------
//  FSceneViewState: ビュー 1 つ分の永続状態
// ------------------------------------------------------------
class FSceneViewState
{
public:
	uint32_t          TemporalAASampleIndex = 0;
	uint32_t          FrameIndex = 0;                // StateFrameIndex (CommitViewState で +1)
	FPreviousViewInfo PrevFrameViewInfo;
	bool              bPrevFrameViewInfoValid = false;
	bool              bForceCameraCut = true;         // 次フレームを強制カット (初回 / bValid 復帰)
	EAntiAliasingMethod PrevAntiAliasingMethod = EAntiAliasingMethod::None;
	FTAATexture       TemporalAAHistoryPool[2];       // ピンポン

	FTAATexture* GetHistoryTexture(const FTemporalAAHistory& H) { return H.IsValid() ? &TemporalAAHistoryPool[H.RTSlot] : nullptr; }
	const FTAATexture* GetHistoryTexture(const FTemporalAAHistory& H) const { return H.IsValid() ? &TemporalAAHistoryPool[H.RTSlot] : nullptr; }
	uint32_t GetFrameIndexMod8() const { return FrameIndex & 7u; }
};

class FLightSceneProxy;

// ------------------------------------------------------------
//  FVisibleLightViewInfo: ビューごとのライトの可視情報
//  FSceneRenderer::ComputeLightVisibility が毎フレーム埋める。
//  FViewInfo::VisibleLightInfos を FLightSceneInfo::Id で引く
// ------------------------------------------------------------
struct FVisibleLightViewInfo
{
	unsigned int bInViewFrustum : 1;	// ビューフラスタム内かつ描画距離内
	unsigned int bInDrawRange : 1;		// 描画距離内 (フラスタム外でも真になり得る)

	FVisibleLightViewInfo()
		: bInViewFrustum(0)
		, bInDrawRange(0)
	{
	}
};

// ------------------------------------------------------------
//  FViewInfo: 1 フレーム分のビュー
// ------------------------------------------------------------
struct FViewInfo
{
	bool      bValid = false;
	DirectX::XMUINT2 ViewRectSize{};                 // R
	DirectX::XMUINT2 UnscaledViewRectSize{};         // O
	EAntiAliasingMethod            AntiAliasingMethod = EAntiAliasingMethod::None;
	EPrimaryScreenPercentageMethod PrimaryScreenPercentageMethod = EPrimaryScreenPercentageMethod::SpatialUpscale;
	FViewMatrices     ViewMatrices;
	FPreviousViewInfo PrevViewInfo;                  // フレーム先頭のスナップショット (リセット適用後, フレーム中不変)
	bool      bCameraCut = false;
	bool      bPrevTransformsReset = false;          // カット or 大移動: Prev 行列 = 今フレーム
	bool      bPrevViewInfoValid = false;            // 前フレームのビュー情報が使えるか (初回 / カットで false)
	DirectX::XMFLOAT2 TemporalJitterPixels{};        // レンダー px (+y 下)
	int       TemporalJitterIndex = 0;
	int       TemporalJitterSequenceLength = 1;
	float     MaterialTextureMipBias = 0.0f;
	DirectX::XMFLOAT4X4 ClipToPrevClip = kIdentity4x4; // 転置前 = InvVP_NoAA(cur) * VP_NoAA(prev)。ComputeClipToPrevClip (カメラ相対, double) で合成
	float     NearClip = 0.1f, FarClip = 500.0f;
	uint32_t  StateFrameIndex = 0;

	// ---- ライト (LightRendering.h) ----
	// FScene::Lights と同じ添字 (FLightSceneInfo::Id) のライト可視情報
	std::vector<FVisibleLightViewInfo> VisibleLightInfos;
	// フォワードシェーディング (半透明 / Volumetric Fog) と CSM が使うディレクショナルライト
	// (SelectedForwardDirectionalLightProxy)。null = なし
	const FLightSceneProxy* SelectedForwardDirectionalLightProxy = nullptr;
};

// 大きなカメラ移動の判定 (FSceneRenderer::IsLargeCameraMovement 相当)。
// ViewMatrix 上 3x3 の列 0/1/2 (= ワールド空間のカメラ右 / 上 / 前方向) の内積が
// cos(RotationThresholdDeg) 未満、または原点間距離の 2 乗が TranslationThresholdM^2 を超えたら true
bool IsLargeCameraMovement(const FViewMatrices& Cur, const FViewMatrices& Prev, float RotationThresholdDeg, float TranslationThresholdM);

// ClipToPrevClip (NoAA x NoAA, 行ベクトル: PrevClip = ThisClip * C2P, 転置前) (§4.4)。
// カメラ相対・double で合成する:
//   C2P = InvProjNoAA(cur) * InvRot(cur) * Translation(O_cur - O_prev) * Rot(prev) * ProjNoAA(prev)
// 絶対座標を行列に入れず原点差分のみを使うので、静止カメラでは厳密に単位行列になる
// (ワールド絶対座標の VP を float で逆行列 x 積にすると |カメラ位置| に比例した誤差が残る)
DirectX::XMFLOAT4X4 ComputeClipToPrevClip(const FViewMatrices& Cur, const FViewMatrices& Prev);

// TAA ジッタのサンプル数 N (§4.4)。
//   TemporalUpscale : N = int(CVar * max(1, 1 / f^2)) (出力画素あたりのサンプル密度一定。int32 代入 = 切り捨て)
//   それ以外         : N = CVar (CVar 5 = 圧縮プラスは N = 4)
// [1, 255] にクランプ。f = EffectivePrimaryResolutionFraction (R.x / O.x)
int ComputeTemporalAASampleCount(bool bTemporalUpsampling, int SamplesCVar, float ResolutionFraction);

// TAA ジッタのサンプル位置 [レンダー px, +y 下] (TemporalJitterPixels) (§4.4)。
//   N == 1           : (0, 0) (AA Off と厳密比較可能にする)
//   TemporalUpscale  : 一様 Halton(Index + 1, 2 / 3) - 0.5 (パターン分岐より先に判定)
//   CVar 2 / 3 / 4 / 5 : 固定パターン (添字は % 長さで保護。5 = 圧縮プラス)
//   それ以外         : 窓付きガウス (Box-Muller, sigma = 0.47 * FilterSize, 半径 0.5 で窓掛け)
DirectX::XMFLOAT2 ComputeTemporalAASample(bool bTemporalUpsampling, int SamplesCVar, int SequenceLength, int Index, float FilterSize);
