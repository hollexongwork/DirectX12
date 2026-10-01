#include "Main.h"
#include "TemporalAA.h"

#include "D3DX12.h"
#include "SceneRenderingUtils.h"

#include <cmath>
#include <cstring>
#include <cstdio>

// ============================================================
//  TemporalAA : FDefaultTemporalUpscaler (UE Gen4 TAA / TAAU)
// ============================================================

namespace
{
	// ------------------------------------------------------------
	//  FBarrierBatch: 1 回の ResourceBarrier にまとめる小さなバッチ (最大 8)。
	//  before == after は除去し、同じリソースの 2 回目は完全重複なら除去、
	//  連鎖 (A0->A1 の後に A1->A2) なら A0->A2 に併合する (デデュープ)。
	//  Add は Flush 後にリソースが After にあるかを返し、Track はその時だけ
	//  FTAATexture::State を遷移先で更新する (実際に発行される状態だけを記録する)
	// ------------------------------------------------------------
	class FBarrierBatch
	{
	public:
		bool Add(ID3D12Resource* Resource, D3D12_RESOURCE_STATES Before, D3D12_RESOURCE_STATES After)
		{
			if (Resource == nullptr || Before == After)
				return true;                                        // 何もしない (既に After)
			for (int i = 0; i < m_Count; ++i)
			{
				D3D12_RESOURCE_TRANSITION_BARRIER& t = m_Barriers[i].Transition;
				if (t.pResource != Resource)
					continue;
				if (t.StateAfter == After)
					return true;                                    // 完全重複
				if (t.StateAfter == Before && t.StateBefore != After)
				{
					t.StateAfter = After;                           // 連鎖 A0->A1->A2 を A0->A2 に併合
					return true;
				}
				assert(!"FBarrierBatch: conflicting duplicate transition");
				return false;
			}
			if (m_Count >= kMaxBarriers)
			{
				assert(!"FBarrierBatch: batch full");
				return false;
			}
			m_Barriers[m_Count++] = CD3DX12_RESOURCE_BARRIER::Transition(Resource, Before, After);
			return true;
		}
		void Track(FTAATexture& Texture, D3D12_RESOURCE_STATES After)
		{
			if (!Texture.RT)
				return;
			if (Add(Texture.RT->Resource.Get(), Texture.State, After))
				Texture.State = After;                              // 実際に発行される遷移だけを記録する
		}
		void Flush(ID3D12GraphicsCommandList* CommandList)
		{
			if (m_Count > 0)
				CommandList->ResourceBarrier((UINT)m_Count, m_Barriers);
			m_Count = 0;
		}
	private:
		static constexpr int kMaxBarriers = 8;
		D3D12_RESOURCE_BARRIER m_Barriers[kMaxBarriers]{};
		int m_Count = 0;
	};

	constexpr D3D12_RESOURCE_STATES kReadState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

	// ------------------------------------------------------------
	//  Main 構成のサンプル重み (§4.8.6)
	// ------------------------------------------------------------
	constexpr float kMinWeightSum      = 1.0e-6f;
	constexpr float kMinCRConditioning = 0.25f;   // Σw >= 0.25·Σ|w| (負ローブの相殺で正規化重みが発散するのを防ぐ)

	float CatmullRom(float x)                       // [PORT] |x| >= 2 は 0 (UE は台の外でも 3 次式を評価する潜在不具合)
	{
		const float ax = std::fabs(x);
		if (ax >= 2.0f) return 0.0f;
		if (ax > 1.0f)  return ((-0.5f * ax + 2.5f) * ax - 4.0f) * ax + 2.0f;
		return (1.5f * ax - 2.5f) * ax * ax + 1.0f;
	}

	// 正規化。総和が極小 / 負、または相殺で悪条件なら「J に最も近いサンプル = 1」(同距離は中心) へフォールバック
	void NormalizeOrNearest(const float* W, const float* D2, const int* Idx, int Count, int CentreSlot, float* Out)
	{
		float Sum = 0.0f, AbsSum = 0.0f;
		int Nearest = CentreSlot;
		for (int k = 0; k < Count; ++k)
		{
			Sum += W[Idx[k]];
			AbsSum += std::fabs(W[Idx[k]]);
			if (D2[Idx[k]] < D2[Idx[Nearest]]) Nearest = k;
		}
		const bool bIllConditioned = !(Sum > kMinWeightSum) || Sum < kMinCRConditioning * AbsSum;
		for (int k = 0; k < Count; ++k)
			Out[k] = bIllConditioned ? (k == Nearest ? 1.0f : 0.0f) : W[Idx[k]] / Sum;
	}

	// 1 デスクリプタのテーブル範囲 (SRV / UAV)
	D3D12_DESCRIPTOR_RANGE MakeRange(D3D12_DESCRIPTOR_RANGE_TYPE Type, UINT Register)
	{
		D3D12_DESCRIPTOR_RANGE r{};
		r.RangeType = Type;
		r.NumDescriptors = 1;
		r.BaseShaderRegister = Register;
		r.RegisterSpace = 0;
		r.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
		return r;
	}

	D3D12_STATIC_SAMPLER_DESC MakeClampSampler(D3D12_FILTER Filter, UINT Register)
	{
		D3D12_STATIC_SAMPLER_DESC s{};
		s.Filter = Filter;
		s.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
		s.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
		s.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
		s.MipLODBias = 0.0f;
		s.MaxAnisotropy = 1;
		s.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
		s.BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK;
		s.MinLOD = 0.0f;
		s.MaxLOD = D3D12_FLOAT32_MAX;
		s.ShaderRegister = Register;
		s.RegisterSpace = 0;
		s.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
		return s;
	}

	// ルートパラメータの添字 (§3.7)
	constexpr UINT kRootCBV = 0;          // b0
	constexpr UINT kRootSRVBase = 1;      // t0..t5 -> 1..6
	constexpr UINT kRootUAVBase = 7;      // u0..u2 -> 7..9
	constexpr UINT kNumSRVTables = 6;
	constexpr UINT kNumUAVTables = 3;

	// TAA / MN のスレッドグループ (== TemporalAA.hlsl / TemporalAAMitchellNetravali_CS.hlsl の TAA_TILE_SIZE, UE GTemporalAATileSizeX/Y)
	constexpr UINT kTAATileSize = 8;

