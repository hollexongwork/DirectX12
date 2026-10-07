#pragma once
#include <memory>
#include <vector>
#include <cstdint>
#include <cstddef>
#include <DirectXMath.h>
#include "RenderManager.h"
#include "AntiAliasingSettings.h"
#include "ScreenPercentage.h"
#include "SceneViewState.h"

// ============================================================
//  TemporalAA
//  TAA / TAAU (FTAAStandaloneCS)。
//  ITemporalUpscaler / FDefaultTemporalUpscaler::AddPasses / AddTemporalAAPass
//  に対応する。TAA はコンピュートパス (8x8 スレッドグループ、専用の
//  コンピュートルートシグネチャ §3.7) で、順列ごとの .cso (TemporalAA_*_CS) を持つ。
//
//  フレーム内の位置 (§4.7): DOF (R) の後、AutoExposure / Bloom の前。
//    入力 : SceneColor (R, PSR -> NPSR -> PSR), LinearDepth (不透明のビュー Z), Velocity,
//           前フレームの履歴 (Hp。FSceneViewState::TemporalAAHistoryPool のピンポン),
//           前フレームの露出 (AutoExposure 結果 or ダミー), Responsive マスク
//    出力 : 新しい履歴 (H) = 後段 (AutoExposure / Bloom / Tonemap) の入力
//  履歴は CommitViewState (RenderPostProcessing の最後) が PrevFrameViewInfo へ確定する。
//
//  構成 (FViewFamilyInfo が選ぶ):
//    Main              : Low / Medium / High / MediumHigh (R -> R)
//    MainUpsampling    : TAAU (R -> O)。Low / Medium / High / MediumHigh
//    MainSuperSampling : HistoryScreenPercentage > 100 (S -> H、High 固定) + Mitchell-Netravali (H -> S)
//    Low + AllowDownsampling : ハーフ解像度出力 (Downsample 順列, AutoExposure / Bloom の入力)
//    Low / Medium + R11G11B10History : R11G11B10 履歴 (アンチゴースト alpha を持たない)
//
//  設計書 = Docs/TAAU/TAAU_Design.md。コード中の §x.y / Appendix A.x/B / decision N / リスク Rn は
//  すべてこの文書を指す。
//  実装上の規約 (設計書 Appendix B):
//    - バッファモデル          : レンダー解像度ぴったりのテクスチャで原点 (0,0)
//                                (既存のスクリーンシェーダを無変更で使うため)
//    - 空間アップスケールの範囲 : 10-200 % (メモリ)。TAAU は 50-200 %
//    - TAA の深度入力          : 不透明の LinearDepth (ビュー Z) をデバイス Z
//                                (d = Q - Q n / z) へ変換して使う (半透明パス後の深度バッファは半透明の深度を持つため)
//    - 遠方画素                : far = 500 m の標準 Z なので、ビュー Z >= 0.999 f の画素を
//                                d = Q (無限遠) として回転のみの再投影にする
//    - Responsive AA           : 深度が D32_FLOAT (ステンシル無し) なので
//                                R8_UNORM マスクパス (RenderResponsiveAAMask) で表す
//    - 入力のサニタイズ         : 入力 / 履歴 / 出力のすべてを浄化する
//    - w <= 0 ガード            : カメラモーション / ベロシティの経路に入れる (大移動 / 背後の点)
//    - ベロシティの ±2 クランプ : |V| > 2.0038 がエンコード値 0 (未書き込みの印) と衝突するのを防ぐ
//    - Main の Catmull-Rom 重み : |x| >= 2 を 0 とし、Σw <= 1e-6 か Σw < 0.25 Σ|w| なら最近傍サンプル
//                                = 1 へフォールバックする (台の外の 3 次式が巨大な負の重みになるのを防ぐ)
//    - ClipToPrevClip の合成    : ViewOrigin の差を明示的に分離し、カメラ相対のまま double で合成する
//    - BlendFinal 表示 (可視化 5) : アンチストールの下限を掛ける前の重みを示す
//                                (ブレンド自体は下限を使う。収束した平坦画素は下限が 1 になり信号が無いため)
//    - 履歴 alpha フラグ        : TAA_FLAG_HISTORY_HAS_ALPHA。R11G11B10 の SRV は a = 1 を返すので、
//                                その履歴を読むフレームはアンチゴーストの alpha を使わない
//    - R11G11B10 の条件         : Quality 0/1、SuperSampling 以外、
//                                かつ UAV 型付きストア対応時のみ (アンチゴースト alpha が無いことと整合)
//    - N == 1 のジッタ          : オフセット 0 (AA 無しと厳密比較できる)
//    - b0 StateFrameIndexMod8   : AA 無効時 0 (DeferredPS の IGN フレーム項が
//                                AA 無しの画像を変えないように)
//    - 大移動時の Lumen / Fog 履歴 : Prev 行列がリセットされる 1 フレームだけ無効にする
//    - ベロシティ出力方式        : 別パスのみ
//                                (PS_INPUT と G-Buffer の MRT 構成を変えないため)
//    - ミップバイアスの方式      : SampleBias + b0 MaterialTextureMipBias
//                                (静的サンプラーがルートシグネチャに焼き込まれているため)
//    - プリエクスポージャ        : 絶対 HDR で描くので無し (補正 = 1、
//                                フック SceneColorPreExposure は残す)
//    - LightGrid の確保          : 容量 2 O を 1 度だけ確保し、次元を毎フレーム設定する
//    - TAA はコンピュートのみ     : #615 (DSV 形式) 回避と単純化のため CS のみ
//    - TAA_SCREEN_PERCENTAGE_RANGE : 順列を畳み込む (Load で結果は同一)
//    - 既定値                    : Upsampling の既定は 1
// ============================================================

