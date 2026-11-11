#include "Main.h"
#include "RenderManager.h"
#include "VolumetricFog.h"
#include "LightGridInjection.h"
#include "ShadowRendering.h"
#include "Halton.h"		// Halton 列 (VolumetricFogTemporalRandom 相当のセル内ジッタ。Lumen / TAA と共有)
#include "D3DX12.h"
#include <fstream>
#include <vector>
#include <cstring>
#include <cmath>
#include <algorithm>

// ============================================================
//  FVolumetricFog : Volumetric Fog の froxel ボリューム構築 (compute)。
// ============================================================

FVolumetricFog::FVolumetricFog(RenderManager* owner)
	: m_Owner(owner)
{
}

FVolumetricFog::~FVolumetricFog()
{
	for (int i = 0; i < 2; ++i)
	{
		if (m_ParamBuffer[i] && m_ParamPtr[i])
			m_ParamBuffer[i]->Unmap(0, nullptr);
		if (m_ElementBuffer[i] && m_ElementPtr[i])
			m_ElementBuffer[i]->Unmap(0, nullptr);
	}

	ReleaseVolumes();
}

// ボリュームテクスチャ / デスクリプタ枠は遅延削除キューへ
// (in-flight のコマンドリストが参照している可能性があるため)
void FVolumetricFog::ReleaseVolumes()
{
	auto release = [&](FVolumeTexture& v)
		{
			if (v.Resource)
			{
				m_Owner->DeferredRelease(std::move(v.Resource), (int)v.SRVIndex, -1);
				m_Owner->ReleaseShaderResourceView(v.UAVIndex);	// UAV も SRV ヒープの枠
			}
			v.Resource.Reset();
			v.SRVIndex = 0;
			v.UAVIndex = 0;
			v.State = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
		};
	release(m_VBufferA);
	release(m_VBufferB);
	release(m_LightScattering[0]);
	release(m_LightScattering[1]);
	release(m_IntegratedLightScattering);
}

void FVolumetricFog::CreateVolumes(unsigned int Width, unsigned int Height)
{
	assert(m_VBufferA.Resource == nullptr && "FVolumetricFog::CreateVolumes: ReleaseVolumes first");

	// ------------------------------------------------------------
	//  グリッド次元 (レンダー解像度 R から確定)
	// ------------------------------------------------------------
	m_ViewWidth = (Width < 1u) ? 1u : Width;
	m_ViewHeight = (Height < 1u) ? 1u : Height;
	m_GridSizeX = (m_ViewWidth + VOLUMETRIC_FOG_GRID_PIXEL_SIZE - 1) / VOLUMETRIC_FOG_GRID_PIXEL_SIZE;
	m_GridSizeY = (m_ViewHeight + VOLUMETRIC_FOG_GRID_PIXEL_SIZE - 1) / VOLUMETRIC_FOG_GRID_PIXEL_SIZE;
	m_GridSizeZ = VOLUMETRIC_FOG_GRID_SIZE_Z;

	m_Stats.GridSizeX = m_GridSizeX;
	m_Stats.GridSizeY = m_GridSizeY;
	m_Stats.GridSizeZ = m_GridSizeZ;
	m_Stats.NumFroxels = m_GridSizeX * m_GridSizeY * m_GridSizeZ;

	// ------------------------------------------------------------
	//  ボリュームテクスチャ (全て R16G16B16A16_FLOAT, GridSize, UAV 状態で開始)
	// ------------------------------------------------------------
	CreateVolumeTexture(m_VBufferA, L"VolumetricFogVBufferA", true);	// ボクセル化の RT0
	CreateVolumeTexture(m_VBufferB, L"VolumetricFogVBufferB", true);	// ボクセル化の RT1
	CreateVolumeTexture(m_LightScattering[0], L"VolumetricFogLightScattering0");
	CreateVolumeTexture(m_LightScattering[1], L"VolumetricFogLightScattering1");
	CreateVolumeTexture(m_IntegratedLightScattering, L"VolumetricFogIntegratedLightScattering");

	// ------------------------------------------------------------
	//  ボクセル化の RTV (Texture3D の全 W スライス。GS が SV_RenderTargetArrayIndex で選ぶ)。
	//  自前ヒープの固定 2 枠へ作り直す (CreateVolumes は GPU アイドル時にしか呼ばれない)
	// ------------------------------------------------------------
	if (m_VoxelizeRTVHeap)
	{
		FVolumeTexture* targets[2] = { &m_VBufferA, &m_VBufferB };
		for (int i = 0; i < 2; ++i)
		{
			D3D12_RENDER_TARGET_VIEW_DESC rtv{};
			rtv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
			rtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE3D;
			rtv.Texture3D.MipSlice = 0;
			rtv.Texture3D.FirstWSlice = 0;
			rtv.Texture3D.WSize = m_GridSizeZ;
			Device()->CreateRenderTargetView(targets[i]->Resource.Get(), &rtv, m_VoxelizeRTV[i]);
		}
	}

	// 履歴 (LightScattering) の中身は未定義 -> 次の Dispatch はテンポラル再投影を使わない。
	// m_FrameNumber (ジッタ位相) / m_LightScatteringFrame (ピンポン) は保持する
	m_bHistoryValid = false;
}

ID3D12Device* FVolumetricFog::Device()
{
	return m_Owner->GetDevice();
}

ID3D12GraphicsCommandList* FVolumetricFog::CommandList()
{
	return m_Owner->GetGraphicsCommandList();
}

