#pragma once
#include <DirectXMath.h>
#include "AntiAliasingSettings.h"

// ============================================================
//  ScreenPercentage
//  UE の FLegacyScreenPercentageDriver + PrepareViewRectsForRendering +
//  FDefaultTemporalUpscaler のパス構成選択に相当する「ビューファミリ」の
//  解像度 / AA 構成の決定。毎フレーム BeginFrame 先頭
//  (FSceneRenderer::PrepareViewRectsForRendering) で FAntiAliasingParams と
//  出力解像度 O から FViewFamilyInfo を求める。
//
//  5 つの解像度 (§0.1):
//    O : 出力 (バックバッファ, UnscaledViewRect)
//    R : レンダー (ViewRect) = ceil(O * f)
//    S : TAA パスの "Secondary" (TemporalUpscale なら O, Main なら R)
//    H : TAA 出力 = 今フレーム書く履歴 (S か trunc(S * HistoryUpscaleFactor))
//    P : ポストプロセス解像度 (TAA 実行時 S, AA 無効時 R)
// ============================================================

constexpr float kMinTAAUpsampleResolutionFraction = 0.5f;   // UE
constexpr float kMaxTAAUpsampleResolutionFraction = 2.0f;   // UE
constexpr float kMinSpatialResolutionFraction     = 0.1f;   // [PORT] UE 0.01
constexpr float kMaxSpatialResolutionFraction     = 2.0f;   // [PORT] UE 4.0

enum class ETAAPassConfig : int { Main = 0, MainUpsampling = 1, MainSuperSampling = 2 };
enum class ETAAQuality    : int { Low = 0, Medium = 1, High = 2, MediumHigh = 3 };
constexpr int kNumTAAPassConfigs = (int)ETAAPassConfig::MainSuperSampling + 1;
constexpr int kNumTAAQualities   = (int)ETAAQuality::MediumHigh + 1;
inline bool IsTAAUpsamplingConfig(ETAAPassConfig P) { return P != ETAAPassConfig::Main; }

struct FViewFamilyInfo
{
	EAntiAliasingMethod            AntiAliasingMethod = EAntiAliasingMethod::None;   // 実効値
	EPrimaryScreenPercentageMethod PrimaryScreenPercentageMethod = EPrimaryScreenPercentageMethod::SpatialUpscale;
	float    ResolutionFraction = 1.0f;                // クランプ後の要求値
	float    EffectivePrimaryResolutionFraction = 1.0f;// R.x / O.x (UE と同じく X で定義)
	DirectX::XMUINT2 OutputExtent{};                   // O
	DirectX::XMUINT2 RenderExtent{};                   // R
	DirectX::XMUINT2 SecondaryExtent{};                // S (TAA 無効時は R)
	DirectX::XMUINT2 HistoryExtent{};                  // H (TAA 無効時は R)
	DirectX::XMUINT2 PostProcessExtent{};              // P (予定値。実際の後段サイズは TAA 出力テクスチャから取る)
	bool     bTemporalAA = false;
	ETAAPassConfig TAAPass = ETAAPassConfig::Main;
	ETAAQuality    TAAQuality = ETAAQuality::High;
	float    HistoryUpscaleFactor = 1.0f;              // clamp(HSP/100, 1, 2)
	bool     bTAADownsample = false;
	bool     bR11G11B10History = false;
	bool     bSpatialUpscale = false;                  // P != O
};

// UE GetTemporalAAHistoryUpscaleFactor (Main にも適用 [H]): clamp(HSP/100, 1, 2)
float           GetTemporalAAHistoryUpscaleFactor(const FAntiAliasingParams& P);
// FAntiAliasingParams + 出力解像度 O -> FViewFamilyInfo (§4.2)。
// bR11G11B10Supported = R11G11B10_FLOAT の UAV 型付きストア対応 (FDefaultTemporalUpscaler::IsR11G11B10HistorySupported)。
// 毎フレーム PrepareViewRectsForRendering から呼ぶ。自己テスト T12 も同じ関数で §4.2 の表を照合する
FViewFamilyInfo ComputeViewFamilyInfo(const FAntiAliasingParams& Params, DirectX::XMUINT2 OutputExtent, bool bR11G11B10Supported);

// Automatic View Mip Bias (UE 4.26 は TAAU 分岐内で計算): TemporalUpscale 時のみ
// max(min(log2 f, 0) + r.ViewTextureMipBias.Offset, r.ViewTextureMipBias.Min)、それ以外は 0。
// f = EffectivePrimaryResolutionFraction。b0 MaterialTextureMipBias と自己テスト T12 が使う
float ComputeViewTextureMipBias(const FViewFamilyInfo& Family, const FAntiAliasingParams& Params);

// ---- 表示用の名前 (ImGui 統計 / テストドライバのログ / スクリーンショットのパス) ----
const char* GetAntiAliasingMethodName(EAntiAliasingMethod Method);        // "None" / "TemporalAA" ...
const char* GetTAAPassConfigName(ETAAPassConfig Pass);                     // "Main" / "MainUpsampling" / "MainSuperSampling"
const char* GetTAAQualityName(ETAAQuality Quality);                        // "Low" / "Medium" / "High" / "MediumHigh"
const char* GetPrimaryScreenPercentageMethodName(EPrimaryScreenPercentageMethod Method); // "SpatialUpscale" / "TemporalUpscale"
