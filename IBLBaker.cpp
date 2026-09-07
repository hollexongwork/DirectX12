#include "Main.h"
#include "RenderManager.h"
#include "IBLBaker.h"
#include "D3DX12.h"

// ============================================================
//  IBLBaker : split-sum IBL プリコンピュート実装
//  RenderManager から分離。デバイス・コマンドリスト・SRVヒープ
//  確保・即時フラッシュは RenderManager のアクセサ経由。
// ============================================================

IBLBaker::IBLBaker(RenderManager* owner)
	: m_Owner(owner)
{
}

IBLBaker::~IBLBaker()
{
}

ID3D12Device* IBLBaker::Device()
{
	return m_Owner->GetDevice();
}

ID3D12GraphicsCommandList* IBLBaker::CommandList()
{
	return m_Owner->GetGraphicsCommandList();
}

// --- 共通: キューブリソース生成（UAV書き込み可・ミップ付） ---
ComPtr<ID3D12Resource> IBLBaker::CreateCubeResource(unsigned int size, unsigned int mips, DXGI_FORMAT fmt)
{
	D3D12_HEAP_PROPERTIES prop{};
	prop.Type = D3D12_HEAP_TYPE_DEFAULT;

	D3D12_RESOURCE_DESC desc{};
	desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	desc.Width = size;
	desc.Height = size;
	desc.DepthOrArraySize = 6;          // cubemap = 6 faces
	desc.MipLevels = (UINT16)mips;
	desc.Format = fmt;
	desc.SampleDesc.Count = 1;
	desc.SampleDesc.Quality = 0;
	desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
	desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

	ComPtr<ID3D12Resource> res;
	HRESULT hr = Device()->CreateCommittedResource(
		&prop, D3D12_HEAP_FLAG_NONE, &desc,
		D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
		IID_PPV_ARGS(&res));
	assert(SUCCEEDED(hr));
	return res;
}

// --- UAV: cubeをTexture2DArray(6)として特定ミップに書く ---
unsigned int IBLBaker::CreateCubeUAV(ID3D12Resource* res, unsigned int mip)
{
	unsigned int index = m_Owner->AllocateDescriptor();
	D3D12_CPU_DESCRIPTOR_HANDLE handle = m_Owner->GetCPUDescriptorHandle(index);

	D3D12_RESOURCE_DESC rd = res->GetDesc();

	D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
	uav.Format = rd.Format;
	uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
	uav.Texture2DArray.MipSlice = mip;
	uav.Texture2DArray.FirstArraySlice = 0;
	uav.Texture2DArray.ArraySize = 6;
	uav.Texture2DArray.PlaneSlice = 0;

	Device()->CreateUnorderedAccessView(res, nullptr, &uav, handle);
	return index;
}

// --- SRV: cubemapとしてサンプル ---
unsigned int IBLBaker::CreateCubeSRV(ID3D12Resource* res, unsigned int mipLevels)
{
	unsigned int index = m_Owner->AllocateDescriptor();
	D3D12_CPU_DESCRIPTOR_HANDLE handle = m_Owner->GetCPUDescriptorHandle(index);

	D3D12_RESOURCE_DESC rd = res->GetDesc();

	D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
	srv.Format = rd.Format;
	srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
	srv.TextureCube.MostDetailedMip = 0;
	srv.TextureCube.MipLevels = mipLevels;
	srv.TextureCube.ResourceMinLODClamp = 0.0f;

	Device()->CreateShaderResourceView(res, &srv, handle);
	return index;
}

unsigned int IBLBaker::CreateTex2DUAV(ID3D12Resource* res, DXGI_FORMAT fmt)
{
	unsigned int index = m_Owner->AllocateDescriptor();
	D3D12_CPU_DESCRIPTOR_HANDLE handle = m_Owner->GetCPUDescriptorHandle(index);

	D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
	uav.Format = fmt;
	uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
	uav.Texture2D.MipSlice = 0;
	uav.Texture2D.PlaneSlice = 0;

	Device()->CreateUnorderedAccessView(res, nullptr, &uav, handle);
	return index;
}

// --- SRV: 2D RG16F (BRDF LUT 読み出し用) ---
unsigned int IBLBaker::CreateTex2DSRV_RG(ID3D12Resource* res)
{
	unsigned int index = m_Owner->AllocateDescriptor();
	D3D12_CPU_DESCRIPTOR_HANDLE handle = m_Owner->GetCPUDescriptorHandle(index);

	D3D12_RESOURCE_DESC rd = res->GetDesc();

	D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
	srv.Format = rd.Format;
	srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	srv.Texture2D.MipLevels = 1;
	srv.Texture2D.MostDetailedMip = 0;

	Device()->CreateShaderResourceView(res, &srv, handle);
	return index;
}

