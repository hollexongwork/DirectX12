#pragma once
#include <DirectXMath.h>
#include "AntiAliasingSettings.h"

// ============================================================
//  PostProcessUpscale
//  UE の一次空間アップスケール (PostProcessUpscale.cpp の AddUpscalePass,
//  r.Upscale.Quality / r.Upscale.Softness) と、トーンマップとの統合判定
//  (r.Tonemapper.MergeWithUpscale.Mode / Threshold) に相当する。
//
//  ポスト解像度 P が出力解像度 O と異なる (スクリーンパーセンテージ != 100 % で
//  TAA を使わない、または TAA 出力 S != O) フレームで、トーンマップの後に
//  トーンマップ済み LDR (RGBA8, P) をバックバッファ (O) へ拡大する。
//  統合時はアップスケールパスを走らせず、トーンマップ PS がバックバッファへ
//  直接描く (t0 の UV サンプリング s1 = バイリニアが拡大を兼ねる。UE と同じく
//  統合時は r.Upscale.Quality に関わらずバイリニア)。
//
//  パス本体 FSceneRenderer::AddPrimaryUpscalePass / SelectPrimaryUpscalePipeline は
//  PostProcessUpscale.cpp に置く (FSceneRenderer のメンバ)。
// ============================================================

// r.Upscale.Quality (PSO 名 "PostProcessUpscale<n>" の添字)
enum class EUpscaleMethod : int
{
	Nearest = 0,		// 最近傍
	Bilinear = 1,		// バイリニア (フォールバック先)
	Directional = 2,	// 方向性ブラー + アンシャープマスク [M]
	CatmullRom = 3,		// 5 タップ Catmull-Rom (既定)
	Lanczos = 4,		// Lanczos-3 菱形 13 タップ [M]
	Gaussian = 5,		// ガウシアン + ラプラシアンのアンシャープ (r.Upscale.Softness) [L]
	Count
};

// UE r.Tonemapper.MergeWithUpscale: Mode 0 -> false, 1 -> true,
// 2 -> (In.x * In.y) / (O.x * O.y) > Threshold (面積比)
bool ShouldMergeTonemapWithUpscale(const FAntiAliasingParams& Params, DirectX::XMUINT2 InputExtent, DirectX::XMUINT2 OutputExtent);

// b4 UpscaleUnsharpAmount (mode 5) = r.Upscale.Softness x max(0, 1 - (In.x * In.y) / (O.x * O.y)) [L]
float ComputeUpscaleUnsharpAmount(const FAntiAliasingParams& Params, DirectX::XMUINT2 InputExtent, DirectX::XMUINT2 OutputExtent);

// r.Upscale.Quality (0..5) -> PSO 名 "PostProcessUpscale<n>" (範囲外はクランプ)
const char* GetPrimaryUpscalePipelineName(int UpscaleQuality);

// 表示用 ("Nearest" / "Bilinear" / ...)
const char* GetUpscaleMethodName(int UpscaleQuality);