	// 順列ラッパの .cso (§4.8.1 の表)。Pass / Quality / Downsample
	struct FPermutationFile { int Pass; int Quality; int Downsample; const char* File; };
	const FPermutationFile kPermutationFiles[] =
	{
		// ---- Main ----
		{ 0, 0, 0, "Shader/cso/TemporalAA_Main_Low_CS.cso" },
		{ 0, 1, 0, "Shader/cso/TemporalAA_Main_Medium_CS.cso" },
		{ 0, 2, 0, "Shader/cso/TemporalAA_Main_High_CS.cso" },
		{ 0, 3, 0, "Shader/cso/TemporalAA_Main_MediumHigh_CS.cso" },
		// ---- MainUpsampling ----
		{ 1, 0, 0, "Shader/cso/TemporalAA_Upsampling_Low_CS.cso" },
		{ 1, 1, 0, "Shader/cso/TemporalAA_Upsampling_Medium_CS.cso" },
		{ 1, 2, 0, "Shader/cso/TemporalAA_Upsampling_High_CS.cso" },
		{ 1, 3, 0, "Shader/cso/TemporalAA_Upsampling_MediumHigh_CS.cso" },
		// ---- MainSuperSampling (品質は High 強制) ----
		{ 2, 2, 0, "Shader/cso/TemporalAA_SuperSampling_CS.cso" },
		// ---- ハーフ解像度出力 (UE 5.x: bAllowDownsample && Quality == Low。SuperSampling は無し) ----
		{ 0, 0, 1, "Shader/cso/TemporalAA_Main_Low_Downsample_CS.cso" },
		{ 1, 0, 1, "Shader/cso/TemporalAA_Upsampling_Low_Downsample_CS.cso" },
	};
}


// ============================================================
//  CPU 計算
// ============================================================
void ComputeTemporalAASampleWeights(XMFLOAT2 J, float FilterSize, bool bCatmullRom, float OutSampleWeights[9], float OutPlusWeights[5])
{
	static const int kOff[9][2] = { {-1,-1},{0,-1},{1,-1},{-1,0},{0,0},{1,0},{-1,1},{0,1},{1,1} };
	static const int kAll[9] = { 0,1,2,3,4,5,6,7,8 };
	static const int kPlus[5] = { 1, 3, 4, 5, 7 };
	const float FS = (std::max)(FilterSize, 0.01f);
	float W[9], D2[9];
	for (int i = 0; i < 9; ++i)
	{
		// UE: SampleOffsets - Jitter (出力画素中心に属する点はジッタ込みで c + J に写る。A.2)
		const float dx = ((float)kOff[i][0] - J.x) / FS, dy = ((float)kOff[i][1] - J.y) / FS;
		D2[i] = dx * dx + dy * dy;
		W[i] = bCatmullRom ? CatmullRom(dx) * CatmullRom(dy) : std::exp(-2.29f * D2[i]);   // ガウス: Sigma = 0.47
	}
	NormalizeOrNearest(W, D2, kAll, 9, 4, OutSampleWeights);   // 中心 = 添字 4
	NormalizeOrNearest(W, D2, kPlus, 5, 2, OutPlusWeights);    // 中心 = プラス内の添字 2
}


XMFLOAT3 ComputePixelFormatQuantizationError(DXGI_FORMAT Format)
{
	// 仮数部 10 bit (half) / 6, 6, 5 bit (R11G11B10)。UE ComputePixelFormatQuantizationError
	switch (Format)
	{
	case DXGI_FORMAT_R11G11B10_FLOAT:
		return { std::ldexp(1.0f, -6), std::ldexp(1.0f, -6), std::ldexp(1.0f, -5) };
	case DXGI_FORMAT_R16G16B16A16_FLOAT:
	default:
		return { std::ldexp(1.0f, -10), std::ldexp(1.0f, -10), std::ldexp(1.0f, -10) };
	}
}


XMFLOAT3 RGBToYCoCgCPU(XMFLOAT3 c)
{
	return { c.x + 2.0f * c.y + c.z, 2.0f * c.x - 2.0f * c.z, -c.x + 2.0f * c.y - c.z };
}

XMFLOAT3 YCoCgToRGBCPU(XMFLOAT3 c)
{
	const float Y = c.x * 0.25f, Co = c.y * 0.25f, Cg = c.z * 0.25f;
	return { Y + Co - Cg, Y + Cg, Y - Co - Cg };
}

float HdrWeightYCPU(float Y, float Exposure)
{
	return 1.0f / (Y * Exposure + 4.0f);
}

XMFLOAT2 WeightedLerpFactorsCPU(float WeightA, float WeightB, float Blend)
{
	const float A = (1.0f - Blend) * WeightA, B = Blend * WeightB;
	const float R = 1.0f / (A + B);
	return { A * R, B * R };
}


// ---- TAAU / 再サンプルカーネルの CPU 鏡像 (T8-T11) ----
float ComputeSampleWeigthCPU(XMFLOAT2 PixelDelta, float UpscaleFactor)
{
	const float x2 = std::clamp(UpscaleFactor * UpscaleFactor * (PixelDelta.x * PixelDelta.x + PixelDelta.y * PixelDelta.y), 0.0f, 1.0f);
	return (0.905f * x2 - 1.9f) * x2 + 1.0f;
}

void ComputeTAAUInputMappingCPU(XMUINT2 OutputPixel, XMUINT2 OutputExtent, XMUINT2 InputExtent, XMFLOAT2 JitterPixels,
	XMINT2& OutK, XMFLOAT2& OutDKO)
{
	// HLSL: ViewportUV = (p + 0.5) / H, PPCo = ViewportUV * R + J, PPCk = floor(PPCo) + 0.5, dKO = PPCo - PPCk,
	//       K = ClampInputPixel(floor(PPCo))。参照値なので double (float の丸めは GPU 側の 1 ULP 差として許容)
	const double ppcX = ((double)OutputPixel.x + 0.5) / (double)OutputExtent.x * (double)InputExtent.x + (double)JitterPixels.x;
	const double ppcY = ((double)OutputPixel.y + 0.5) / (double)OutputExtent.y * (double)InputExtent.y + (double)JitterPixels.y;
	const double kx = std::floor(ppcX), ky = std::floor(ppcY);
	OutDKO = { (float)(ppcX - (kx + 0.5)), (float)(ppcY - (ky + 0.5)) };
	OutK = { std::clamp((int)kx, 0, (int)InputExtent.x - 1), std::clamp((int)ky, 0, (int)InputExtent.y - 1) };
}

void CatmullRomWeights1DCPU(float F, float OutWeights[4])
{
	// CatmullRom5Taps (TemporalAACommon.hlsl) の 1 軸分: w0 / w1 / w2 / w3 (タップ -1, 0, +1, +2)
	const float f2 = F * F, f3 = f2 * F;
	const float w0 = f2 - 0.5f * (f3 + F);
	const float w1 = 1.5f * f3 - 2.5f * f2 + 1.0f;
	const float w3 = 0.5f * (f3 - f2);
	const float w2 = 1.0f - w0 - w1 - w3;
	OutWeights[0] = w0;
	OutWeights[1] = w1;
	OutWeights[2] = w2;
	OutWeights[3] = w3;
}