// ---- TAA CB の Flags (HLSL TemporalAA.hlsl の TAA_FLAG_* と同値) ----
enum ETAAFlags : uint32_t
{
	TAA_FLAG_UPSAMPLE_FILTERED     = 1u << 0,   // bTemporalAAUpsampleFiltered (MainUpsampling)
	TAA_FLAG_RESPONSIVE_MASK_VALID = 1u << 1,   // マスクを今フレーム描いた
	TAA_FLAG_EYE_ADAPTATION_BUFFER = 1u << 2,   // t4 EyeAdaptation[0] を使う (無効時 ManualExposure)
	TAA_FLAG_HISTORY_HAS_ALPHA     = 1u << 3,   // 入力履歴が RGBA16F (R11G11B10 は a=1 を返すためクリア)
	TAA_FLAG_FTW_MODE_SHIFT        = 4,         // bits 4-5: FilteredTemporalWeight 定義 (0 総和 / 1 最近傍 / 2 = 1)
	TAA_FLAG_DOWNSAMPLE_OUTPUT     = 1u << 6,   // u1 へ書く (DOWNSAMPLE 順列内の安全ゲート)
};

// ------------------------------------------------------------
//  TAA 定数バッファ (TAA ルートシグネチャのルート CBV b0)。336 B / リング 1 スロット 512 B (§3.4)
//  HLSL: cbuffer TemporalAAParameters : register(b0) (TemporalAA.hlsl) と 1:1
// ------------------------------------------------------------
struct FTemporalAAParameters
{
	DirectX::XMFLOAT4   InputSceneColorSize;            //   0 (R.x, R.y, 1/R.x, 1/R.y)
	DirectX::XMINT4     InputMinMaxPixelCoord;          //  16 (0, 0, R.x-1, R.y-1)
	DirectX::XMFLOAT4   OutputViewportSize;             //  32 (H.x, H.y, 1/H.x, 1/H.y)
	DirectX::XMFLOAT4   HistoryBufferSize;              //  48 (Hp.x, Hp.y, 1/Hp.x, 1/Hp.y) 入力履歴の実寸
	DirectX::XMFLOAT4   HistoryBufferUVMinMax;          //  64 (0.5/Hp.x, 0.5/Hp.y, (Hp.x-0.5)/Hp.x, (Hp.y-0.5)/Hp.y)
	DirectX::XMFLOAT4   ScreenPosToHistoryBufferUV;     //  80 (0.5, -0.5, 0.5, 0.5) exact-size なので定数
	DirectX::XMFLOAT4X4 ClipToPrevClip;                 //  96 転置済み (NoAA x NoAA)
	DirectX::XMFLOAT2   TemporalJitterPixels;           // 160 レンダー px (+y 下)
	DirectX::XMFLOAT2   ScreenPosAbsMax;                // 168 (1-1/Hp.x, 1-1/Hp.y) (AA_BICUBIC=0 経路用。現状未使用)
	float               ScreenPercentage;               // 176 R.x / H.x
	float               UpscaleFactor;                  // 180 H.x / R.x
	float               CurrentFrameWeight;             // 184
	float               HistoryPreExposureCorrection;   // 188 = 1 (プリエクスポージャ無し)
	uint32_t            bCameraCut;                     // 192 View.bCameraCut || !InputHistory.IsValid() || bForceHistoryBypass
	uint32_t            Flags;                          // 196 ETAAFlags
	float               ManualExposure;                 // 200 PostProcess.Exposure
	uint32_t            DebugMode;                      // 204 ETemporalAADebugView (5..13 のみ書く。それ以外 0)
	DirectX::XMFLOAT4   SampleWeights[3];               // 208 [0..8] (i>>2, i&3) Main のみ
	DirectX::XMFLOAT4   PlusWeights[2];                 // 256 [0..4] Main のみ
	DirectX::XMFLOAT4   OutputQuantizationError;        // 288 xyz = ComputePixelFormatQuantizationError(出力フォーマット), w = 最大有限値
	DirectX::XMFLOAT4   DepthParams;                    // 304 x = Q = f/(f-n), y = -Q*n (d = x + y/viewZ), z = 0.999*f (遠方判定), w = 0
	uint32_t            StateFrameIndexMod8;            // 320 FrameIndex & 7 (常に回す)
	float               DebugScale;                     // 324
	float               SampleDistanceThreshold;        // 328 = 1.51 + (1.3 - 1.51) * (UpscaleFactor - 1) (クランプ無し) [H]
	uint32_t            Pad0;                           // 332
};
static_assert(sizeof(FTemporalAAParameters) == 336, "FTemporalAAParameters must mirror HLSL TemporalAAParameters (336 B)");
static_assert(offsetof(FTemporalAAParameters, ClipToPrevClip) == 96, "FTemporalAAParameters::ClipToPrevClip offset");
static_assert(offsetof(FTemporalAAParameters, TemporalJitterPixels) == 160, "FTemporalAAParameters::TemporalJitterPixels offset");
static_assert(offsetof(FTemporalAAParameters, ScreenPercentage) == 176, "FTemporalAAParameters::ScreenPercentage offset");
static_assert(offsetof(FTemporalAAParameters, bCameraCut) == 192, "FTemporalAAParameters::bCameraCut offset");
static_assert(offsetof(FTemporalAAParameters, SampleWeights) == 208, "FTemporalAAParameters::SampleWeights offset");
static_assert(offsetof(FTemporalAAParameters, PlusWeights) == 256, "FTemporalAAParameters::PlusWeights offset");
static_assert(offsetof(FTemporalAAParameters, OutputQuantizationError) == 288, "FTemporalAAParameters::OutputQuantizationError offset");
static_assert(offsetof(FTemporalAAParameters, DepthParams) == 304, "FTemporalAAParameters::DepthParams offset");
static_assert(offsetof(FTemporalAAParameters, StateFrameIndexMod8) == 320, "FTemporalAAParameters::StateFrameIndexMod8 offset");