// --- compute PSO 生成 ---
ComPtr<ID3D12PipelineState> IBLBaker::CreateComputePipeline(const char* csoFile)
{
	std::vector<char> cs;
	{
		std::ifstream file(csoFile, std::ios_base::in | std::ios_base::binary);
		assert(file);
		file.seekg(0, std::ios_base::end);
		int filesize = (int)file.tellg();
		file.seekg(0, std::ios_base::beg);
		cs.resize(filesize);
		file.read(&cs[0], filesize);
		file.close();
	}

	D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
	desc.pRootSignature = m_ComputeRootSignature.Get();
	desc.CS.pShaderBytecode = cs.data();
	desc.CS.BytecodeLength = cs.size();

	ComPtr<ID3D12PipelineState> pso;
	HRESULT hr = Device()->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pso));
	assert(SUCCEEDED(hr));
	return pso;
}

// ============================================================
//  InitIBL : compute ルートシグネチャ・PSO・リソース生成
// ============================================================
void IBLBaker::Init()
{
	// ---- compute ルートシグネチャ ----
	// [0] CBV b0 (BakeParams)
	// [1] SRV table t0 (入力: equirect or env cube)
	// [2] UAV table u0 (出力)
	// static sampler s0 (linear)
	{
		D3D12_DESCRIPTOR_RANGE rangeSRV{};
		rangeSRV.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
		rangeSRV.NumDescriptors = 1;
		rangeSRV.BaseShaderRegister = 0; // t0
		rangeSRV.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

		D3D12_DESCRIPTOR_RANGE rangeUAV{};
		rangeUAV.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
		rangeUAV.NumDescriptors = 1;
		rangeUAV.BaseShaderRegister = 0; // u0
		rangeUAV.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

		D3D12_DESCRIPTOR_RANGE rangeUAV1{};
		rangeUAV1.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
		rangeUAV1.NumDescriptors = 1;
		rangeUAV1.BaseShaderRegister = 1; // u1 (downsample親mip読み取り)
		rangeUAV1.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

		D3D12_ROOT_PARAMETER params[4]{};
		// b0 : CBV (root descriptor)
		params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
		params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
		params[0].Descriptor.ShaderRegister = 0;
		params[0].Descriptor.RegisterSpace = 0;
		// t0 : SRV table
		params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
		params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
		params[1].DescriptorTable.NumDescriptorRanges = 1;
		params[1].DescriptorTable.pDescriptorRanges = &rangeSRV;
		// u0 : UAV table
		params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
		params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
		params[2].DescriptorTable.NumDescriptorRanges = 1;
		params[2].DescriptorTable.pDescriptorRanges = &rangeUAV;
		// u1 : UAV table (downsample用)
		params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
		params[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
		params[3].DescriptorTable.NumDescriptorRanges = 1;
		params[3].DescriptorTable.pDescriptorRanges = &rangeUAV1;

		D3D12_STATIC_SAMPLER_DESC samp{};
		samp.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
		samp.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
		samp.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
		samp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
		samp.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
		samp.BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
		samp.MinLOD = 0.0f;
		samp.MaxLOD = D3D12_FLOAT32_MAX;
		samp.ShaderRegister = 0;
		samp.RegisterSpace = 0;
		samp.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

		D3D12_ROOT_SIGNATURE_DESC rs{};
		rs.NumParameters = _countof(params);
		rs.pParameters = params;
		rs.NumStaticSamplers = 1;
		rs.pStaticSamplers = &samp;
		rs.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

		ComPtr<ID3DBlob> blob, err;
		HRESULT hr = D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err);
		assert(SUCCEEDED(hr));
		hr = Device()->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
			IID_PPV_ARGS(&m_ComputeRootSignature));
		assert(SUCCEEDED(hr));
	}

	// ---- compute PSO ----
	m_PSOEquirectToCube = CreateComputePipeline("Shader/cso/IBL_EquirectToCube.cso");
	m_PSODownsampleCube = CreateComputePipeline("Shader/cso/IBL_DownsampleCube.cso");
	m_PSOIrradiance = CreateComputePipeline("Shader/cso/IBL_Irradiance.cso");
	m_PSOPrefilter = CreateComputePipeline("Shader/cso/IBL_Prefilter.cso");
	m_PSOBRDFLut = CreateComputePipeline("Shader/cso/IBL_BRDFLut.cso");

	// ---- リソース生成 ----
	const DXGI_FORMAT HDRFmt = DXGI_FORMAT_R16G16B16A16_FLOAT;

	m_EnvCube.Resource = CreateCubeResource(ENV_CUBE_SIZE, ENV_CUBE_MIP_COUNT, HDRFmt);
	m_IrradianceCube.Resource = CreateCubeResource(IRRADIANCE_SIZE, 1, HDRFmt);
	m_PrefilterCube.Resource = CreateCubeResource(PREFILTER_SIZE, PREFILTER_MIP_COUNT, HDRFmt);

	m_EnvCube.Resource->SetName(L"IBL_EnvCube");
	m_IrradianceCube.Resource->SetName(L"IBL_IrradianceCube");
	m_PrefilterCube.Resource->SetName(L"IBL_PrefilterCube");

	// cubemap SRV (Deferred で使う)
	m_EnvCube.SRVIndex = CreateCubeSRV(m_EnvCube.Resource.Get(), ENV_CUBE_MIP_COUNT);
	m_IrradianceCube.SRVIndex = CreateCubeSRV(m_IrradianceCube.Resource.Get(), 1);
	m_PrefilterCube.SRVIndex = CreateCubeSRV(m_PrefilterCube.Resource.Get(), PREFILTER_MIP_COUNT);

	m_EnvCube.SRVHandle = m_Owner->GetGPUDescriptorHandle(m_EnvCube.SRVIndex);
	m_IrradianceCube.SRVHandle = m_Owner->GetGPUDescriptorHandle(m_IrradianceCube.SRVIndex);
	m_PrefilterCube.SRVHandle = m_Owner->GetGPUDescriptorHandle(m_PrefilterCube.SRVIndex);

	// BRDF LUT (RG16F 2D)
	{
		D3D12_HEAP_PROPERTIES prop{};
		prop.Type = D3D12_HEAP_TYPE_DEFAULT;

		D3D12_RESOURCE_DESC desc{};
		desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
		desc.Width = BRDF_LUT_SIZE;
		desc.Height = BRDF_LUT_SIZE;
		desc.DepthOrArraySize = 1;
		desc.MipLevels = 1;
		desc.Format = DXGI_FORMAT_R16G16_FLOAT;
		desc.SampleDesc.Count = 1;
		desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
		desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

		HRESULT hr = Device()->CreateCommittedResource(
			&prop, D3D12_HEAP_FLAG_NONE, &desc,
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
			IID_PPV_ARGS(&m_BRDFLut));
		assert(SUCCEEDED(hr));
		m_BRDFLut->SetName(L"IBL_BRDFLut");

		m_BRDFLutSRVIndex = CreateTex2DSRV_RG(m_BRDFLut.Get());
		m_BRDFLutSRVHandle = m_Owner->GetGPUDescriptorHandle(m_BRDFLutSRVIndex);
	}
}

