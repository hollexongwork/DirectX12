#include "Main.h"
#include "ScreenPercentage.h"

#include <cmath>

// ============================================================
//  ScreenPercentage : ビューファミリ (解像度 / AA 構成) の決定
//  (UE FLegacyScreenPercentageDriver + PrepareViewRectsForRendering +
//   FDefaultTemporalUpscaler::AddPasses のパス構成選択)
// ============================================================

FViewFamilyInfo ComputeViewFamilyInfo(const FAntiAliasingParams& p, XMUINT2 O, bool bR11G11B10Supported)
{
	FViewFamilyInfo F;
	F.OutputExtent = O;

	// ---- 1. AA メソッド (未実装値の丸め: 1 FXAA / 3 MSAA -> None, 4 TSR -> TemporalAA) ----
	const int m = p.AntiAliasingMethod;
	const EAntiAliasingMethod method = (m == 2 || m == 4) ? EAntiAliasingMethod::TemporalAA : EAntiAliasingMethod::None;
	F.AntiAliasingMethod = method;
	F.bTemporalAA = (method == EAntiAliasingMethod::TemporalAA);

	// ---- 2. 一次スクリーンパーセンテージ方式 (UE: TAA でなければ Spatial へ自動フォールバック) ----
	const bool bTAAU = F.bTemporalAA && p.bTemporalAAUpsampling;
	F.PrimaryScreenPercentageMethod = bTAAU ? EPrimaryScreenPercentageMethod::TemporalUpscale
	                                        : EPrimaryScreenPercentageMethod::SpatialUpscale;

	// ---- 3. 解像度率と ViewRect (UE ApplyResolutionFraction = CeilToInt) ----
	float f = std::isfinite(p.ScreenPercentage) ? p.ScreenPercentage / 100.0f : 1.0f;
	f = bTAAU ? std::clamp(f, kMinTAAUpsampleResolutionFraction, kMaxTAAUpsampleResolutionFraction)
	          : std::clamp(f, kMinSpatialResolutionFraction,     kMaxSpatialResolutionFraction);
	F.ResolutionFraction = f;
	// 浮動小数誤差で 1 増えないよう -1e-6 (1920 * 0.5 = 960 を 961 にしない)
	F.RenderExtent.x = (std::max)(1u, (unsigned)std::ceil((double)O.x * (double)f - 1e-6));
	F.RenderExtent.y = (std::max)(1u, (unsigned)std::ceil((double)O.y * (double)f - 1e-6));
	F.EffectivePrimaryResolutionFraction = (float)F.RenderExtent.x / (float)O.x;     // UE と同じく X で定義
	const XMUINT2 R = F.RenderExtent;

	if (!F.bTemporalAA)
	{
		F.SecondaryExtent = F.HistoryExtent = F.PostProcessExtent = R;
		F.bSpatialUpscale = (R.x != O.x || R.y != O.y);
		return F;
	}

	// ---- 4. TAA パス構成 (FDefaultTemporalUpscaler::AddPasses と同じ判定) ----
	F.SecondaryExtent = bTAAU ? O : R;                        // UE: TAAParameters.OutputViewRect (SetupViewRect)
	F.TAAPass    = bTAAU ? ETAAPassConfig::MainUpsampling : ETAAPassConfig::Main;
	F.TAAQuality = (ETAAQuality)std::clamp(p.TemporalAAQuality, 0, 3);
	F.HistoryUpscaleFactor = GetTemporalAAHistoryUpscaleFactor(p);
	if (F.HistoryUpscaleFactor > 1.0f)
	{
		// UE: Pass = MainSuperSampling, bUseFast = false, OutputViewRect = SecondaryRect * Factor (float -> int32 切り捨て)
		F.TAAPass = ETAAPassConfig::MainSuperSampling;
		F.TAAQuality = ETAAQuality::High;
		F.HistoryExtent = { (unsigned)((float)F.SecondaryExtent.x * F.HistoryUpscaleFactor),
		                    (unsigned)((float)F.SecondaryExtent.y * F.HistoryUpscaleFactor) };
	}
	else
	{
		F.HistoryExtent = F.SecondaryExtent;
	}
	F.PostProcessExtent = F.SecondaryExtent;                 // SuperSampling は MN で S へ戻す

	const bool bLowOrMedium = (F.TAAQuality == ETAAQuality::Low || F.TAAQuality == ETAAQuality::Medium);
	const bool bSS = (F.TAAPass == ETAAPassConfig::MainSuperSampling);
	F.bTAADownsample    = F.TAAQuality == ETAAQuality::Low
	                   && p.bTemporalAAAllowDownsampling && !bSS;                    // UE 5.x: bAllowDownsample && Quality == Low
	F.bR11G11B10History = p.bTemporalAAR11G11B10History && bR11G11B10Supported
	                   && bLowOrMedium && !bSS;                                      // アンチゴースト (alpha) 不要の品質のみ [M]
	F.bSpatialUpscale   = (F.PostProcessExtent.x != O.x || F.PostProcessExtent.y != O.y);
	return F;
}


float GetTemporalAAHistoryUpscaleFactor(const FAntiAliasingParams& p)   // UE 同名関数 (Main にも適用 [H])
{
	const float v = std::isfinite(p.TemporalAAHistoryScreenPercentage) ? p.TemporalAAHistoryScreenPercentage : 100.0f;
	return std::clamp(v / 100.0f, 1.0f, 2.0f);
}


float ComputeViewTextureMipBias(const FViewFamilyInfo& F, const FAntiAliasingParams& p)
{
	// Automatic View Mip Bias (TemporalUpscale 時のみ; UE 4.26 は TAAU 分岐内で計算)
	float bias = 0.0f;
	if (F.PrimaryScreenPercentageMethod == EPrimaryScreenPercentageMethod::TemporalUpscale)
	{
		bias = -(std::max)(-std::log2(F.EffectivePrimaryResolutionFraction), 0.0f) + p.ViewTextureMipBiasOffset;
		bias = (std::max)(bias, p.ViewTextureMipBiasMin);
		if (!std::isfinite(bias)) bias = 0.0f;
	}
	return bias;
}


// ------------------------------------------------------------
//  表示用の名前
// ------------------------------------------------------------
const char* GetAntiAliasingMethodName(EAntiAliasingMethod Method)
{
	switch (Method)
	{
	case EAntiAliasingMethod::None:       return "None";
	case EAntiAliasingMethod::FXAA:       return "FXAA";
	case EAntiAliasingMethod::TemporalAA: return "TemporalAA";
	case EAntiAliasingMethod::MSAA:       return "MSAA";
	case EAntiAliasingMethod::TSR:        return "TSR";
	default:                              return "?";
	}
}

const char* GetTAAPassConfigName(ETAAPassConfig Pass)
{
	switch (Pass)
	{
	case ETAAPassConfig::Main:              return "Main";
	case ETAAPassConfig::MainUpsampling:    return "MainUpsampling";
	case ETAAPassConfig::MainSuperSampling: return "MainSuperSampling";
	default:                                return "?";
	}
}

const char* GetTAAQualityName(ETAAQuality Quality)
{
	switch (Quality)
	{
	case ETAAQuality::Low:        return "Low";
	case ETAAQuality::Medium:     return "Medium";
	case ETAAQuality::High:       return "High";
	case ETAAQuality::MediumHigh: return "MediumHigh";
	default:                      return "?";
	}
}

const char* GetPrimaryScreenPercentageMethodName(EPrimaryScreenPercentageMethod Method)
{
	return (Method == EPrimaryScreenPercentageMethod::TemporalUpscale) ? "TemporalUpscale" : "SpatialUpscale";
}