// ------------------------------------------------------------
//  Mitchell-Netravali ダウンサンプルの定数バッファ (同じリングのスロット 1、ルート CBV b0)。48 B (§3.5)
//  HLSL: cbuffer MitchellNetravaliParameters : register(b0) (TemporalAAMitchellNetravali_CS.hlsl) と 1:1
// ------------------------------------------------------------
struct FMitchellNetravaliParameters
{
	DirectX::XMFLOAT4 InputSize;                    //  0 (H.x, H.y, 1/H.x, 1/H.y)
	DirectX::XMFLOAT4 OutputSize;                   // 16 (S.x, S.y, 1/S.x, 1/S.y)
	DirectX::XMFLOAT2 InputPerOutputPixel;          // 32 (H.x/S.x, H.y/S.y) (1..2)
	DirectX::XMFLOAT2 Pad;                          // 40
};
static_assert(sizeof(FMitchellNetravaliParameters) == 48, "FMitchellNetravaliParameters must mirror HLSL MitchellNetravaliParameters (48 B)");
static_assert(offsetof(FMitchellNetravaliParameters, InputPerOutputPixel) == 32, "FMitchellNetravaliParameters::InputPerOutputPixel offset");

// ------------------------------------------------------------
//  FTAAPassParameters (1 回の TAA パスの入力一式)
// ------------------------------------------------------------
struct FTAAPassParameters
{
	ETAAPassConfig Pass = ETAAPassConfig::Main;
	ETAAQuality    Quality = ETAAQuality::High;
	bool           bDownsample = false;             // ハーフ解像度出力
	bool           bUseR11G11B10History = false;
	bool           bUpsampleFiltered = true;
	RENDER_TARGET* SceneColorInput = nullptr;       // R, PSR
	unsigned int   SceneDepthSRVIndex = 0;          // LinearDepth (RG32F, R = view Z, 不透明のみ)
	unsigned int   SceneVelocitySRVIndex = 0;       // Velocity (R16G16_UNORM) またはダミー
	unsigned int   ResponsiveMaskSRVIndex = 0;      // ResponsiveAAMask (R8_UNORM) またはダミー
	bool           bResponsiveMaskValid = false;
	unsigned int   EyeAdaptationSRVIndex = 0;       // AutoExposure 結果 or ダミー
	bool           bUseEyeAdaptationBuffer = false;
	float          ManualExposure = 1.0f;           // PostProcess.Exposure
	DirectX::XMUINT2 InputExtent{};                 // InputViewRect サイズ = R
	DirectX::XMUINT2 OutputExtent{};                // OutputViewRect サイズ = H
	float          CurrentFrameWeight = 0.04f, FilterSize = 1.0f;
	bool           bCatmullRom = false;
	bool           bForceHistoryBypass = false;     // CB bCameraCut = 1 (カット扱いはしない)
	ETemporalAADebugView DebugView = ETemporalAADebugView::Off;
	float          DebugScale = 1.0f;
	int            FilteredTemporalWeightMode = 0;
	float          NearClip = 0.1f, FarClip = 500.0f;
};