float MitchellNetravaliCPU(float X)
{
	const float x = std::fabs(X);
	if (x < 1.0f) return (7.0f * x * x * x - 12.0f * x * x + 16.0f / 3.0f) / 6.0f;
	if (x < 2.0f) return (-7.0f / 3.0f * x * x * x + 12.0f * x * x - 20.0f * x + 32.0f / 3.0f) / 6.0f;
	return 0.0f;
}

void ComputeMitchellNetravaliTapsCPU(float OutputPixel, float InputPerOutputPixel, int& OutFirst, float OutWeights[9], float& OutRawSum)
{
	// TemporalAAMitchellNetravali_CS の 1 軸分 (同じ式・同じ順序)
	const float inPos = (OutputPixel + 0.5f) * InputPerOutputPixel;
	const float radius = 2.0f * InputPerOutputPixel;
	OutFirst = (int)std::floor(inPos - radius - 0.5f) + 1;
	float sum = 0.0f;
	for (int k = 0; k < 9; ++k)
	{
		OutWeights[k] = MitchellNetravaliCPU(((float)(OutFirst + k) + 0.5f - inPos) / InputPerOutputPixel);
		sum += OutWeights[k];
	}
	OutRawSum = sum;
	for (int k = 0; k < 9; ++k)
		OutWeights[k] = (sum != 0.0f) ? OutWeights[k] / sum : 0.0f;
}


// ============================================================
//  Lifetime / Init (§4.8.1)
// ============================================================
FDefaultTemporalUpscaler::FDefaultTemporalUpscaler(RenderManager* RHI)
	: m_RHI(RHI)
{
}

FDefaultTemporalUpscaler::~FDefaultTemporalUpscaler()
{
	for (int i = 0; i < 2; ++i)
	{
		if (m_ParamBuffer[i] && m_ParamPtr[i])
			m_ParamBuffer[i]->Unmap(0, nullptr);
	}
	if (m_DummyEyeAdaptationSRV) { m_RHI->ReleaseShaderResourceView(m_DummyEyeAdaptationSRV); }
	if (m_SelfTestUAV) { m_RHI->ReleaseShaderResourceView(m_SelfTestUAV); }
}