bool FVolumetricFog::ReadShaderBytecode(const char* csoFile, std::vector<char>& Out)
{
	std::ifstream file(csoFile, std::ios_base::in | std::ios_base::binary);
	if (!file)
	{
		char msg[512];
		sprintf_s(msg, "[VolumetricFog] shader .cso not found: %s\n", csoFile);
		OutputDebugStringA(msg);
		assert(false && "VolumetricFog shader .cso not found");
		return false;
	}
	file.seekg(0, std::ios_base::end);
	int filesize = (int)file.tellg();
	file.seekg(0, std::ios_base::beg);
	Out.resize(filesize);
	if (filesize > 0)
	{
		file.read(&Out[0], filesize);
	}
	file.close();
	return filesize > 0;
}

ComPtr<ID3D12PipelineState> FVolumetricFog::CreateComputePipeline(const char* csoFile)
{
	std::vector<char> cs;
	if (!ReadShaderBytecode(csoFile, cs))
	{
		return nullptr;
	}

	D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
	desc.pRootSignature = m_RootSignature.Get();
	desc.CS.pShaderBytecode = cs.data();
	desc.CS.BytecodeLength = cs.size();

	ComPtr<ID3D12PipelineState> pso;
	HRESULT hr = Device()->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pso));
	assert(SUCCEEDED(hr));
	return pso;
}

// ------------------------------------------------------------
//  GetVolumetricFogGridZParams 相当。
//  froxel の Z を指数分布 (B, O, S) で切る:
//    Slice = log2(Depth * B + O) * S
//  S = DepthDistributionScale (32)。near は 9.5cm 押し出して
//  最前面にスライスが密集しすぎないようにする。
//  far (= VolumetricFogDistance) が最終スライス (GridSizeZ - 1) の
//  開始境界に一致する。
// ------------------------------------------------------------
XMFLOAT3 FVolumetricFog::ComputeGridZParams(float NearPlane, float FarPlane)
{
	const double NearOffset = 0.095;	// [m]
	const double S = (double)VOLUMETRIC_FOG_DEPTH_DISTRIBUTION_SCALE;

	double N = (double)NearPlane + NearOffset;
	double F = ((double)FarPlane > N + 1.0) ? (double)FarPlane : (N + 1.0);

	double O = (F - N * std::exp2((double)(VOLUMETRIC_FOG_GRID_SIZE_Z - 1) / S)) / (F - N);
	double B = (1.0 - O) / N;

	return XMFLOAT3((float)B, (float)O, (float)S);
}

void FVolumetricFog::CreateVolumeTexture(FVolumeTexture& Out, const wchar_t* Name, bool bRenderTarget)
{
	D3D12_HEAP_PROPERTIES prop{};
	prop.Type = D3D12_HEAP_TYPE_DEFAULT;

	D3D12_RESOURCE_DESC d{};
	d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
	d.Width = m_GridSizeX;
	d.Height = m_GridSizeY;
	d.DepthOrArraySize = (UINT16)m_GridSizeZ;
	d.MipLevels = 1;
	d.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
	d.SampleDesc.Count = 1;
	d.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
	d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
	if (bRenderTarget)
	{
		d.Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;	// ボクセル化の加算 RTV
	}

	HRESULT hr = Device()->CreateCommittedResource(&prop, D3D12_HEAP_FLAG_NONE,
		&d, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
		IID_PPV_ARGS(&Out.Resource));
	assert(SUCCEEDED(hr));
	Out.Resource->SetName(Name);
	Out.State = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

	// SRV (Texture3D)
	Out.SRVIndex = m_Owner->AllocateDescriptor();
	{
		D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
		srv.Format = d.Format;
		srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
		srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		srv.Texture3D.MostDetailedMip = 0;
		srv.Texture3D.MipLevels = 1;
		srv.Texture3D.ResourceMinLODClamp = 0.0f;
		Device()->CreateShaderResourceView(Out.Resource.Get(), &srv,
			m_Owner->GetCPUDescriptorHandle(Out.SRVIndex));
	}

	// UAV (Texture3D, 全スライス)
	Out.UAVIndex = m_Owner->AllocateDescriptor();
	{
		D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
		uav.Format = d.Format;
		uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
		uav.Texture3D.MipSlice = 0;
		uav.Texture3D.FirstWSlice = 0;
		uav.Texture3D.WSize = m_GridSizeZ;
		Device()->CreateUnorderedAccessView(Out.Resource.Get(), nullptr, &uav,
			m_Owner->GetCPUDescriptorHandle(Out.UAVIndex));
	}
}

void FVolumetricFog::Transition(FVolumeTexture& Volume, D3D12_RESOURCE_STATES NewState)
{
	if (Volume.State == NewState || Volume.Resource == nullptr)
	{
		return;
	}
	CommandList()->ResourceBarrier(1,
		&CD3DX12_RESOURCE_BARRIER::Transition(Volume.Resource.Get(), Volume.State, NewState));
	Volume.State = NewState;
}

