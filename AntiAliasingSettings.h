#pragma once
#include <algorithm>

// ============================================================
//  AntiAliasingSettings
//  アンチエイリアシング / スクリーンパーセンテージ (TAAU) の設定値。
//  UE の CVar (r.AntiAliasingMethod / r.ScreenPercentage /
//  r.TemporalAA.* / r.Upscale.* / r.ViewTextureMipBias.*) に対応する。
//
//    FAntiAliasingParams      : 永続化対象 (SettingsManager が INI の
//                               [AntiAliasing] セクションへ保存。キー = フィールド名)
//    FTemporalAADebugSettings : 非永続のデバッグ設定 (Lumen の DebugMode と同じ扱い。
//                               毎回 Off で起動)
//
//  依存ヘッダを持たないので SettingsManager.h からも取り込める。
//  既定値 = 最終値。
// ============================================================

enum class EAntiAliasingMethod : int { None = 0, FXAA = 1, TemporalAA = 2, MSAA = 3, TSR = 4 }; // UE と同値 (1/3/4 は未実装)
enum class EPrimaryScreenPercentageMethod : int { SpatialUpscale = 0, TemporalUpscale = 1 };

enum class ETemporalAADebugView : int
{
	Off = 0,
	MotionVectors = 1,          // PS (VisualizeMotionVectors)
	VelocityMask = 2,           // PS
	InputOutputSplit = 3,       // 2 回目のトーンマップ (左半分シザー)
	TemporalUpscalerIO = 4,     // PS 2x2 (VisualizeTemporalUpscaler)
	BlendFinal = 5,             // 5..13 は TAA CS の DebugOutput (AA_DEBUG 相当)
	Rejection = 6,
	HistoryClamp = 7,
	ReprojectionError = 8,
	FilteredTemporalWeight = 9,
	ClosestDepthOffset = 10,
	ResponsiveMask = 11,
	DynamicAntiGhost = 12,
	InputSampleAlignment = 13,
	Count
};

inline bool IsTemporalAADebugViewFromCS(ETemporalAADebugView V)
{
	return V >= ETemporalAADebugView::BlendFinal && V < ETemporalAADebugView::Count;
}

// ------------------------------------------------------------
//  永続化される設定 (INI [AntiAliasing]。キー = フィールド名)。
//  各フィールドの範囲は下の AntiAliasingRanges (= ImGui スライダーの範囲)。
// ------------------------------------------------------------
struct FAntiAliasingParams
{
	int   AntiAliasingMethod = 2;                   // r.AntiAliasingMethod
	float ScreenPercentage = 100.0f;                // r.ScreenPercentage
	bool  bTemporalAAUpsampling = true;             // r.TemporalAA.Upsampling  [PORT: UE4 既定 0]
	int   TemporalAAQuality = 2;                    // r.TemporalAA.Quality
	int   TemporalAASamples = 8;                    // r.TemporalAASamples
	float TemporalAACurrentFrameWeight = 0.04f;     // r.TemporalAACurrentFrameWeight
	float TemporalAAFilterSize = 1.0f;              // r.TemporalAAFilterSize
	bool  bTemporalAACatmullRom = false;            // r.TemporalAACatmullRom
	bool  bTemporalAAUpsampleFiltered = true;       // r.TemporalAAUpsampleFiltered
	float TemporalAAHistoryScreenPercentage = 100.0f; // r.TemporalAA.HistoryScreenPercentage
	bool  bTemporalAAR11G11B10History = true;       // r.TemporalAA.R11G11B10History
	bool  bTemporalAAAllowDownsampling = true;      // r.TemporalAA.AllowDownsampling
	int   UpscaleQuality = 3;                       // r.Upscale.Quality
	float UpscaleSoftness = 1.0f;                   // r.Upscale.Softness
	int   TonemapperMergeWithUpscaleMode = 0;       // r.Tonemapper.MergeWithUpscale.Mode
	float TonemapperMergeWithUpscaleThreshold = 0.49f; // r.Tonemapper.MergeWithUpscale.Threshold
	float ViewTextureMipBiasOffset = -0.3f;         // r.ViewTextureMipBias.Offset
	float ViewTextureMipBiasMin = -2.0f;            // r.ViewTextureMipBias.Min
	float CameraRotationThreshold = 45.0f;          // GEngine->CameraRotationThreshold [度]
	float CameraTranslationThreshold = 100.0f;      // GEngine->CameraTranslationThreshold (10000cm) [m]
};