void FDefaultTemporalUpscaler::Init()
{
	ID3D12Device* device = m_RHI->GetDevice();
	ID3D12GraphicsCommandList* cl = m_RHI->GetGraphicsCommandList();

	// ---- (1) コンピュートルートシグネチャ (§3.7, version 1.0, flags NONE) ----
	{
		D3D12_DESCRIPTOR_RANGE ranges[kNumSRVTables + kNumUAVTables];
		D3D12_ROOT_PARAMETER params[1 + kNumSRVTables + kNumUAVTables]{};

		params[kRootCBV].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
		params[kRootCBV].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
		params[kRootCBV].Descriptor.ShaderRegister = 0;
		params[kRootCBV].Descriptor.RegisterSpace = 0;

		for (UINT i = 0; i < kNumSRVTables; ++i)
		{
			ranges[i] = MakeRange(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, i);
			D3D12_ROOT_PARAMETER& p = params[kRootSRVBase + i];
			p.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
			p.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
			p.DescriptorTable.NumDescriptorRanges = 1;
			p.DescriptorTable.pDescriptorRanges = &ranges[i];
		}
		for (UINT i = 0; i < kNumUAVTables; ++i)
		{
			ranges[kNumSRVTables + i] = MakeRange(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, i);
			D3D12_ROOT_PARAMETER& p = params[kRootUAVBase + i];
			p.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
			p.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
			p.DescriptorTable.NumDescriptorRanges = 1;
			p.DescriptorTable.pDescriptorRanges = &ranges[kNumSRVTables + i];
		}

		// s0 ポイントクランプ / s1 リニアクランプ (MipLODBias 0)
		const D3D12_STATIC_SAMPLER_DESC samplers[2] =
		{
			MakeClampSampler(D3D12_FILTER_MIN_MAG_MIP_POINT, 0),
			MakeClampSampler(D3D12_FILTER_MIN_MAG_MIP_LINEAR, 1),
		};

		D3D12_ROOT_SIGNATURE_DESC desc{};
		desc.NumParameters = _countof(params);
		desc.pParameters = params;
		desc.NumStaticSamplers = _countof(samplers);
		desc.pStaticSamplers = samplers;
		desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

		ComPtr<ID3DBlob> blob, errorBlob;
		HRESULT hr = D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errorBlob);
		if (FAILED(hr) && errorBlob)
		{
			OutputDebugStringA((const char*)errorBlob->GetBufferPointer());
		}
		assert(SUCCEEDED(hr));
		hr = device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&m_RootSignature));
		assert(SUCCEEDED(hr));
		m_RootSignature->SetName(L"TemporalAARootSignature");
	}

	// ---- (2) PSO (欠落はログのみで null。IsReady が false を返し AA 無しへフォールバック) ----
	for (const FPermutationFile& f : kPermutationFiles)
	{
		m_PSO[f.Pass][f.Quality][f.Downsample] = TryCreateComputePipeline(f.File);
	}
	// MainSuperSampling の H -> S ダウンサンプル。欠落なら SuperSampling 構成は IsReady = false
	m_PSOMitchellNetravali = TryCreateComputePipeline("Shader/cso/TemporalAAMitchellNetravali_CS.cso");
	m_PSOSelfTest = TryCreateComputePipeline("Shader/cso/TemporalAASelfTest_CS.cso");

	// ---- (3) 定数リング (UPLOAD, 永続 Map): フレームごと 4 スロット x 512 B ----
	for (int i = 0; i < 2; ++i)
	{
		const CD3DX12_HEAP_PROPERTIES heap(D3D12_HEAP_TYPE_UPLOAD);
		const CD3DX12_RESOURCE_DESC desc = CD3DX12_RESOURCE_DESC::Buffer(kParamSlotSize * kParamSlotCount);
		HRESULT hr = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
			D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_ParamBuffer[i]));
		assert(SUCCEEDED(hr));
		m_ParamBuffer[i]->SetName(L"TemporalAAParams");
		const D3D12_RANGE noRead{ 0, 0 };
		hr = m_ParamBuffer[i]->Map(0, &noRead, (void**)&m_ParamPtr[i]);
		assert(SUCCEEDED(hr));
		std::memset(m_ParamPtr[i], 0, kParamSlotSize * kParamSlotCount);
	}

	// ---- (4) ダミー ----
	// 1x1 RGBA16F を (0,0,0,1) = CreateRenderTarget の最適化クリア値でクリアし (#820 無し)、RD 常駐。
	// RG = 0 は「ベロシティ未書き込み」、R = 0 は「Responsive でない」。A = 1 は読まれない
	// (履歴として束縛されるのは bCameraCut = 1 の時だけで、その時 HISTORY_HAS_ALPHA はクリア)
	m_DummyTex = m_RHI->CreateRenderTarget(1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT);
	m_DummyTex->Resource->SetName(L"TemporalAADummy");
	{
		TransitionToRenderTarget(cl, m_DummyTex.get());						// PSR -> RT
		const FLOAT clear[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
		cl->ClearRenderTargetView(m_DummyTex->RTVHandle, clear, 0, nullptr);
		TransitionRenderTargetToRead(cl, m_DummyTex.get());					// RT -> RD
	}

	// 未使用 UAV (u1 / u2) 用。UNORDERED_ACCESS に常駐 (SRV ダミーとは別リソース = 状態の衝突無し)
	m_DummyUAVTex = m_RHI->CreateRenderTarget(1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT, 1, true);
	m_DummyUAVTex->Resource->SetName(L"TemporalAADummyUAV");
	cl->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(m_DummyUAVTex->Resource.Get(),
		D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS));

	// 露出ダミー: UPLOAD float[2] = {1, 1} + 型付き SRV (R32_FLOAT, 2 要素)。AutoExposure 結果が
	// 未初期化 (初回 Dispatch 前) の時の t4 (HLSL 側は TAA_FLAG_EYE_ADAPTATION_BUFFER が立たないので読まない)
	{
		const CD3DX12_HEAP_PROPERTIES heap(D3D12_HEAP_TYPE_UPLOAD);
		const CD3DX12_RESOURCE_DESC desc = CD3DX12_RESOURCE_DESC::Buffer(2 * sizeof(float));
		HRESULT hr = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
			D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_DummyEyeAdaptation));
		assert(SUCCEEDED(hr));
		m_DummyEyeAdaptation->SetName(L"TemporalAADummyEyeAdaptation");
		float* ptr = nullptr;
		const D3D12_RANGE noRead{ 0, 0 };
		hr = m_DummyEyeAdaptation->Map(0, &noRead, (void**)&ptr);
		assert(SUCCEEDED(hr));
		ptr[0] = 1.0f;
		ptr[1] = 1.0f;
		m_DummyEyeAdaptation->Unmap(0, nullptr);

		m_DummyEyeAdaptationSRV = m_RHI->AllocateDescriptor();
		D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
		srv.Format = DXGI_FORMAT_R32_FLOAT;
		srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
		srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		srv.Buffer.FirstElement = 0;
		srv.Buffer.NumElements = 2;
		srv.Buffer.StructureByteStride = 0;
		srv.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
		device->CreateShaderResourceView(m_DummyEyeAdaptation.Get(), &srv, m_RHI->GetCPUDescriptorHandle(m_DummyEyeAdaptationSRV));
	}

	// ---- (5) 自己テスト: 64 float の DEFAULT バッファ (構造化 UAV, stride 4) + READBACK ----
	// DEFAULT バッファは COMMON で作る (バッファは他の初期状態を無視する #1328)。
	// ExecuteCommandLists の完了ごとに COMMON へ減衰するので、RunGPUSelfTest が毎回
	// COMMON -> UAV -> COPY_SOURCE と明示遷移する
	{
		const CD3DX12_HEAP_PROPERTIES heap(D3D12_HEAP_TYPE_DEFAULT);
		const CD3DX12_RESOURCE_DESC desc = CD3DX12_RESOURCE_DESC::Buffer(kSelfTestValueCount * sizeof(float), D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
		HRESULT hr = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
			D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&m_SelfTestBuffer));
		assert(SUCCEEDED(hr));
		m_SelfTestBuffer->SetName(L"TemporalAASelfTest");

		const CD3DX12_HEAP_PROPERTIES rbHeap(D3D12_HEAP_TYPE_READBACK);
		const CD3DX12_RESOURCE_DESC rbDesc = CD3DX12_RESOURCE_DESC::Buffer(kSelfTestValueCount * sizeof(float));
		hr = device->CreateCommittedResource(&rbHeap, D3D12_HEAP_FLAG_NONE, &rbDesc,
			D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&m_SelfTestReadback));
		assert(SUCCEEDED(hr));
		m_SelfTestReadback->SetName(L"TemporalAASelfTestReadback");

		m_SelfTestUAV = m_RHI->AllocateDescriptor();
		D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
		uav.Format = DXGI_FORMAT_UNKNOWN;
		uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
		uav.Buffer.FirstElement = 0;
		uav.Buffer.NumElements = kSelfTestValueCount;
		uav.Buffer.StructureByteStride = sizeof(float);
		uav.Buffer.CounterOffsetInBytes = 0;
		uav.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_NONE;
		device->CreateUnorderedAccessView(m_SelfTestBuffer.Get(), nullptr, &uav, m_RHI->GetCPUDescriptorHandle(m_SelfTestUAV));
	}

	// ---- (6) R11G11B10 の型付き UAV ストア対応 (FL11 では必須だが確認は安価) ----
	{
		D3D12_FEATURE_DATA_FORMAT_SUPPORT fs{ DXGI_FORMAT_R11G11B10_FLOAT };
		m_bR11G11B10Supported = SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &fs, sizeof(fs)))
			&& (fs.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE) != 0;
	}
}


ComPtr<ID3D12PipelineState> FDefaultTemporalUpscaler::TryCreateComputePipeline(const char* CsoFile)
{
	std::vector<char> cs;
	{
		std::ifstream file(CsoFile, std::ios_base::in | std::ios_base::binary);
		if (!file)
		{
			char msg[256];
			sprintf_s(msg, "[TemporalAA] cso not found: %s\n", CsoFile);
			OutputDebugStringA(msg);
			return nullptr;
		}
		file.seekg(0, std::ios_base::end);
		const std::streamoff size = file.tellg();
		file.seekg(0, std::ios_base::beg);
		if (size <= 0)
		{
			char msg[256];
			sprintf_s(msg, "[TemporalAA] cso is empty: %s\n", CsoFile);
			OutputDebugStringA(msg);
			return nullptr;
		}
		cs.resize((size_t)size);
		file.read(cs.data(), size);
	}

	D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
	desc.pRootSignature = m_RootSignature.Get();
	desc.CS.pShaderBytecode = cs.data();
	desc.CS.BytecodeLength = cs.size();

	ComPtr<ID3D12PipelineState> pso;
	const HRESULT hr = m_RHI->GetDevice()->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pso));
	if (FAILED(hr))
	{
		char msg[256];
		sprintf_s(msg, "[TemporalAA] PSO creation failed: %s (hr=0x%08X)\n", CsoFile, (unsigned int)hr);
		OutputDebugStringA(msg);
		return nullptr;
	}
	return pso;
}