// ベイク用定数バッファ構造体（HLSL BakeParams と一致, 16byte境界）
struct IBL_BAKE_PARAMS
{
	unsigned int FaceSize;
	unsigned int MipLevel;
	float        Roughness;
	float        _pad;
};

// ============================================================
//  BakeIBL : 起動時に一度だけ全ベイクを実行
//  ※ Init() 内で呼ばれ、開いている m_GraphicsCommandList に
//    積んだ後 Close/Execute/Wait して即時完了させる
// ============================================================
void IBLBaker::Bake(ID3D12Resource* EquirectResource, unsigned int equirectSRVIndex)
{
	ID3D12GraphicsCommandList* cl = CommandList();

	// SRVヒープ設定（compute でも同じ shader-visible heap を使う）
	ID3D12DescriptorHeap* dh[] = { m_Owner->GetSRVDescriptorHeap() };
	cl->SetDescriptorHeaps(_countof(dh), dh);
	cl->SetComputeRootSignature(m_ComputeRootSignature.Get());

	auto uavBarrier = [&](ID3D12Resource* r)
		{
			D3D12_RESOURCE_BARRIER b{};
			b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
			b.UAV.pResource = r;
			cl->ResourceBarrier(1, &b);
		};

	// ベイク用定数バッファ（小さなUPLOADを一つ確保し、各dispatchで詰め直す）
	// 簡便のため毎dispatchで別領域を使い回す: m_ConstantBuffer をそのまま利用
	auto setBakeParams = [&](unsigned int faceSize, unsigned int mip, float rough)
		{
			IBL_BAKE_PARAMS p{ faceSize, mip, rough, 0.0f };
			// 専用に小さなUPLOADバッファを作って毎回入れる（衝突回避）
			static ComPtr<ID3D12Resource> s_cb[64];
			static int s_cbIndex = 0;
			int idx = s_cbIndex++ % 64;

			if (!s_cb[idx])
			{
				D3D12_HEAP_PROPERTIES prop{};
				prop.Type = D3D12_HEAP_TYPE_UPLOAD;
				D3D12_RESOURCE_DESC d{};
				d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
				d.Width = 256;
				d.Height = 1;
				d.DepthOrArraySize = 1;
				d.MipLevels = 1;
				d.Format = DXGI_FORMAT_UNKNOWN;
				d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
				d.SampleDesc.Count = 1;
				Device()->CreateCommittedResource(&prop, D3D12_HEAP_FLAG_NONE, &d,
					D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&s_cb[idx]));
			}
			void* ptr = nullptr;
			s_cb[idx]->Map(0, nullptr, &ptr);
			memcpy(ptr, &p, sizeof(p));
			s_cb[idx]->Unmap(0, nullptr);

			cl->SetComputeRootConstantBufferView(0, s_cb[idx]->GetGPUVirtualAddress());
		};

	const unsigned int GROUP = 8;

	// 実行完了まで解放してはいけない一時ディスクリプタを溜める
	std::vector<unsigned int> tempDescriptors;

	// ============================================================
	// 1) Equirect -> EnvCube (mip0)
	// ============================================================

	// equirect は LoadTexture で PIXEL_SHADER_RESOURCE 状態。
	// compute(non-pixel)から読むため NON_PIXEL に遷移。
	cl->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(
		EquirectResource,
		D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
		D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));

	{
		// 入力: equirect SRV (m_EnvironmentTexture->SRVIndex)
		// 出力: EnvCube mip0 UAV
		unsigned int srcSRV = equirectSRVIndex;
		unsigned int dstUAV = CreateCubeUAV(m_EnvCube.Resource.Get(), 0);

		cl->SetPipelineState(m_PSOEquirectToCube.Get());
		setBakeParams(ENV_CUBE_SIZE, 0, 0.0f);
		cl->SetComputeRootDescriptorTable(1, m_Owner->GetGPUDescriptorHandle(srcSRV));
		cl->SetComputeRootDescriptorTable(2, m_Owner->GetGPUDescriptorHandle(dstUAV));

		unsigned int groups = (ENV_CUBE_SIZE + GROUP - 1) / GROUP;
		cl->Dispatch(groups, groups, 6);
		uavBarrier(m_EnvCube.Resource.Get());

		tempDescriptors.push_back(dstUAV);
	}

	// ------------------------------------------------------------
	// 1.5) EnvCube の mip チェーン生成 (box downsample, UAVベース)
	//      prefilter の importance sampling が高 mip を参照するため必須
	// ------------------------------------------------------------
	{
		cl->SetPipelineState(m_PSODownsampleCube.Get());

		for (unsigned int mip = 1; mip < ENV_CUBE_MIP_COUNT; ++mip)
		{
			unsigned int dstSize = ENV_CUBE_SIZE >> mip;
			if (dstSize < 1) dstSize = 1;

			unsigned int srcUAV = CreateCubeUAV(m_EnvCube.Resource.Get(), mip - 1); // 親mip
			unsigned int dstUAV = CreateCubeUAV(m_EnvCube.Resource.Get(), mip);     // 子mip

			setBakeParams(dstSize, mip, 0.0f);
			cl->SetComputeRootDescriptorTable(2, m_Owner->GetGPUDescriptorHandle(dstUAV)); // u0
			cl->SetComputeRootDescriptorTable(3, m_Owner->GetGPUDescriptorHandle(srcUAV)); // u1

			unsigned int g = (dstSize + GROUP - 1) / GROUP;
			cl->Dispatch(g, g, 6);
			uavBarrier(m_EnvCube.Resource.Get());

			tempDescriptors.push_back(srcUAV);
			tempDescriptors.push_back(dstUAV);
		}
	}

	// EnvCube を UAV -> 非PS-SRV に遷移してサンプル可能に
	cl->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(
		m_EnvCube.Resource.Get(),
		D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
		D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));

	// ============================================================
	// 2) Irradiance 畳み込み (EnvCube -> IrradianceCube)
	// ============================================================
	{
		unsigned int dstUAV = CreateCubeUAV(m_IrradianceCube.Resource.Get(), 0);

		cl->SetPipelineState(m_PSOIrradiance.Get());
		setBakeParams(IRRADIANCE_SIZE, 0, 0.0f);
		cl->SetComputeRootDescriptorTable(1, m_EnvCube.SRVHandle);
		cl->SetComputeRootDescriptorTable(2, m_Owner->GetGPUDescriptorHandle(dstUAV));

		unsigned int groups = (IRRADIANCE_SIZE + GROUP - 1) / GROUP;
		cl->Dispatch(groups, groups, 6);
		uavBarrier(m_IrradianceCube.Resource.Get());

		tempDescriptors.push_back(dstUAV);
	}

	// ============================================================
	// 3) Prefilter (EnvCube -> PrefilterCube, mipごとにroughness変化)
	// ============================================================
	{
		cl->SetPipelineState(m_PSOPrefilter.Get());
		cl->SetComputeRootDescriptorTable(1, m_EnvCube.SRVHandle);

		for (unsigned int mip = 0; mip < PREFILTER_MIP_COUNT; ++mip)
		{
			unsigned int mipSize = PREFILTER_SIZE >> mip;
			if (mipSize < 1) mipSize = 1;
			float roughness = (float)mip / (float)(PREFILTER_MIP_COUNT - 1);

			unsigned int dstUAV = CreateCubeUAV(m_PrefilterCube.Resource.Get(), mip);

			setBakeParams(mipSize, mip, roughness);
			cl->SetComputeRootDescriptorTable(2, m_Owner->GetGPUDescriptorHandle(dstUAV));

			unsigned int groups = (mipSize + GROUP - 1) / GROUP;
			cl->Dispatch(groups, groups, 6);
			uavBarrier(m_PrefilterCube.Resource.Get());

			tempDescriptors.push_back(dstUAV);
		}
	}

	// ============================================================
	// 4) BRDF LUT (一度きり)
	// ============================================================
	{
		unsigned int dstUAV = CreateTex2DUAV(m_BRDFLut.Get(), DXGI_FORMAT_R16G16_FLOAT);

		cl->SetPipelineState(m_PSOBRDFLut.Get());
		setBakeParams(BRDF_LUT_SIZE, 0, 0.0f);
		// t0 は未使用だが table は要設定 -> ダミーで env cube を入れておく
		cl->SetComputeRootDescriptorTable(1, m_EnvCube.SRVHandle);
		cl->SetComputeRootDescriptorTable(2, m_Owner->GetGPUDescriptorHandle(dstUAV));

		unsigned int groups = (BRDF_LUT_SIZE + GROUP - 1) / GROUP;
		cl->Dispatch(groups, groups, 1);
		uavBarrier(m_BRDFLut.Get());

		tempDescriptors.push_back(dstUAV);
	}

	// ============================================================
	// 最終遷移: 全て PS から読める状態へ
	// ============================================================
	cl->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(
		m_EnvCube.Resource.Get(),
		D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
		D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE));

	// irradiance はデファード (t6, ピクセル) に加えて Lumen Radiosity の
	// スカイ項 (t11, コンピュート) からも読まれるため (PIXEL | NON_PIXEL)
	cl->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(
		m_IrradianceCube.Resource.Get(),
		D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
		D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));

	// prefilter もデファード (t7) に加えて Lumen のスカイ / 反射ミス
	// (コンピュート, t14) から読まれるため (PIXEL | NON_PIXEL)
	cl->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(
		m_PrefilterCube.Resource.Get(),
		D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
		D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));

	cl->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(
		m_BRDFLut.Get(),
		D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
		D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE));

	// equirect を元の PIXEL_SHADER_RESOURCE に戻す（Deferred の t6 で使用）
	cl->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(
		EquirectResource,
		D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
		D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE));

	// ---- 一時ディスクリプタを実行完了後に解放するため、先に flush ----
	// コマンドリストを即時実行して GPU 完了待ち＋Reset
	m_Owner->FlushAndResetCommandList();

	// 実行完了後に一時ディスクリプタを解放
	for (unsigned int d : tempDescriptors)
		m_Owner->ReleaseShaderResourceView(d);
	tempDescriptors.clear();
}

// ============================================================
//  Deferred パスで IBL precomputed テクスチャを t7,t8,t9 にバインド
// ============================================================
void IBLBaker::BindTextures()
{
	// t6 : Irradiance cube
	CommandList()->SetGraphicsRootDescriptorTable(
		(unsigned int)RenderManager::TEXTURE_TYPE::IRRADIANCE, m_IrradianceCube.SRVHandle);

	// t7 : Prefilter cube
	CommandList()->SetGraphicsRootDescriptorTable(
		(unsigned int)RenderManager::TEXTURE_TYPE::PREFILTER, m_PrefilterCube.SRVHandle);

	// t8 : BRDF LUT
	CommandList()->SetGraphicsRootDescriptorTable(
		(unsigned int)RenderManager::TEXTURE_TYPE::BRDF_LUT, m_BRDFLutSRVHandle);
}