void FVolumetricFog::Init()
{
	// ------------------------------------------------------------
	//  コンピュートルートシグネチャ (3 パス共通)
	//  デスクリプタはフリーリストから個別確保され連続性が無いため、
	//  1 テーブル = 1 デスクリプタで分割する (FLightGridInjection と同じ)。
	// ------------------------------------------------------------
	{
		D3D12_DESCRIPTOR_RANGE rangeSRV[NUM_SRV_SLOTS]{};
		for (unsigned int i = 0; i < NUM_SRV_SLOTS; ++i)
		{
			rangeSRV[i].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
			rangeSRV[i].NumDescriptors = 1;
			rangeSRV[i].BaseShaderRegister = i; // t0..t10
			rangeSRV[i].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
		}

		D3D12_DESCRIPTOR_RANGE rangeUAV[NUM_UAV_SLOTS]{};
		for (unsigned int i = 0; i < NUM_UAV_SLOTS; ++i)
		{
			rangeUAV[i].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
			rangeUAV[i].NumDescriptors = 1;
			rangeUAV[i].BaseShaderRegister = i; // u0..u3
			rangeUAV[i].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
		}

		D3D12_ROOT_PARAMETER params[1 + NUM_SRV_SLOTS + NUM_UAV_SLOTS]{};
		params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
		params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
		params[0].Descriptor.ShaderRegister = 0;
		params[0].Descriptor.RegisterSpace = 0;

		for (unsigned int i = 0; i < NUM_SRV_SLOTS; ++i)
		{
			params[1 + i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
			params[1 + i].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
			params[1 + i].DescriptorTable.NumDescriptorRanges = 1;
			params[1 + i].DescriptorTable.pDescriptorRanges = &rangeSRV[i];
		}
		for (unsigned int i = 0; i < NUM_UAV_SLOTS; ++i)
		{
			params[1 + NUM_SRV_SLOTS + i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
			params[1 + NUM_SRV_SLOTS + i].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
			params[1 + NUM_SRV_SLOTS + i].DescriptorTable.NumDescriptorRanges = 1;
			params[1 + NUM_SRV_SLOTS + i].DescriptorTable.pDescriptorRanges = &rangeUAV[i];
		}

		// s0: 線形クランプ (ボリューム履歴 / スカイキューブ)
		// s1: シャドウ比較 (LESS_EQUAL, ボーダー白 = マップ外は影なし。グラフィックス s2 と同じ)
		D3D12_STATIC_SAMPLER_DESC samplers[2]{};
		samplers[0].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
		samplers[0].AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
		samplers[0].AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
		samplers[0].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
		samplers[0].MipLODBias = 0.0f;
		samplers[0].MaxAnisotropy = 1;
		samplers[0].ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
		samplers[0].BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
		samplers[0].MinLOD = 0.0f;
		samplers[0].MaxLOD = D3D12_FLOAT32_MAX;
		samplers[0].ShaderRegister = 0;
		samplers[0].RegisterSpace = 0;
		samplers[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

		samplers[1].Filter = D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
		samplers[1].AddressU = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
		samplers[1].AddressV = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
		samplers[1].AddressW = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
		samplers[1].MipLODBias = 0.0f;
		samplers[1].MaxAnisotropy = 1;
		samplers[1].ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
		samplers[1].BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
		samplers[1].MinLOD = 0.0f;
		samplers[1].MaxLOD = D3D12_FLOAT32_MAX;
		samplers[1].ShaderRegister = 1;
		samplers[1].RegisterSpace = 0;
		samplers[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

		D3D12_ROOT_SIGNATURE_DESC rs{};
		rs.NumParameters = _countof(params);
		rs.pParameters = params;
		rs.NumStaticSamplers = _countof(samplers);
		rs.pStaticSamplers = samplers;
		rs.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

		ComPtr<ID3DBlob> blob, err;
		HRESULT hr = D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err);
		if (FAILED(hr))
		{
			OutputDebugStringA("[VolumetricFog] D3D12SerializeRootSignature failed:\n");
			if (err && err->GetBufferPointer())
			{
				OutputDebugStringA((const char*)err->GetBufferPointer());
			}
			assert(false && "VolumetricFog root signature");
			return;
		}
		hr = Device()->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
			IID_PPV_ARGS(&m_RootSignature));
		assert(SUCCEEDED(hr));
	}

	m_PSOAttributes = CreateComputePipeline("Shader/cso/VolumetricFogAttributes_CS.cso");
	m_PSOLightScattering = CreateComputePipeline("Shader/cso/VolumetricFogLightScattering_CS.cso");
	m_PSOIntegration = CreateComputePipeline("Shader/cso/VolumetricFogIntegration_CS.cso");

	// ボクセル化 (RTV ヒープは CreateVolumes が使うので先に)
	InitVoxelization();

	// ------------------------------------------------------------
	//  ボリュームテクスチャ (バックバッファ解像度で開始。レンダー解像度が
	//  変わると FSceneRenderer::ResizeRenderTargets が ReleaseVolumes / CreateVolumes で作り直す)
	// ------------------------------------------------------------
	CreateVolumes((unsigned int)m_Owner->GetBackBufferWidth(), (unsigned int)m_Owner->GetBackBufferHeight());

	// ------------------------------------------------------------
	//  b0 アップロードバッファ x2 (FVolumetricFogParams, 256 アライン)
	// ------------------------------------------------------------
	{
		D3D12_HEAP_PROPERTIES prop{};
		prop.Type = D3D12_HEAP_TYPE_UPLOAD;

		D3D12_RESOURCE_DESC d{};
		d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		d.Width = (sizeof(FVolumetricFogParams) + 255) & ~255u;
		d.Height = 1;
		d.DepthOrArraySize = 1;
		d.MipLevels = 1;
		d.Format = DXGI_FORMAT_UNKNOWN;
		d.SampleDesc.Count = 1;
		d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

		for (int i = 0; i < 2; ++i)
		{
			HRESULT hr = Device()->CreateCommittedResource(&prop, D3D12_HEAP_FLAG_NONE,
				&d, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
				IID_PPV_ARGS(&m_ParamBuffer[i]));
			assert(SUCCEEDED(hr));
			m_ParamBuffer[i]->SetName(L"VolumetricFogParams");
			m_ParamBuffer[i]->Map(0, nullptr, &m_ParamPtr[i]);
		}
	}
}

void FVolumetricFog::Dispatch(const FVolumetricFogInputs& Inputs)
{
	m_Stats.bDispatchedThisFrame = false;

	const FExponentialHeightFogSceneInfo* fog = Inputs.FogInfo;
	if (fog == nullptr || !fog->bEnableVolumetricFog || Inputs.View == nullptr
		|| m_RootSignature == nullptr || m_PSOAttributes == nullptr)
	{
		// 無効フレーム: 履歴は捨てる (次に有効化されたときに古い蓄積を混ぜない)。
		// t34 は無効時もバインドされる (シェーダ側で参照しない) ので、
		// 初期状態 (UAV) のままにせずピクセル読み取り状態へ揃えておく。
		m_bHistoryValid = false;
		m_Stats.NumVoxelizedElements = 0;
		m_Stats.NumVoxelizeDraws = 0;
		Transition(m_IntegratedLightScattering, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		return;
	}

	ID3D12GraphicsCommandList* cl = CommandList();
	ID3D12DescriptorHeap* heap = m_Owner->GetSRVDescriptorHeap();
	cl->SetDescriptorHeaps(1, &heap);

	const VIEW_CONSTANT& view = *Inputs.View;
	FVolumetricFog::FVolumeTexture& scatterWrite = m_LightScattering[m_LightScatteringFrame];
	FVolumetricFog::FVolumeTexture& scatterHistory = m_LightScattering[m_LightScatteringFrame ^ 1];

	// ---- 外部リソース (シャドウマップ / ライトグリッド) を NON_PIXEL へ ----
	// 常在状態は PIXEL_SHADER_RESOURCE。Dispatch 後に必ず戻す。
	std::vector<D3D12_RESOURCE_BARRIER> toCompute;
	std::vector<D3D12_RESOURCE_BARRIER> fromCompute;
	auto borrow = [&](ID3D12Resource* resource)
		{
			if (resource == nullptr) return;
			toCompute.push_back(CD3DX12_RESOURCE_BARRIER::Transition(resource,
				D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
				D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
			fromCompute.push_back(CD3DX12_RESOURCE_BARRIER::Transition(resource,
				D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
				D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE));
		};

	if (Inputs.ShadowRenderer)
	{
		borrow(Inputs.ShadowRenderer->GetCSMResource());
		borrow(Inputs.ShadowRenderer->GetLocalShadowResource());
	}
	const bool bLightGridReady = (Inputs.LightGrid != nullptr) && Inputs.LightGrid->AreOutputsInSRVState();
	if (bLightGridReady)
	{
		borrow(Inputs.LightGrid->GetNumCulledLightsGridResource());
		borrow(Inputs.LightGrid->GetCulledLightDataGridResource());
	}
	if (!toCompute.empty())
	{
		cl->ResourceBarrier((UINT)toCompute.size(), toCompute.data());
	}

	// ---- b0 パラメータ (ダブルバッファをフリップして書く) ----
	m_ParamFrame ^= 1;

	FVolumetricFogParams p{};

	// ビュー -> ワールド (VIEW 定数は転置済みなので戻してから逆行列、再び転置)
	{
		XMMATRIX viewM = XMMatrixTranspose(XMLoadFloat4x4(&view.View));
		XMMATRIX invView = XMMatrixInverse(nullptr, viewM);
		XMStoreFloat4x4(&p.ViewToWorld, XMMatrixTranspose(invView));
	}
	p.PrevWorldToClip = Inputs.PrevViewProjectionT;

	// シャドウ (b5 の内容をコピー)
	if (Inputs.ShadowRenderer)
	{
		const DIRECTIONAL_SHADOW_CONSTANT& shadow = Inputs.ShadowRenderer->GetDirectionalShadowConstant();
		for (unsigned int i = 0; i < MAX_SHADOW_CASCADES; ++i)
		{
			p.WorldToShadowCascade[i] = shadow.WorldToShadowCascade[i];
		}
		p.CascadeSplits = shadow.CascadeSplits;
		p.CascadeDepthBias = shadow.CascadeDepthBias;
		p.DirectionalShadowParams = shadow.DirectionalShadowParams;
	}
	else
	{
		p.CascadeSplits = { 1.0e9f, 1.0e9f, 1.0e9f, 1.0e9f };
	}

	// グリッド
	const float nearPlane = view.NearFar.x;
	const float maxDistance = ComputeMaxDistance(nearPlane, fog->VolumetricFogDistance);
	const XMFLOAT3 gridZ = ComputeGridZParams(nearPlane, maxDistance);

	p.GridSize = { (float)m_GridSizeX, (float)m_GridSizeY, (float)m_GridSizeZ, (float)VOLUMETRIC_FOG_GRID_PIXEL_SIZE };
	p.GridZParams = { gridZ.x, gridZ.y, gridZ.z, 1.0f / (float)m_GridSizeZ };
	// ボリュームを作ったレンダー解像度 (SVPosition -> froxel の対応。バックバッファではない)
	const float screenW = (float)m_ViewWidth;
	const float screenH = (float)m_ViewHeight;
	p.ScreenSize = { screenW, screenH, 1.0f / screenW, 1.0f / screenH };
	// 対称透視射影の対角成分は転置の影響を受けない
	p.ProjectionParams = { 1.0f / view.Projection._11, 1.0f / view.Projection._22, nearPlane, maxDistance };
	p.CameraOrigin = view.WorldCameraOrigin;

	// テンポラルジッタ (Halton 2/3/5、8 フレーム周期)
	{
		const unsigned int n = (m_FrameNumber % 8u) + 1u;
		if (m_Params.bJitter)
		{
			p.FrameJitter = { Halton(n, 2), Halton(n, 3), Halton(n, 5), m_Params.HistoryWeight };
		}
		else
		{
			p.FrameJitter = { 0.5f, 0.5f, 0.5f, m_Params.HistoryWeight };
		}
	}
	const bool bTemporal = m_Params.bTemporalReprojection && Inputs.bHistoryValid && m_bHistoryValid;
	p.TemporalParams = { bTemporal ? 1.0f : 0.0f, bTemporal ? 1.0f : 0.0f,
		m_Params.InverseSquaredLightDistanceBiasScale, Inputs.Time };

	// ボクセル化: 境界の投影 (VIEW 定数は転置済みなのでそのまま)
	p.WorldToView = view.View;

	// フォグ
	p.FogDensityParams0 = { fog->FogData[0].Density, fog->FogData[0].HeightFalloff, fog->FogData[0].Height, fog->VolumetricFogExtinctionScale };
	p.FogDensityParams1 = { fog->FogData[1].Density, fog->FogData[1].HeightFalloff, fog->FogData[1].Height, fog->VolumetricFogScatteringDistribution };
	p.FogAlbedo = { fog->VolumetricFogAlbedo.x, fog->VolumetricFogAlbedo.y, fog->VolumetricFogAlbedo.z, fog->VolumetricFogStartDistance };
	p.FogEmissive = { fog->VolumetricFogEmissive.x, fog->VolumetricFogEmissive.y, fog->VolumetricFogEmissive.z, fog->VolumetricFogNearFadeInDistance };
	p.FogInscatteringColor = { fog->FogColor.x, fog->FogColor.y, fog->FogColor.z,
		fog->bOverrideLightColorsWithFogInscatteringColors ? 1.0f : 0.0f };
	p.DirectionalInscatteringColor = { fog->DirectionalInscatteringColor.x, fog->DirectionalInscatteringColor.y, fog->DirectionalInscatteringColor.z,
		fog->VolumetricFogStaticLightingScatteringIntensity };

	// ディレクショナルライト: b3 の「選択されたフォワードディレクショナルライト」1 灯。
	// 色には VolumetricScatteringIntensity を掛ける。bCastVolumetricShadow が偽なら
	// CSM を無効 (NumCascades = 0) にして影なしで散乱させる
	p.DirectionalLightDirection = { 0.0f, 1.0f, 0.0f, 0.0f };
	p.DirectionalLightColor = { 0.0f, 0.0f, 0.0f, 0.0f };
	if (Inputs.ForwardLightData && Inputs.ForwardLightData->HasDirectionalLight != 0u)
	{
		const FORWARD_LIGHT_CONSTANT& f = *Inputs.ForwardLightData;
		const float dirScale = f.DirectionalLightVolumetricScatteringIntensity;
		p.DirectionalLightDirection = { f.DirectionalLightDirection.x, f.DirectionalLightDirection.y, f.DirectionalLightDirection.z, 1.0f };
		p.DirectionalLightColor = { f.DirectionalLightColor.x * dirScale, f.DirectionalLightColor.y * dirScale, f.DirectionalLightColor.z * dirScale, 0.0f };

		if ((f.DirectionalLightFlags & LIGHT_FLAG_CAST_VOLUMETRIC_SHADOW) == 0u)
		{
			p.DirectionalShadowParams.x = 0.0f;
		}
	}

	// ライトグリッド (b3 と同値)
	if (Inputs.ForwardLightData)
	{
		const FORWARD_LIGHT_CONSTANT& f = *Inputs.ForwardLightData;
		p.NumLocalLights = f.NumLocalLights;
		p.CulledGridSizeX = f.CulledGridSizeX;
		p.CulledGridSizeY = f.CulledGridSizeY;
		p.CulledGridSizeZ = f.CulledGridSizeZ;
		p.LightGridPixelSizeShift = f.LightGridPixelSizeShift;
		p.MaxCulledLightsPerCell = f.MaxCulledLightsPerCell;
		p.bUseLightGrid = (bLightGridReady && f.bUseLightGrid != 0u) ? 1u : 0u;
		p.LightGridZParams = f.LightGridZParams;
	}
	std::memcpy(m_ParamPtr[m_ParamFrame], &p, sizeof(p));

	// ---- ルートシグネチャ + 共通バインド ----
	cl->SetComputeRootSignature(m_RootSignature.Get());
	cl->SetComputeRootConstantBufferView(0, m_ParamBuffer[m_ParamFrame]->GetGPUVirtualAddress());

	auto bindSRV = [&](unsigned int slot, unsigned int srvIndex)
		{
			cl->SetComputeRootDescriptorTable(1 + slot, m_Owner->GetGPUDescriptorHandle(srvIndex));
		};
	auto bindUAV = [&](unsigned int slot, unsigned int uavIndex)
		{
			cl->SetComputeRootDescriptorTable(1 + NUM_SRV_SLOTS + slot, m_Owner->GetGPUDescriptorHandle(uavIndex));
		};

	// 未使用スロットにも有効なデスクリプタを置く (ライトバッファで代用)
	const unsigned int lightSRV = Inputs.LightBufferSRVIndex;
	bindSRV(0, lightSRV);																					// t0
	bindSRV(1, Inputs.ShadowRenderer ? Inputs.ShadowRenderer->GetLocalShadowParamSRVIndex() : lightSRV);	// t1
	bindSRV(2, bLightGridReady ? Inputs.LightGrid->GetNumCulledLightsGridSRVIndex() : lightSRV);			// t2
	bindSRV(3, bLightGridReady ? Inputs.LightGrid->GetCulledLightDataGridSRVIndex() : lightSRV);			// t3
	bindSRV(4, Inputs.ShadowRenderer ? Inputs.ShadowRenderer->GetCSMSRVIndex() : lightSRV);				// t4
	bindSRV(5, Inputs.ShadowRenderer ? Inputs.ShadowRenderer->GetLocalShadowSRVIndex() : lightSRV);		// t5
	bindSRV(6, m_VBufferA.SRVIndex);																		// t6
	bindSRV(7, m_VBufferB.SRVIndex);																		// t7
	bindSRV(8, scatterHistory.SRVIndex);																	// t8
	bindSRV(9, scatterWrite.SRVIndex);																		// t9
	bindSRV(10, Inputs.SkyIrradianceSRVIndex);																// t10
	bindUAV(0, m_VBufferA.UAVIndex);																		// u0
	bindUAV(1, m_VBufferB.UAVIndex);																		// u1
	bindUAV(2, scatterWrite.UAVIndex);																		// u2
	bindUAV(3, m_IntegratedLightScattering.UAVIndex);														// u3

	const unsigned int TG = 4;	// VOLUMETRIC_FOG_THREADGROUP_SIZE (4x4x4)
	const UINT gx = (m_GridSizeX + TG - 1) / TG;
	const UINT gy = (m_GridSizeY + TG - 1) / TG;
	const UINT gz = (m_GridSizeZ + TG - 1) / TG;

	// ---- Pass 1 : 媒質属性 (VBufferA / VBufferB) ----
	{
		Transition(m_VBufferA, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		Transition(m_VBufferB, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

		cl->SetPipelineState(m_PSOAttributes.Get());
		cl->Dispatch(gx, gy, gz);

		Transition(m_VBufferA, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
		Transition(m_VBufferB, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
	}

	// ---- Pass 1b : Volume ドメインのプリミティブを VBufferA / VBufferB へ加算 ----
	//  (グラフィックス。コンピュートのルートシグネチャ / バインドはここで失われるので
	//   直後に積み直す)
	m_Stats.NumVoxelizedElements = 0;
	m_Stats.NumVoxelizeDraws = 0;
	if (Inputs.VolumeElements != nullptr && !Inputs.VolumeElements->empty() && m_PSOVoxelize != nullptr)
	{
		VoxelizeFogVolumePrimitives(Inputs.VolumeElements);

		// コンピュート側のバインドを復帰 (b0 / t0..t10 / u0..u3)
		cl->SetDescriptorHeaps(1, &heap);
		cl->SetComputeRootSignature(m_RootSignature.Get());
		cl->SetComputeRootConstantBufferView(0, m_ParamBuffer[m_ParamFrame]->GetGPUVirtualAddress());
		bindSRV(0, lightSRV);
		bindSRV(1, Inputs.ShadowRenderer ? Inputs.ShadowRenderer->GetLocalShadowParamSRVIndex() : lightSRV);
		bindSRV(2, bLightGridReady ? Inputs.LightGrid->GetNumCulledLightsGridSRVIndex() : lightSRV);
		bindSRV(3, bLightGridReady ? Inputs.LightGrid->GetCulledLightDataGridSRVIndex() : lightSRV);
		bindSRV(4, Inputs.ShadowRenderer ? Inputs.ShadowRenderer->GetCSMSRVIndex() : lightSRV);
		bindSRV(5, Inputs.ShadowRenderer ? Inputs.ShadowRenderer->GetLocalShadowSRVIndex() : lightSRV);
		bindSRV(6, m_VBufferA.SRVIndex);
		bindSRV(7, m_VBufferB.SRVIndex);
		bindSRV(8, scatterHistory.SRVIndex);
		bindSRV(9, scatterWrite.SRVIndex);
		bindSRV(10, Inputs.SkyIrradianceSRVIndex);
		bindUAV(0, m_VBufferA.UAVIndex);
		bindUAV(1, m_VBufferB.UAVIndex);
		bindUAV(2, scatterWrite.UAVIndex);
		bindUAV(3, m_IntegratedLightScattering.UAVIndex);
	}

	// ---- Pass 2 : ライト散乱 + テンポラル再投影 (LightScattering) ----
	{
		Transition(scatterWrite, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		Transition(scatterHistory, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

		cl->SetPipelineState(m_PSOLightScattering.Get());
		cl->Dispatch(gx, gy, gz);

		Transition(scatterWrite, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
	}

	// ---- Pass 3 : Z 積分 (IntegratedLightScattering, t34) ----
	{
		Transition(m_IntegratedLightScattering, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

		const unsigned int IG = 8;	// VOLUMETRIC_FOG_INTEGRATION_GROUP_SIZE (8x8)
		cl->SetPipelineState(m_PSOIntegration.Get());
		cl->Dispatch((m_GridSizeX + IG - 1) / IG, (m_GridSizeY + IG - 1) / IG, 1);

		// フォグパス (ピクセル) が t34 で読む
		Transition(m_IntegratedLightScattering, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	}

	// ---- 外部リソースを常在状態 (PIXEL_SHADER_RESOURCE) へ戻す ----
	if (!fromCompute.empty())
	{
		cl->ResourceBarrier((UINT)fromCompute.size(), fromCompute.data());
	}

	// 次フレーム: 今フレームの LightScattering が履歴になる
	m_LightScatteringFrame ^= 1;
	m_bHistoryValid = true;
	m_FrameNumber++;
	m_Stats.bDispatchedThisFrame = true;
}


// ============================================================
//  ボクセル化 (VoxelizeFogVolumePrimitives 相当)
//  Material Domain = Volume のプリミティブの境界を、属性パスの直後に
//  VBufferA / VBufferB へ加算ラスタライズする。
//  シェーダ: VolumetricFogVoxelizationVS / GS / PS
// ============================================================
void FVolumetricFog::InitVoxelization()
{
	// ------------------------------------------------------------
	//  グラフィックスルートシグネチャ: [0] CBV b0 / [1] CBV b1 (どちらもルート CBV)
	// ------------------------------------------------------------
	{
		D3D12_ROOT_PARAMETER params[2]{};
		for (unsigned int i = 0; i < 2; ++i)
		{
			params[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
			params[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
			params[i].Descriptor.ShaderRegister = i;
			params[i].Descriptor.RegisterSpace = 0;
		}

		D3D12_ROOT_SIGNATURE_DESC rs{};
		rs.NumParameters = _countof(params);
		rs.pParameters = params;
		rs.NumStaticSamplers = 0;
		rs.pStaticSamplers = nullptr;
		rs.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;	// 頂点入力無し

		ComPtr<ID3DBlob> blob, err;
		HRESULT hr = D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err);
		if (FAILED(hr))
		{
			OutputDebugStringA("[VolumetricFog] voxelization D3D12SerializeRootSignature failed:\n");
			if (err && err->GetBufferPointer())
			{
				OutputDebugStringA((const char*)err->GetBufferPointer());
			}
			assert(false && "VolumetricFog voxelization root signature");
			return;
		}
		hr = Device()->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
			IID_PPV_ARGS(&m_VoxelizeRootSignature));
		assert(SUCCEEDED(hr));
	}

	// ------------------------------------------------------------
	//  PSO: VS + GS + PS、点リスト (1 点 = 1 要素、インスタンス = スライス 8 枚のパス)、
	//  加算ブレンド (One / One) x 2 RT (RGBA16F)、カリング無し、深度無し
	// ------------------------------------------------------------
	{
		std::vector<char> vs, gs, ps;
		if (!ReadShaderBytecode("Shader/cso/VolumetricFogVoxelizationVS.cso", vs)
			|| !ReadShaderBytecode("Shader/cso/VolumetricFogVoxelizationGS.cso", gs)
			|| !ReadShaderBytecode("Shader/cso/VolumetricFogVoxelizationPS.cso", ps))
		{
			OutputDebugStringA("[VolumetricFog] voxelization shaders missing: volume materials are disabled\n");
			return;
		}

		D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
		desc.pRootSignature = m_VoxelizeRootSignature.Get();
		desc.VS = { vs.data(), vs.size() };
		desc.GS = { gs.data(), gs.size() };
		desc.PS = { ps.data(), ps.size() };

		desc.BlendState.AlphaToCoverageEnable = FALSE;
		desc.BlendState.IndependentBlendEnable = FALSE;
		D3D12_RENDER_TARGET_BLEND_DESC& rt = desc.BlendState.RenderTarget[0];
		rt.BlendEnable = TRUE;
		rt.LogicOpEnable = FALSE;
		rt.SrcBlend = D3D12_BLEND_ONE;
		rt.DestBlend = D3D12_BLEND_ONE;
		rt.BlendOp = D3D12_BLEND_OP_ADD;
		rt.SrcBlendAlpha = D3D12_BLEND_ONE;
		rt.DestBlendAlpha = D3D12_BLEND_ONE;
		rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
		rt.LogicOp = D3D12_LOGIC_OP_NOOP;
		rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
		desc.BlendState.RenderTarget[1] = rt;

		desc.SampleMask = UINT_MAX;

		desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
		desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
		desc.RasterizerState.FrontCounterClockwise = FALSE;
		desc.RasterizerState.DepthClipEnable = FALSE;
		desc.RasterizerState.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;

		desc.DepthStencilState.DepthEnable = FALSE;
		desc.DepthStencilState.StencilEnable = FALSE;

		desc.InputLayout = { nullptr, 0 };
		desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
		desc.NumRenderTargets = 2;
		desc.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
		desc.RTVFormats[1] = DXGI_FORMAT_R16G16B16A16_FLOAT;
		desc.DSVFormat = DXGI_FORMAT_UNKNOWN;
		desc.SampleDesc.Count = 1;
		desc.SampleDesc.Quality = 0;

		HRESULT hr = Device()->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&m_PSOVoxelize));
		if (FAILED(hr))
		{
			OutputDebugStringA("[VolumetricFog] voxelization PSO creation failed: volume materials are disabled\n");
			m_PSOVoxelize.Reset();
			return;
		}
	}

	// ------------------------------------------------------------
	//  RTV ヒープ (2 枠。RTV は CreateVolumes がボリューム生成のたびに作り直す)
	// ------------------------------------------------------------
	{
		D3D12_DESCRIPTOR_HEAP_DESC hd{};
		hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
		hd.NumDescriptors = 2;
		hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
		HRESULT hr = Device()->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&m_VoxelizeRTVHeap));
		assert(SUCCEEDED(hr));
		m_VoxelizeRTVHeap->SetName(L"VolumetricFogVoxelizationRTVHeap");

		const UINT inc = Device()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
		D3D12_CPU_DESCRIPTOR_HANDLE base = m_VoxelizeRTVHeap->GetCPUDescriptorHandleForHeapStart();
		for (int i = 0; i < 2; ++i)
		{
			m_VoxelizeRTV[i] = base;
			m_VoxelizeRTV[i].ptr += (SIZE_T)inc * i;
		}
	}

	// ------------------------------------------------------------
	//  b1 リング (要素 x VOXELIZE_ELEMENT_STRIDE B x 2 フレーム。アップロードヒープ、常時マップ)
	// ------------------------------------------------------------
	{
		D3D12_HEAP_PROPERTIES prop{};
		prop.Type = D3D12_HEAP_TYPE_UPLOAD;

		D3D12_RESOURCE_DESC d{};
		d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		d.Width = (UINT64)VOXELIZE_ELEMENT_STRIDE * MAX_VOXELIZE_VOLUME_ELEMENTS;
		d.Height = 1;
		d.DepthOrArraySize = 1;
		d.MipLevels = 1;
		d.Format = DXGI_FORMAT_UNKNOWN;
		d.SampleDesc.Count = 1;
		d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

		for (int i = 0; i < 2; ++i)
		{
			HRESULT hr = Device()->CreateCommittedResource(&prop, D3D12_HEAP_FLAG_NONE,
				&d, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
				IID_PPV_ARGS(&m_ElementBuffer[i]));
			assert(SUCCEEDED(hr));
			m_ElementBuffer[i]->SetName(L"VolumetricFogVoxelizeElements");
			m_ElementBuffer[i]->Map(0, nullptr, &m_ElementPtr[i]);
		}
	}
}

void FVolumetricFog::VoxelizeFogVolumePrimitives(const std::vector<FVoxelizeVolumeElement>* Elements)
{
	if (Elements == nullptr || Elements->empty()
		|| m_PSOVoxelize == nullptr || m_VoxelizeRootSignature == nullptr
		|| m_VoxelizeRTVHeap == nullptr || m_ElementPtr[m_ParamFrame] == nullptr)
	{
		return;
	}

	ID3D12GraphicsCommandList* cl = CommandList();

	// ---- 要素定数 (b1 リング) ----
	const unsigned int numElements = (std::min)((unsigned int)Elements->size(), MAX_VOXELIZE_VOLUME_ELEMENTS);
	char* ring = (char*)m_ElementPtr[m_ParamFrame];
	for (unsigned int i = 0; i < numElements; ++i)
	{
		const FVoxelizeVolumeElement& e = (*Elements)[i];
		FVoxelizeVolumeElementParams params{};
		XMStoreFloat4x4(&params.BoxToWorld, XMMatrixTranspose(XMLoadFloat4x4(&e.BoxToWorld)));
		XMStoreFloat4x4(&params.WorldToBox, XMMatrixTranspose(XMLoadFloat4x4(&e.WorldToBox)));
		params.BoundsSphere = { e.BoundsSphere.Center.x, e.BoundsSphere.Center.y, e.BoundsSphere.Center.z, e.BoundsSphere.W };
		params.Material = e.Material;
		std::memcpy(ring + (size_t)VOXELIZE_ELEMENT_STRIDE * i, &params, sizeof(params));
	}

	// ---- VBufferA / VBufferB を RTV へ ----
	Transition(m_VBufferA, D3D12_RESOURCE_STATE_RENDER_TARGET);
	Transition(m_VBufferB, D3D12_RESOURCE_STATE_RENDER_TARGET);

	// ---- グラフィックス状態 (ビューポート = froxel グリッド、深度無し) ----
	cl->OMSetRenderTargets(2, m_VoxelizeRTV, FALSE, nullptr);

	D3D12_VIEWPORT vp{};
	vp.TopLeftX = 0.0f;
	vp.TopLeftY = 0.0f;
	vp.Width = (float)m_GridSizeX;
	vp.Height = (float)m_GridSizeY;
	vp.MinDepth = 0.0f;
	vp.MaxDepth = 1.0f;
	cl->RSSetViewports(1, &vp);

	D3D12_RECT scissor{ 0, 0, (LONG)m_GridSizeX, (LONG)m_GridSizeY };
	cl->RSSetScissorRects(1, &scissor);

	cl->SetGraphicsRootSignature(m_VoxelizeRootSignature.Get());
	cl->SetPipelineState(m_PSOVoxelize.Get());
	cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_POINTLIST);
	cl->SetGraphicsRootConstantBufferView(0, m_ParamBuffer[m_ParamFrame]->GetGPUVirtualAddress());

	// 1 要素 = 1 点 x (GridSizeZ / 8) インスタンス (GS がインスタンスごとに 8 スライス分のクアッドを出す)
	const UINT numPasses = (m_GridSizeZ + 7u) / 8u;	// MAX_SLICES_PER_VOXELIZATION_PASS
	const D3D12_GPU_VIRTUAL_ADDRESS ringBase = m_ElementBuffer[m_ParamFrame]->GetGPUVirtualAddress();
	for (unsigned int i = 0; i < numElements; ++i)
	{
		cl->SetGraphicsRootConstantBufferView(1, ringBase + (UINT64)VOXELIZE_ELEMENT_STRIDE * i);
		cl->DrawInstanced(1, numPasses, 0, 0);
	}

	// ---- 散乱パス (コンピュート) が読める状態へ ----
	Transition(m_VBufferA, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
	Transition(m_VBufferB, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

	// RTV を外し、既定のグラフィックス状態 (RS / ビューポート / ヒープ) へ戻す
	cl->OMSetRenderTargets(0, nullptr, FALSE, nullptr);
	m_Owner->RestoreDefaultGraphicsState();

	m_Stats.NumVoxelizedElements = numElements;
	m_Stats.NumVoxelizeDraws = numElements;
}