unsigned int FDefaultTemporalUpscaler::GetDummySRVIndex() const
{
	return m_DummyTex ? m_DummyTex->SRVIndex : 0u;
}


bool FDefaultTemporalUpscaler::IsReady(const FViewFamilyInfo& Family) const
{
	const int pass = std::clamp((int)Family.TAAPass, 0, kNumTAAPassConfigs - 1);
	const int quality = std::clamp((int)Family.TAAQuality, 0, kNumTAAQualities - 1);
	const int ds = Family.bTAADownsample ? 1 : 0;
	const bool bNeedsMitchellNetravali = (Family.TAAPass == ETAAPassConfig::MainSuperSampling);
	if (m_PSO[pass][quality][ds] && (!bNeedsMitchellNetravali || m_PSOMitchellNetravali))
	{
		return true;
	}

	if (m_PSO[pass][quality][ds])
	{
		// TAA 本体はあるが SuperSampling の後段 (H -> S の Mitchell-Netravali) が無い。初回だけログ
		constexpr uint32_t kMNBit = 1u << 31;
		if ((m_LoggedMissingPSOMask & kMNBit) == 0u)
		{
			m_LoggedMissingPSOMask |= kMNBit;
			OutputDebugStringA("[TemporalAA] missing PSO TemporalAAMitchellNetravali (MainSuperSampling) -> AA disabled\n");
		}
		return false;
	}

	// 組合せごとに初回だけログ
	const uint32_t bit = 1u << (uint32_t)(pass * 8 + quality * 2 + ds);
	if ((m_LoggedMissingPSOMask & bit) == 0u)
	{
		m_LoggedMissingPSOMask |= bit;
		char msg[160];
		sprintf_s(msg, "[TemporalAA] missing PSO (%s/%s/%d) -> AA disabled\n",
			GetTAAPassConfigName(Family.TAAPass), GetTAAQualityName(Family.TAAQuality), ds);
		OutputDebugStringA(msg);
	}
	return false;
}


void FDefaultTemporalUpscaler::BindComputeTables(ID3D12GraphicsCommandList* CommandList, const unsigned int SRVs[6], const unsigned int UAVs[3])
{
	for (UINT i = 0; i < kNumSRVTables; ++i)
		CommandList->SetComputeRootDescriptorTable(kRootSRVBase + i, m_RHI->GetGPUDescriptorHandle(SRVs[i]));
	for (UINT i = 0; i < kNumUAVTables; ++i)
		CommandList->SetComputeRootDescriptorTable(kRootUAVBase + i, m_RHI->GetGPUDescriptorHandle(UAVs[i]));
}


// ============================================================
//  AddPasses (UE FDefaultTemporalUpscaler::AddPasses, 4.26 形。§4.8.2)
// ============================================================
ITemporalUpscaler::FPassOutputs FDefaultTemporalUpscaler::AddPasses(const FViewInfo& View, const FViewFamilyInfo& Family,
	FSceneViewState& ViewState, const FAntiAliasingParams& Params, const FTemporalAADebugSettings& Debug, const FPassInputs& In)
{
	FTAAPassParameters P;
	P.Pass = Family.TAAPass;
	P.Quality = Family.TAAQuality;                                          // SuperSampling は High 強制済み
	P.bDownsample = In.bAllowDownsampleSceneColor && P.Quality == ETAAQuality::Low && P.Pass != ETAAPassConfig::MainSuperSampling;
	P.bUseR11G11B10History = Family.bR11G11B10History;
	P.bUpsampleFiltered = Params.bTemporalAAUpsampleFiltered || P.Pass != ETAAPassConfig::MainUpsampling;   // UE: TAA_UPSAMPLE_FILTERED = CVar || Pass != MainUpsampling
	P.SceneColorInput = In.SceneColorTexture;
	P.SceneDepthSRVIndex = In.SceneDepthSRVIndex;
	P.SceneVelocitySRVIndex = In.SceneVelocitySRVIndex;
	P.ResponsiveMaskSRVIndex = In.ResponsiveMaskSRVIndex;
	P.bResponsiveMaskValid = In.bResponsiveMaskValid;
	P.EyeAdaptationSRVIndex = In.EyeAdaptationSRVIndex;
	P.bUseEyeAdaptationBuffer = In.bUseEyeAdaptationBuffer;
	P.ManualExposure = In.ManualExposure;
	P.InputExtent = Family.RenderExtent;
	P.OutputExtent = Family.HistoryExtent;                                   // SetupViewRect (+ SuperSampling 拡大)
	P.CurrentFrameWeight = Params.TemporalAACurrentFrameWeight;
	P.FilterSize = Params.TemporalAAFilterSize;
	P.bCatmullRom = Params.bTemporalAACatmullRom;
	P.bForceHistoryBypass = In.bForceHistoryBypass;
	P.DebugView = Debug.DebugView;
	P.DebugScale = Debug.VisualizeScale;
	P.FilteredTemporalWeightMode = Debug.FilteredTemporalWeightMode;
	P.NearClip = View.NearClip;
	P.FarClip = View.FarClip;

	FPassOutputs Out;
	if (P.SceneColorInput == nullptr)
	{
		return FPassOutputs{};
	}
	const FTAAOutputs o = AddTemporalAAPass(View, P, View.PrevViewInfo.TemporalAAHistory, &Out.NewHistory, ViewState);
	if (!o.SceneColor)
	{
		return FPassOutputs{};                                              // PSO 欠落 (IsReady 済みなので通常起きない)
	}
	if (P.Pass == ETAAPassConfig::MainSuperSampling)
	{
		// UE 4.26: 拡大履歴 (H = S x HistoryUpscaleFactor) を Mitchell-Netravali で S へ戻し、それを後段へ渡す。
		// 新しい履歴 (Out.NewHistory) は拡大解像度 H のまま次フレームへ持ち越す
		FTAATexture* mn = ComputeMitchellNetravaliDownsample(o.SceneColor, Family.SecondaryExtent);
		if (mn == nullptr)
		{
			// MN の PSO 欠落 (IsReady 済みなので通常起きない)。履歴 (RD) を後段へは渡さず TAA 不実行として扱う
			return FPassOutputs{};
		}
		Out.SceneColor = mn->RT.get();
		Out.SceneColorExtent = mn->Extent;                                   // = S (後段のサイズは実テクスチャから)
	}
	else
	{
		m_MNOutput.Release();                                               // SuperSampling 以外は不要 (遅延解放)
		Out.SceneColor = o.SceneColor->RT.get();
		Out.SceneColorExtent = o.SceneColor->Extent;
		if (o.DownsampledSceneColor)
		{
			Out.HalfResSceneColor = o.DownsampledSceneColor->RT.get();
			Out.HalfResExtent = o.DownsampledSceneColor->Extent;
		}
	}
	Out.bHistoryValid = o.bHistoryValid;
	return Out;
}