struct FTAAOutputs
{
	FTAATexture* SceneColor = nullptr;              // 新しい履歴 (H)
	FTAATexture* DownsampledSceneColor = nullptr;   // ハーフ解像度
	bool         bHistoryValid = false;             // 入力履歴を読んだ (カット / 無効 / バイパスでない)
};

// ------------------------------------------------------------
//  ITemporalUpscaler。実装は FDefaultTemporalUpscaler のみ
// ------------------------------------------------------------
class ITemporalUpscaler
{
public:
	struct FPassInputs
	{
		bool           bAllowDownsampleSceneColor = false;
		RENDER_TARGET* SceneColorTexture = nullptr;
		unsigned int   SceneDepthSRVIndex = 0, SceneVelocitySRVIndex = 0, ResponsiveMaskSRVIndex = 0;
		bool           bResponsiveMaskValid = false;
		unsigned int   EyeAdaptationSRVIndex = 0;
		bool           bUseEyeAdaptationBuffer = false;
		float          ManualExposure = 1.0f;
		bool           bForceHistoryBypass = false;
	};
	struct FPassOutputs
	{
		RENDER_TARGET*   SceneColor = nullptr;        DirectX::XMUINT2 SceneColorExtent{};   // S, PSR
		RENDER_TARGET*   HalfResSceneColor = nullptr; DirectX::XMUINT2 HalfResExtent{};      // PSR or null
		FTemporalAAHistory NewHistory;                                                     // CommitViewState へ渡す
		bool             bHistoryValid = false;       // 入力履歴を読んだ (統計 / ログ用)
	};
	virtual ~ITemporalUpscaler() = default;
	virtual const char* GetDebugName() const = 0;
	virtual bool  IsReady(const FViewFamilyInfo& Family) const = 0;   // 必要な PSO が全て存在するか
	virtual FPassOutputs AddPasses(const FViewInfo& View, const FViewFamilyInfo& Family, FSceneViewState& ViewState,
		const FAntiAliasingParams& Params, const FTemporalAADebugSettings& Debug, const FPassInputs& Inputs) = 0;
	virtual float GetMinUpsampleResolutionFraction() const = 0;       // 0.5
	virtual float GetMaxUpsampleResolutionFraction() const = 0;       // 2.0
};