// ------------------------------------------------------------
//  非永続のデバッグ設定 (ImGui "Debug (not saved)" / テストドライバ)
// ------------------------------------------------------------
struct FTemporalAADebugSettings
{
	ETemporalAADebugView DebugView = ETemporalAADebugView::Off;
	float VisualizeScale = 1.0f;                    // [0.1, 64] 可視化の増幅
	int   OverrideTemporalIndex = -1;               // r.TemporalAA.Debug.OverrideTemporalIndex (>=0 で固定)
	int   FilteredTemporalWeightMode = 0;           // 0 = 空間重み総和, 1 = 最近傍重み, 2 = 1.0
	bool  bForceJitterWithoutTAA = false;           // TAA 無効時のジッタ比較用
	bool  bDisableJitter = false;                   // ジッタ 0 (比較用)
	bool  bForceVelocityPass = false;               // TAA 無効でもベロシティを描く
	bool  bForceResponsiveAA = false;               // 全半透明を Responsive 扱い
	bool  bDisableVelocitySmallObjectCull = false;  // MotionBlurPerObjectSize カリング無効
	bool  bRequestHistoryReset = false;             // ワンショット (カメラカット扱い)
	bool  bRequestSelfTest = false;                 // ワンショット (次の BeginFrame 先頭で実行)
	bool  bRequestReallocate = false;               // ワンショット: 同一サイズでも ResizeRenderTargets を実行 (同一サイズ再確保 / リーク検査用)
};

// ------------------------------------------------------------
//  FAntiAliasingParams の各フィールドの範囲 (§7.1)。
//  INI 読み込み / テストドライバの上書き / ImGui スライダーで共通に使う
//  (スライダーは ImGuiSliderFlags_AlwaysClamp でこの範囲に収める)。
// ------------------------------------------------------------
namespace AntiAliasingRanges
{
	constexpr float kScreenPercentageMin = 10.0f,  kScreenPercentageMax = 200.0f;   // [PORT] 空間アップスケールの範囲 (TAAU は 50-200 に再クランプ)
	constexpr int   kQualityMin = 0,               kQualityMax = 3;
	constexpr int   kSamplesMin = 1,               kSamplesMax = 64;
	constexpr float kCurrentFrameWeightMin = 0.0f, kCurrentFrameWeightMax = 1.0f;
	constexpr float kFilterSizeMin = 0.1f,         kFilterSizeMax = 2.0f;
	constexpr float kHistoryScreenPercentageMin = 100.0f, kHistoryScreenPercentageMax = 200.0f;
	constexpr int   kUpscaleQualityMin = 0,        kUpscaleQualityMax = 5;
	constexpr float kUpscaleSoftnessMin = 0.0f,    kUpscaleSoftnessMax = 1.0f;
	constexpr int   kMergeModeMin = 0,             kMergeModeMax = 2;
	constexpr float kMergeThresholdMin = 0.0f,     kMergeThresholdMax = 1.0f;
	constexpr float kMipBiasOffsetMin = -2.0f,     kMipBiasOffsetMax = 1.0f;
	constexpr float kMipBiasMinMin = -4.0f,        kMipBiasMinMax = 0.0f;
	constexpr float kCameraRotationThresholdMin = 0.0f,    kCameraRotationThresholdMax = 180.0f;     // [度]
	constexpr float kCameraTranslationThresholdMin = 0.0f, kCameraTranslationThresholdMax = 10000.0f; // [m]
	// FTemporalAADebugSettings (非永続。ImGui / テストドライバ -visscale / -ftwmode)
	constexpr float kVisualizeScaleMin = 0.1f,     kVisualizeScaleMax = 64.0f;
	constexpr int   kFilteredTemporalWeightModeMin = 0, kFilteredTemporalWeightModeMax = 2;   // Sum / Nearest / One
}