// ============================================================
//  AddTemporalAAPass (UE AddTemporalAAPass。§4.8.3)
//  入力履歴 (Hp) を読み、ピンポンのもう一方のスロットへ新しい履歴 (H) を書く。
//  バリアは 1 回のバッチ (同一リソースの重複 / before == after は除去)
// ============================================================
FTAAOutputs FDefaultTemporalUpscaler::AddTemporalAAPass(const FViewInfo& View, const FTAAPassParameters& P,
	const FTemporalAAHistory& In, FTemporalAAHistory* Out, FSceneViewState& VS)
{
	ID3D12PipelineState* pso = m_PSO[(int)P.Pass][(int)P.Quality][P.bDownsample ? 1 : 0].Get();
	if (pso == nullptr || P.SceneColorInput == nullptr)
	{
		return {};
	}
	const DXGI_FORMAT fmt = P.bUseR11G11B10History ? DXGI_FORMAT_R11G11B10_FLOAT : DXGI_FORMAT_R16G16B16A16_FLOAT;
	const XMUINT2 R = P.InputExtent, H = P.OutputExtent;

	// ---- 入力履歴 (UE: !InputHistory.IsValid() || bCameraCut ならダミー黒) ----
	FTAATexture* inTex = VS.GetHistoryTexture(In);
	const bool bInValid = In.IsValid() && inTex && inTex->RT
		&& inTex->Extent.x == In.ReferenceBufferSize.x && inTex->Extent.y == In.ReferenceBufferSize.y && inTex->Format == In.Format;
	const bool bHistory = bInValid && !View.bCameraCut && !P.bForceHistoryBypass;
	const XMUINT2 Hp = bHistory ? In.ReferenceBufferSize : H;

	// ---- 出力スロット (入力と別のピンポン枠。サイズ / フォーマット不一致なら遅延解放 + 再確保) ----
	const int outSlot = bInValid ? (In.RTSlot ^ 1) : 0;
	FTAATexture& outTex = VS.TemporalAAHistoryPool[outSlot];
	if (!outTex.Matches(H, fmt))
	{
		outTex.Allocate(m_RHI, H, fmt, L"TemporalAA");
	}

	// ---- 定数 (§3.4) ----
	FTemporalAAParameters cb{};
	cb.InputSceneColorSize = { (float)R.x, (float)R.y, 1.0f / (float)R.x, 1.0f / (float)R.y };
	cb.InputMinMaxPixelCoord = { 0, 0, (int)R.x - 1, (int)R.y - 1 };
	cb.OutputViewportSize = { (float)H.x, (float)H.y, 1.0f / (float)H.x, 1.0f / (float)H.y };
	cb.HistoryBufferSize = { (float)Hp.x, (float)Hp.y, 1.0f / (float)Hp.x, 1.0f / (float)Hp.y };
	cb.HistoryBufferUVMinMax = { 0.5f / (float)Hp.x, 0.5f / (float)Hp.y, ((float)Hp.x - 0.5f) / (float)Hp.x, ((float)Hp.y - 0.5f) / (float)Hp.y };   // Off = 0, Ext = Buf (exact-size)
	cb.ScreenPosToHistoryBufferUV = { 0.5f, -0.5f, 0.5f, 0.5f };                                        // = (Ext*0.5/Buf, -Ext*0.5/Buf, (Ext*0.5+Off)/Buf ...)
	XMStoreFloat4x4(&cb.ClipToPrevClip, XMMatrixTranspose(XMLoadFloat4x4(&View.ClipToPrevClip)));
	cb.TemporalJitterPixels = View.TemporalJitterPixels;
	cb.ScreenPosAbsMax = { 1.0f - 1.0f / (float)Hp.x, 1.0f - 1.0f / (float)Hp.y };
	cb.ScreenPercentage = (float)R.x / (float)H.x;
	cb.UpscaleFactor = (float)H.x / (float)R.x;
	cb.CurrentFrameWeight = P.CurrentFrameWeight;
	cb.HistoryPreExposureCorrection = 1.0f;                                  // プリエクスポージャ無し
	cb.bCameraCut = bHistory ? 0u : 1u;
	cb.Flags = (P.bUpsampleFiltered ? (uint32_t)TAA_FLAG_UPSAMPLE_FILTERED : 0u)
		| (P.bResponsiveMaskValid ? (uint32_t)TAA_FLAG_RESPONSIVE_MASK_VALID : 0u)
		| (P.bUseEyeAdaptationBuffer ? (uint32_t)TAA_FLAG_EYE_ADAPTATION_BUFFER : 0u)
		| ((bHistory && inTex->Format == DXGI_FORMAT_R16G16B16A16_FLOAT) ? (uint32_t)TAA_FLAG_HISTORY_HAS_ALPHA : 0u)
		| ((uint32_t)(P.FilteredTemporalWeightMode & 3) << TAA_FLAG_FTW_MODE_SHIFT)
		| (P.bDownsample ? (uint32_t)TAA_FLAG_DOWNSAMPLE_OUTPUT : 0u);
	cb.ManualExposure = P.ManualExposure;
	cb.DebugMode = IsTemporalAADebugViewFromCS(P.DebugView) ? (uint32_t)P.DebugView : 0u;
	if (P.Pass == ETAAPassConfig::Main)
	{
		float sw[9], pw[5];
		ComputeTemporalAASampleWeights(View.TemporalJitterPixels, P.FilterSize, P.bCatmullRom, sw, pw);
		for (int i = 0; i < 9; ++i) (&cb.SampleWeights[i >> 2].x)[i & 3] = sw[i];
		for (int i = 0; i < 5; ++i) (&cb.PlusWeights[i >> 2].x)[i & 3] = pw[i];
	}
	const XMFLOAT3 qe = ComputePixelFormatQuantizationError(fmt);
	cb.OutputQuantizationError = { qe.x, qe.y, qe.z, (fmt == DXGI_FORMAT_R11G11B10_FLOAT) ? 64512.0f : 65504.0f };   // w = 出力フォーマットの最大有限値
	const float n = P.NearClip, f = P.FarClip, Q = f / (f - n);
	cb.DepthParams = { Q, -Q * n, 0.999f * f, 0.0f };
	cb.StateFrameIndexMod8 = View.StateFrameIndex & 7u;                     // 量子化ノイズは常に回す
	cb.DebugScale = P.DebugScale;
	cb.SampleDistanceThreshold = 1.51f + (1.3f - 1.51f) * (cb.UpscaleFactor - 1.0f);
	const unsigned frame = m_RHI->GetCurrentFrameIndex();
	std::memcpy(m_ParamPtr[frame] + kParamSlotTAA * kParamSlotSize, &cb, sizeof(cb));

	// ---- 補助出力 ----
	// ハーフ解像度 (UE 5.x の DownsampledSceneColor, Quality Low + r.TemporalAA.AllowDownsampling):
	// TAA 出力 H の 2x2 ボックス平均 (有効画素のみで重み付け) を ceil(H/2) の RGBA16F へ書く。
	// AutoExposure の入力と Bloom のしきい値パスの入力になる (後段の AE / Bloom の入力解像度が下がる)
	FTAATexture* half = nullptr;
	if (P.bDownsample)
	{
		const XMUINT2 he = { (H.x + 1u) / 2u, (H.y + 1u) / 2u };
		if (!m_HalfRes.Matches(he, DXGI_FORMAT_R16G16B16A16_FLOAT))
		{
			m_HalfRes.Allocate(m_RHI, he, DXGI_FORMAT_R16G16B16A16_FLOAT, L"TemporalAAHalfRes");
		}
		half = &m_HalfRes;
	}
	FTAATexture* dbg = nullptr;
	if (cb.DebugMode != 0u)
	{
		if (!m_DebugOutput.Matches(H, DXGI_FORMAT_R16G16B16A16_FLOAT))
		{
			m_DebugOutput.Allocate(m_RHI, H, DXGI_FORMAT_R16G16B16A16_FLOAT, L"TemporalAADebug");
		}
		dbg = &m_DebugOutput;
	}
	// 今フレーム使わない補助出力は解放する (遅延解放。GetDebugOutput は DebugMode != 0 の CS 表示でのみ読まれる)
	if (!P.bDownsample) m_HalfRes.Release();
	if (cb.DebugMode == 0u) m_DebugOutput.Release();

	// ---- バリア (1 回のバッチ。同一リソースの重複 / before == after は除去) ----
	ID3D12GraphicsCommandList* cl = m_RHI->GetGraphicsCommandList();
	FBarrierBatch pre;
	pre.Add(P.SceneColorInput->Resource.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
	if (bHistory) pre.Track(*inTex, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
	pre.Track(outTex, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	if (half) pre.Track(*half, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	if (dbg) pre.Track(*dbg, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	pre.Flush(cl);

	// ---- ディスパッチ (グラフィックスのルートバインドは乱さない。ヒープは同じ SRV ヒープ) ----
	ID3D12DescriptorHeap* heap = m_RHI->GetSRVDescriptorHeap();
	cl->SetDescriptorHeaps(1, &heap);
	cl->SetComputeRootSignature(m_RootSignature.Get());
	cl->SetPipelineState(pso);
	cl->SetComputeRootConstantBufferView(kRootCBV, m_ParamBuffer[frame]->GetGPUVirtualAddress() + kParamSlotTAA * kParamSlotSize);
	const unsigned int dummyUAV = m_DummyUAVTex->UAVIndex;
	const unsigned int srvs[kNumSRVTables] =
	{
		P.SceneColorInput->SRVIndex,                                // t0 InputSceneColor (R)
		P.SceneDepthSRVIndex,                                       // t1 SceneLinearDepth
		P.SceneVelocitySRVIndex,                                    // t2 SceneVelocity (またはダミー)
		bHistory ? inTex->RT->SRVIndex : m_DummyTex->SRVIndex,      // t3 HistoryBuffer (Hp)
		P.EyeAdaptationSRVIndex,                                    // t4 EyeAdaptationBuffer
		P.ResponsiveMaskSRVIndex,                                   // t5 ResponsiveAAMask (またはダミー)
	};
	const unsigned int uavs[kNumUAVTables] =
	{
		outTex.RT->UAVIndex,                                        // u0 OutComputeTex (H)
		half ? half->RT->UAVIndex : dummyUAV,                       // u1 OutComputeTexDownsampled (ceil(H/2))
		dbg ? dbg->RT->UAVIndex : dummyUAV,                         // u2 DebugOutput
	};
	BindComputeTables(cl, srvs, uavs);
	cl->Dispatch((H.x + kTAATileSize - 1u) / kTAATileSize, (H.y + kTAATileSize - 1u) / kTAATileSize, 1u);

	// ---- 戻しバリア ----
	FBarrierBatch post;
	post.Track(outTex, P.Pass == ETAAPassConfig::MainSuperSampling ? kReadState
		: D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);                     // Main/Upsampling: AutoExposure の入口 PSR 契約
	if (half) post.Track(*half, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);   // AutoExposure の入口 PSR 契約 / Bloom しきい値 (t0)
	if (dbg) post.Track(*dbg, kReadState);                                 // 可視化 PS (t36)
	post.Add(P.SceneColorInput->Resource.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	post.Flush(cl);
	// 入力履歴は NPSR のまま (次フレームの出力スロット。追跡状態から UAV へ遷移する)

	*Out = FTemporalAAHistory{};
	Out->RTSlot = outSlot;
	Out->ReferenceBufferSize = H;
	Out->ViewportSize = H;
	Out->Format = fmt;

	FTAAOutputs result;
	result.SceneColor = &outTex;
	result.DownsampledSceneColor = half;
	result.bHistoryValid = bHistory;
	return result;
}


// ============================================================
//  ComputeMitchellNetravaliDownsample (§4.8.4, [L])
//  MainSuperSampling の TAA 出力 (拡大履歴 H。AddTemporalAAPass の戻しバリアで RD) を
//  Mitchell-Netravali (B = C = 1/3, 台 = 出力 2 px, 分離可能, 正規化) で OutputExtent (= S) へ
//  ダウンサンプルする。出力 m_MNOutput は PSR 常駐 (AutoExposure / Bloom / Tonemap が読む)
// ============================================================
FTAATexture* FDefaultTemporalUpscaler::ComputeMitchellNetravaliDownsample(FTAATexture* Input, XMUINT2 OutputExtent)
{
	if (m_PSOMitchellNetravali == nullptr || Input == nullptr || !Input->RT || OutputExtent.x == 0u || OutputExtent.y == 0u)
	{
		return nullptr;
	}
	const XMUINT2 H = Input->Extent, S = OutputExtent;

	// ---- (1) 出力 (S, RGBA16F)。サイズ不一致なら遅延解放 + 再確保 (初期状態 PSR) ----
	if (!m_MNOutput.Matches(S, DXGI_FORMAT_R16G16B16A16_FLOAT))
	{
		m_MNOutput.Allocate(m_RHI, S, DXGI_FORMAT_R16G16B16A16_FLOAT, L"TemporalAAMitchellNetravali");
	}

	// ---- (2) 定数 (§3.5, リングのスロット 1) ----
	FMitchellNetravaliParameters cb{};
	cb.InputSize = { (float)H.x, (float)H.y, 1.0f / (float)H.x, 1.0f / (float)H.y };
	cb.OutputSize = { (float)S.x, (float)S.y, 1.0f / (float)S.x, 1.0f / (float)S.y };
	cb.InputPerOutputPixel = { (float)H.x / (float)S.x, (float)H.y / (float)S.y };
	const unsigned frame = m_RHI->GetCurrentFrameIndex();
	std::memcpy(m_ParamPtr[frame] + kParamSlotMitchellNetravali * kParamSlotSize, &cb, sizeof(cb));

	// ---- (3) バリア: 入力は TAA パス後の RD のまま (NPSR を含む。追跡状態が RD なら何もしない)、出力 PSR -> UAV ----
	ID3D12GraphicsCommandList* cl = m_RHI->GetGraphicsCommandList();
	FBarrierBatch pre;
	pre.Track(*Input, kReadState);
	pre.Track(m_MNOutput, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	pre.Flush(cl);

	// ---- (4) ディスパッチ (TAA と同じルートシグネチャ。使わないテーブルはダミー) ----
	ID3D12DescriptorHeap* heap = m_RHI->GetSRVDescriptorHeap();
	cl->SetDescriptorHeaps(1, &heap);
	cl->SetComputeRootSignature(m_RootSignature.Get());
	cl->SetPipelineState(m_PSOMitchellNetravali.Get());
	cl->SetComputeRootConstantBufferView(kRootCBV, m_ParamBuffer[frame]->GetGPUVirtualAddress() + kParamSlotMitchellNetravali * kParamSlotSize);
	const unsigned int dummy = m_DummyTex->SRVIndex;
	const unsigned int dummyUAV = m_DummyUAVTex->UAVIndex;
	const unsigned int srvs[kNumSRVTables] =
	{
		Input->RT->SRVIndex,                                        // t0 InputTexture (履歴 H)
		dummy, dummy, dummy,                                        // t1..t3
		m_DummyEyeAdaptationSRV,                                    // t4
		dummy,                                                      // t5
	};
	const unsigned int uavs[kNumUAVTables] = { m_MNOutput.RT->UAVIndex, dummyUAV, dummyUAV };   // u0 OutputTexture (S)
	BindComputeTables(cl, srvs, uavs);
	cl->Dispatch((S.x + kTAATileSize - 1u) / kTAATileSize, (S.y + kTAATileSize - 1u) / kTAATileSize, 1u);

	// ---- (5) 出力 UAV -> PSR (AutoExposure の入口 PSR 契約) ----
	FBarrierBatch post;
	post.Track(m_MNOutput, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	post.Flush(cl);
	return &m_MNOutput;
}


// ============================================================
//  GPU 自己テスト (§4.8.5)。BeginFrame 先頭からのみ呼ぶ
//  (1) FlushAndReset (記録済みを実行してアイドル) -> (2) 記録 -> (3) FlushAndReset (待機 +
//  グラフィックス状態の復帰) -> (4) READBACK を Map して 64 float を取り出す
// ============================================================
bool FDefaultTemporalUpscaler::RunGPUSelfTest(std::vector<float>& OutValues)
{
	OutValues.clear();
	if (!m_PSOSelfTest || !m_SelfTestBuffer || !m_SelfTestReadback)
	{
		return false;
	}

	m_RHI->FlushAndResetCommandList();
	ID3D12GraphicsCommandList* cl = m_RHI->GetGraphicsCommandList();

	// スロット 2: シェーダが入力に掛ける実行時定数 (One = 1, Zero = 0)。fxc の定数畳み込みを防ぎ、
	// ヘルパーを実際に GPU で評価させる (GPU は FlushAndReset 直後でアイドル)
	const unsigned frame = m_RHI->GetCurrentFrameIndex();
	{
		const float runtimeConstants[4] = { 1.0f, 0.0f, 0.0f, 0.0f };
		std::memset(m_ParamPtr[frame] + kParamSlotSelfTest * kParamSlotSize, 0, kParamSlotSize);
		std::memcpy(m_ParamPtr[frame] + kParamSlotSelfTest * kParamSlotSize, runtimeConstants, sizeof(runtimeConstants));
	}

	// バッファは COMMON (減衰) -> UAV
	cl->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(m_SelfTestBuffer.Get(),
		D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS));

	ID3D12DescriptorHeap* heap = m_RHI->GetSRVDescriptorHeap();
	cl->SetDescriptorHeaps(1, &heap);
	cl->SetComputeRootSignature(m_RootSignature.Get());
	cl->SetPipelineState(m_PSOSelfTest.Get());
	cl->SetComputeRootConstantBufferView(kRootCBV, m_ParamBuffer[frame]->GetGPUVirtualAddress() + kParamSlotSelfTest * kParamSlotSize);
	const unsigned int dummy = m_DummyTex->SRVIndex;
	const unsigned int srvs[kNumSRVTables] = { dummy, dummy, dummy, dummy, m_DummyEyeAdaptationSRV, dummy };
	const unsigned int uavs[kNumUAVTables] = { m_SelfTestUAV, m_DummyUAVTex->UAVIndex, m_DummyUAVTex->UAVIndex };
	BindComputeTables(cl, srvs, uavs);
	cl->Dispatch(1, 1, 1);

	cl->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(m_SelfTestBuffer.Get(),
		D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE));
	cl->CopyBufferRegion(m_SelfTestReadback.Get(), 0, m_SelfTestBuffer.Get(), 0, kSelfTestValueCount * sizeof(float));
	cl->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(m_SelfTestBuffer.Get(),
		D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS));
	// (UNORDERED_ACCESS のまま実行完了で COMMON へ減衰する)

	m_RHI->FlushAndResetCommandList();			// 待機 + ヒープ / ルートシグネチャ / 既定ビューポートの復帰

	const D3D12_RANGE readRange{ 0, kSelfTestValueCount * sizeof(float) };
	float* ptr = nullptr;
	if (FAILED(m_SelfTestReadback->Map(0, &readRange, (void**)&ptr)) || ptr == nullptr)
	{
		return false;
	}
	OutValues.assign(ptr, ptr + kSelfTestValueCount);
	const D3D12_RANGE noWrite{ 0, 0 };
	m_SelfTestReadback->Unmap(0, &noWrite);
	return true;
}