// ------------------------------------------------------------
//  FDefaultTemporalUpscaler (TAAU)
//  コンピュートルートシグネチャ (§3.7):
//    [0] ルート CBV b0 (FTemporalAAParameters / MN / 自己テスト)
//    [1..6] SRV テーブル t0..t5  [7..9] UAV テーブル u0..u2 (各 1 デスクリプタ)
//    静的サンプラ s0 = ポイントクランプ, s1 = リニアクランプ
//  全テーブルを毎ディスパッチでバインドする (リソースが無い所はダミー。GBV を静かに保つ)
// ------------------------------------------------------------
class FDefaultTemporalUpscaler final : public ITemporalUpscaler
{
public:
	explicit FDefaultTemporalUpscaler(RenderManager* RHI);
	~FDefaultTemporalUpscaler() override;

	FDefaultTemporalUpscaler(const FDefaultTemporalUpscaler&) = delete;
	FDefaultTemporalUpscaler& operator=(const FDefaultTemporalUpscaler&) = delete;

	// RS / PSO (Try) / CB リング / ダミー / 自己テストバッファ / R11G11B10 UAV 対応確認。
	// FSceneRenderer のコンストラクタ (コマンドリスト記録中) から 1 度だけ呼ぶ
	void Init();

	const char* GetDebugName() const override { return "Gen4 TAAU"; }
	// m_PSO[Pass][Quality][Downsample] が存在するか。初回の false (組合せごと) だけログを出す
	bool  IsReady(const FViewFamilyInfo& Family) const override;
	FPassOutputs AddPasses(const FViewInfo& View, const FViewFamilyInfo& Family, FSceneViewState& ViewState,
		const FAntiAliasingParams& Params, const FTemporalAADebugSettings& Debug, const FPassInputs& Inputs) override;   // §4.8.2
	float GetMinUpsampleResolutionFraction() const override { return kMinTAAUpsampleResolutionFraction; }
	float GetMaxUpsampleResolutionFraction() const override { return kMaxTAAUpsampleResolutionFraction; }

	// AddTemporalAAPass(View, Inputs, InputHistory, OutputHistory) (§4.8.3)
	FTAAOutputs AddTemporalAAPass(const FViewInfo& View, const FTAAPassParameters& P, const FTemporalAAHistory& InputHistory,
		FTemporalAAHistory* OutputHistory, FSceneViewState& ViewState);
	// MainSuperSampling の後段 (§4.8.4): TAA 出力 (履歴 H, RD) を Mitchell-Netravali で OutputExtent (= S) へ
	// ダウンサンプルする。結果は m_MNOutput (RGBA16F, PSR = AutoExposure の入口契約)。PSO 欠落時は null
	FTAATexture* ComputeMitchellNetravaliDownsample(FTAATexture* Input, DirectX::XMUINT2 OutputExtent);

