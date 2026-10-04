#include "Main.h"
#include "PostProcessUpscale.h"

#include "RenderManager.h"
#include "SceneRenderer.h"
#include "SceneRenderingUtils.h"

#include <algorithm>
#include <cstdio>

// ============================================================
//  PostProcessUpscale : 一次空間アップスケール (AddUpscalePass)
// ============================================================

namespace
{
	// PSO 名 (RenderManager::InitPipelines がオプション PSO として登録する。添字 = UpscaleQuality)
	const char* const kUpscalePipelineNames[(int)EUpscaleMethod::Count] =
	{
		"PostProcessUpscale0",	// Nearest
		"PostProcessUpscale1",	// Bilinear
		"PostProcessUpscale2",	// Directional
		"PostProcessUpscale3",	// CatmullRom
		"PostProcessUpscale4",	// Lanczos
		"PostProcessUpscale5",	// Gaussian
	};

	const char* const kUpscaleMethodNames[(int)EUpscaleMethod::Count] =
	{
		"Nearest", "Bilinear", "Directional", "Catmull-Rom", "Lanczos-3", "Gaussian Unsharp",
	};

	int ClampUpscaleQuality(int Quality)
	{
		return std::clamp(Quality, 0, (int)EUpscaleMethod::Count - 1);
	}

	// FSceneRenderer::m_UpscaleFallbackLoggedMask (フォールバックのログは種類ごとに 1 回だけ):
	//   bit q (0..Count-1) : 要求品質 q の PSO が無く Bilinear (1) へ落とした
	//   bit Count          : Bilinear も無くトーンマップ統合 (bilinear) へ落とした
	constexpr unsigned int kBilinearMissingBit = 1u << (int)EUpscaleMethod::Count;
	static_assert((int)EUpscaleMethod::Count < 32, "fallback log mask overflow");

	// (In.x * In.y) / (O.x * O.y)。O が 0 の異常時は 1 (= 拡大なし扱い)
	double ComputeAreaRatio(XMUINT2 In, XMUINT2 O)
	{
		const double outArea = (double)O.x * (double)O.y;
		return (outArea > 0.0) ? ((double)In.x * (double)In.y) / outArea : 1.0;
	}
}


bool ShouldMergeTonemapWithUpscale(const FAntiAliasingParams& p, XMUINT2 In, XMUINT2 O)
{
	// TonemapperMergeWithUpscaleMode: 0 = しない, 1 = 常に, 2 = 面積比が閾値を超える時
	switch (p.TonemapperMergeWithUpscaleMode)
	{
	case 1:  return true;
	case 2:  return ComputeAreaRatio(In, O) > (double)p.TonemapperMergeWithUpscaleThreshold;
	case 0:
	default: return false;
	}
}


float ComputeUpscaleUnsharpAmount(const FAntiAliasingParams& p, XMUINT2 In, XMUINT2 O)
{
	// UpscaleSoftness x max(0, 1 - 面積比) [L]。100 % 以上 (縮小 / 等倍) では 0
	const double amount = (double)p.UpscaleSoftness * (std::max)(0.0, 1.0 - ComputeAreaRatio(In, O));
	return (float)amount;
}


const char* GetPrimaryUpscalePipelineName(int UpscaleQuality)
{
	return kUpscalePipelineNames[ClampUpscaleQuality(UpscaleQuality)];
}


const char* GetUpscaleMethodName(int UpscaleQuality)
{
	return kUpscaleMethodNames[ClampUpscaleQuality(UpscaleQuality)];
}


// ------------------------------------------------------------
//  SelectPrimaryUpscalePipeline
//  UpscaleQuality の PSO があればそれ。無ければ Bilinear (PostProcessUpscale1)、
//  それも無ければ nullptr (RenderPostProcessing はトーンマップ統合経路を取り、
//  トーンマップ PS の s1 バイリニアが拡大する)。.cso 欠落でバックバッファが
//  黒くなることは無い。フォールバックはそれぞれ 1 回だけログに出す (§3.8, §6.7)
// ------------------------------------------------------------
const char* FSceneRenderer::SelectPrimaryUpscalePipeline() const
{
	const int quality = ClampUpscaleQuality(m_AAParams.UpscaleQuality);
	const char* requested = kUpscalePipelineNames[quality];
	if (m_RHI->HasPipelineState(requested))
	{
		return requested;
	}

	const char* bilinear = kUpscalePipelineNames[(int)EUpscaleMethod::Bilinear];
	if (m_RHI->HasPipelineState(bilinear))
	{
		if ((m_UpscaleFallbackLoggedMask & (1u << quality)) == 0u)
		{
			m_UpscaleFallbackLoggedMask |= (1u << quality);
			char msg[256];
			sprintf_s(msg, "[Upscale] missing PSO %s (UpscaleQuality=%d %s) -> fallback to %s (Bilinear)\n",
				requested, quality, kUpscaleMethodNames[quality], bilinear);
			OutputDebugStringA(msg);
		}
		return bilinear;
	}

	if ((m_UpscaleFallbackLoggedMask & kBilinearMissingBit) == 0u)
	{
		m_UpscaleFallbackLoggedMask |= kBilinearMissingBit;
		char msg[256];
		sprintf_s(msg, "[Upscale] missing PSO %s and %s -> merged tonemap (bilinear) upscale\n", requested, bilinear);
		OutputDebugStringA(msg);
	}
	return nullptr;
}


// ------------------------------------------------------------
//  AddPrimaryUpscalePass (AddUpscalePass)
//  In (トーンマップ済み LDR, PSR, InExtent) -> 現在バインド中のバックバッファ RTV (O)。
//  b4: UpscaleUnsharpAmount (mode 5) / SceneTexelSize = 1/InExtent (§3.3)
// ------------------------------------------------------------
void FSceneRenderer::AddPrimaryUpscalePass(RENDER_TARGET* In, XMUINT2 InExtent, XMUINT2 OutputExtent, const char* PipelineName)
{
	if (In == nullptr || PipelineName == nullptr || !m_RHI->HasPipelineState(PipelineName))
	{
		// 呼び出し側 (RenderPostProcessing) は SelectPrimaryUpscalePipeline が nullptr なら
		// 統合経路を取るのでここへは来ない
		assert(false && "AddPrimaryUpscalePass: missing input or pipeline");
		return;
	}

	ID3D12GraphicsCommandList* cl = m_RHI->GetGraphicsCommandList();

	// (1) b4: アンシャープ量 (mode 5) + 入力テクセルサイズ
	m_FinalSettings.UpscaleUnsharpAmount = ComputeUpscaleUnsharpAmount(m_AAParams, InExtent, OutputExtent);
	SetTexelSize((int)InExtent.x, (int)InExtent.y);
	UploadPostProcessConstant();

	// (2) 出力ビューポート O へフルスクリーン描画 (exact-size なので出力 UV = 入力 UV)
	m_RHI->SetPipelineState(PipelineName);
	m_RHI->SetTexture(RenderManager::TEXTURE_TYPE::BASE_COLOR, In);	// t0
	SetViewportAndScissor(cl, (int)OutputExtent.x, (int)OutputExtent.y);
	DrawScreenPass();
}