// r.AntiAliasingMethod を実装済みの値へ丸める (INI 読み込み / テストドライバ -aa):
// 1 (FXAA) / 3 (MSAA) -> 0 (None), 4 (TSR) -> 2 (TemporalAA), {0, 2} 以外 -> 2
inline int SanitizeAntiAliasingMethod(int Method)
{
	switch (Method)
	{
	case 0: return 0;
	case 1: return 0;
	case 2: return 2;
	case 3: return 0;
	case 4: return 2;
	default: return 2;
	}
}

// 全フィールドを §7.1 の範囲へ収める。float の NaN は既定値へ戻す
// (NaN は比較が常に偽で std::clamp を素通りするため。EditorViewport と同じ形)
inline void SanitizeAntiAliasingParams(FAntiAliasingParams& p)
{
	using namespace AntiAliasingRanges;
	const FAntiAliasingParams def{};
	auto clampFloat = [](float& v, float lo, float hi, float fallback)
		{
			v = (v == v) ? std::clamp(v, lo, hi) : fallback;
		};

	p.AntiAliasingMethod = SanitizeAntiAliasingMethod(p.AntiAliasingMethod);
	clampFloat(p.ScreenPercentage, kScreenPercentageMin, kScreenPercentageMax, def.ScreenPercentage);
	p.TemporalAAQuality = std::clamp(p.TemporalAAQuality, kQualityMin, kQualityMax);
	p.TemporalAASamples = std::clamp(p.TemporalAASamples, kSamplesMin, kSamplesMax);
	clampFloat(p.TemporalAACurrentFrameWeight, kCurrentFrameWeightMin, kCurrentFrameWeightMax, def.TemporalAACurrentFrameWeight);
	clampFloat(p.TemporalAAFilterSize, kFilterSizeMin, kFilterSizeMax, def.TemporalAAFilterSize);
	clampFloat(p.TemporalAAHistoryScreenPercentage, kHistoryScreenPercentageMin, kHistoryScreenPercentageMax, def.TemporalAAHistoryScreenPercentage);
	p.UpscaleQuality = std::clamp(p.UpscaleQuality, kUpscaleQualityMin, kUpscaleQualityMax);
	clampFloat(p.UpscaleSoftness, kUpscaleSoftnessMin, kUpscaleSoftnessMax, def.UpscaleSoftness);
	p.TonemapperMergeWithUpscaleMode = std::clamp(p.TonemapperMergeWithUpscaleMode, kMergeModeMin, kMergeModeMax);
	clampFloat(p.TonemapperMergeWithUpscaleThreshold, kMergeThresholdMin, kMergeThresholdMax, def.TonemapperMergeWithUpscaleThreshold);
	clampFloat(p.ViewTextureMipBiasOffset, kMipBiasOffsetMin, kMipBiasOffsetMax, def.ViewTextureMipBiasOffset);
	clampFloat(p.ViewTextureMipBiasMin, kMipBiasMinMin, kMipBiasMinMax, def.ViewTextureMipBiasMin);
	clampFloat(p.CameraRotationThreshold, kCameraRotationThresholdMin, kCameraRotationThresholdMax, def.CameraRotationThreshold);
	clampFloat(p.CameraTranslationThreshold, kCameraTranslationThresholdMin, kCameraTranslationThresholdMax, def.CameraTranslationThreshold);
}