	// CS デバッグ表示 (ETemporalAADebugView 5..13) の出力 (RD 常駐)。未確保なら null
	FTAATexture* GetDebugOutput() { return m_DebugOutput.RT ? &m_DebugOutput : nullptr; }
	// AA 無効時: ハーフ解像度 / Mitchell-Netravali / デバッグ出力を解放する (~RENDER_TARGET の遅延解放。フレーム途中でも安全)
	void ReleaseAuxiliaryTargets() { m_HalfRes.Release(); m_MNOutput.Release(); m_DebugOutput.Release(); }
	bool IsR11G11B10HistorySupported() const { return m_bR11G11B10Supported; }

	// GPU 自己テスト (TemporalAASelfTest_CS, §4.8.5)。BeginFrame 先頭からのみ呼ぶ
	// (FlushAndResetCommandList で 2 回 GPU を待つ)。OutValues に 64 個の float を返す
	bool RunGPUSelfTest(std::vector<float>& OutValues);
	bool HasSelfTestPSO() const { return m_PSOSelfTest != nullptr; }

	// 1x1 RGBA16F (0,0,0,1) (RD 常駐)。未書き込みベロシティ / マスク / 未使用 SRV の代替
	unsigned int GetDummySRVIndex() const;
	// UPLOAD バッファ float[2] = {1, 1} の型付き SRV (R32_FLOAT)。露出が未初期化の時の t4
	unsigned int GetDummyEyeAdaptationSRVIndex() const { return m_DummyEyeAdaptationSRV; }

private:
	// 定数リング: フレーム (0/1) ごとに 4 スロット x 512 B (0 TAA, 1 MN, 2 自己テスト, 3 予備)
	static constexpr unsigned int kParamSlotSize = 512;
	static constexpr unsigned int kParamSlotCount = 4;
	static constexpr unsigned int kParamSlotTAA = 0;
	static constexpr unsigned int kParamSlotMitchellNetravali = 1;
	static constexpr unsigned int kParamSlotSelfTest = 2;
	static constexpr unsigned int kSelfTestValueCount = 64;

	// Lumen の TryCreateComputePipeline と同じ: .cso 欠落 / 生成失敗はログのみで null を返す
	ComPtr<ID3D12PipelineState> TryCreateComputePipeline(const char* CsoFile);
	// 全ルートテーブルを (ダミー込みで) バインドする。SRV は t0..t5, UAV は u0..u2 の SRV ヒープ添字
	void BindComputeTables(ID3D12GraphicsCommandList* CommandList, const unsigned int SRVs[6], const unsigned int UAVs[3]);

	RenderManager* m_RHI = nullptr;
	ComPtr<ID3D12RootSignature> m_RootSignature;
	ComPtr<ID3D12PipelineState> m_PSO[kNumTAAPassConfigs][kNumTAAQualities][2];   // [Pass][Quality][Downsample]; 存在しない組合せは null
	ComPtr<ID3D12PipelineState> m_PSOMitchellNetravali;   // MainSuperSampling の H -> S ダウンサンプル
	ComPtr<ID3D12PipelineState> m_PSOSelfTest;
	ComPtr<ID3D12Resource> m_ParamBuffer[2];        // フレーム毎 4 スロット x 512 B (UPLOAD, 永続 Map)
	uint8_t* m_ParamPtr[2] = {};
	FTAATexture m_HalfRes;                          // ハーフ解像度出力 (ceil(H/2), RGBA16F, PSR 常駐。Low + AllowDownsampling)
	FTAATexture m_MNOutput;                         // Mitchell-Netravali 出力 (S, RGBA16F, PSR 常駐)
	FTAATexture m_DebugOutput;                      // CS デバッグ表示 (H, RGBA16F)
	std::unique_ptr<RENDER_TARGET> m_DummyTex;      // 1x1 RGBA16F (0,0,0,1), RD 常駐 (SRV 用)
	std::unique_ptr<RENDER_TARGET> m_DummyUAVTex;   // 1x1 RGBA16F, UNORDERED_ACCESS 常駐 (未使用 u1/u2 用)
	ComPtr<ID3D12Resource> m_DummyEyeAdaptation;    // UPLOAD, float[2] = {1,1}, GENERIC_READ
	unsigned int m_DummyEyeAdaptationSRV = 0;
	ComPtr<ID3D12Resource> m_SelfTestBuffer;        // DEFAULT, 64 floats (構造化 UAV, stride 4)。COMMON 開始 (バッファは減衰する)
	ComPtr<ID3D12Resource> m_SelfTestReadback;      // READBACK, 64 floats
	unsigned int m_SelfTestUAV = 0;
	bool m_bR11G11B10Supported = false;
	mutable uint32_t m_LoggedMissingPSOMask = 0;    // IsReady のログ済み組合せ (bit = Pass*8 + Quality*2 + Downsample)
};

// ---- CPU 側の計算 ----
// Main 構成の 3x3 / プラス 5 サンプル重み (§4.8.6)。ガウス exp(-2.29 |o - J|^2 / FS^2) または
// Catmull-Rom (|x| >= 2 は 0) を正規化。総和が極小 / 負、または負ローブの相殺で
// 悪条件 (Σw < 0.25 Σ|w|) なら J に最も近いサンプル = 1 へフォールバック
void ComputeTemporalAASampleWeights(DirectX::XMFLOAT2 JitterPixels, float FilterSize, bool bCatmullRom,
	float OutSampleWeights[9], float OutPlusWeights[5]);
// 出力フォーマットの 1 ULP (確率的量子化の誤差): RGBA16F (2^-10)x3, R11G11B10 (2^-6, 2^-6, 2^-5)
DirectX::XMFLOAT3 ComputePixelFormatQuantizationError(DXGI_FORMAT Format);

// ---- TemporalAACommon.hlsl の CPU 鏡像 (自己テスト T14 用。HLSL と同式) ----
DirectX::XMFLOAT3 RGBToYCoCgCPU(DirectX::XMFLOAT3 C);
DirectX::XMFLOAT3 YCoCgToRGBCPU(DirectX::XMFLOAT3 C);
float             HdrWeightYCPU(float Y, float Exposure);
DirectX::XMFLOAT2 WeightedLerpFactorsCPU(float WeightA, float WeightB, float Blend);

// ---- TAAU / 再サンプルカーネルの CPU 鏡像 (自己テスト T8-T11 用。HLSL と同式) ----
// ComputeSampleWeigth: Blackman-Harris 近似 (0.905 x^2 - 1.9) x^2 + 1, x^2 = saturate(UF^2 |d|^2)
float ComputeSampleWeigthCPU(DirectX::XMFLOAT2 PixelDelta, float UpscaleFactor);
// TAAU の入力写像 (TemporalAA.hlsl 手順 1-2): 出力画素 p の中心が写るジッタ済み入力座標
// PPCo = (p + 0.5) / O * R + J、最近接入力画素 K = clamp(floor(PPCo)) と dKO = PPCo - (floor(PPCo) + 0.5)。
// 参照値として double で計算する (GPU の float では 1920 付近で 1 ULP = 6.1e-5 px の差が出る)
void ComputeTAAUInputMappingCPU(DirectX::XMUINT2 OutputPixel, DirectX::XMUINT2 OutputExtent, DirectX::XMUINT2 InputExtent,
	DirectX::XMFLOAT2 JitterPixels, DirectX::XMINT2& OutK, DirectX::XMFLOAT2& OutDKO);
// CatmullRom5Taps の 1D 重み (タップ -1, 0, +1, +2)。f = UV * Size - (floor(UV * Size - 0.5) + 0.5)
void CatmullRomWeights1DCPU(float F, float OutWeights[4]);
// Mitchell-Netravali (B = C = 1/3) [L]
float MitchellNetravaliCPU(float X);
// TemporalAAMitchellNetravali_CS の 1 軸分: 出力画素 OutputPixel の 9 タップ (先頭の入力画素 OutFirst) の
// 正規化重みと、正規化前の総和 (= 比 InputPerOutputPixel)。2D の正規化重みはこの積
void ComputeMitchellNetravaliTapsCPU(float OutputPixel, float InputPerOutputPixel, int& OutFirst, float OutWeights[9], float& OutRawSum);
