#include "Main.h"
#include "RenderManager.h"
#include "LumenScene.h"
#include "LumenHardwareRayTracing.h"
#include "Scene.h"
#include "PrimitiveSceneProxy.h"
#include "FBXModel.h"
#include "DistanceFieldAtlas.h"

#include "D3DX12.h"

#include <fstream>

// ============================================================
//  Lifetime
// ============================================================
FLumenSceneData::FLumenSceneData(RenderManager* RHI)
	: m_RHI(RHI)
{
}


FLumenSceneData::~FLumenSceneData()
{
	// アップロードバッファの永続 Map を解除
	for (int i = 0; i < 2; i++)
	{
		if (m_ObjectBuffer[i]) { m_ObjectBuffer[i]->Unmap(0, nullptr); }
		if (m_CardBuffer[i]) { m_CardBuffer[i]->Unmap(0, nullptr); }
		if (m_PassParamBuffer[i]) { m_PassParamBuffer[i]->Unmap(0, nullptr); }

		if (m_ObjectBufferSRVIndex[i]) { m_RHI->ReleaseShaderResourceView(m_ObjectBufferSRVIndex[i]); }
		if (m_CardBufferSRVIndex[i]) { m_RHI->ReleaseShaderResourceView(m_CardBufferSRVIndex[i]); }
	}

	if (m_DepthAtlasSRVIndex) { m_RHI->ReleaseShaderResourceView(m_DepthAtlasSRVIndex); }

	auto releaseComputeTexture = [this](FLumenComputeTexture& tex)
		{
			if (tex.Resource)
			{
				m_RHI->DeferredRelease(tex.Resource, (int)tex.SRVIndex, -1);
				m_RHI->ReleaseShaderResourceView(tex.UAVIndex);
				tex.Resource = nullptr;
			}
		};
	releaseComputeTexture(m_DirectLightingAtlas);
	releaseComputeTexture(m_IndirectLightingAtlas);
	releaseComputeTexture(m_FinalLightingAtlas);
	for (unsigned int i = 0; i < LUMEN_GLOBAL_SDF_CLIPMAPS; i++)
	{
		releaseComputeTexture(m_GlobalSDF[i]);
	}
	releaseComputeTexture(m_ProbeGeo);
	releaseComputeTexture(m_ProbeTraceRadiance);
	releaseComputeTexture(m_ProbeFilteredRadiance);
	for (int i = 0; i < 2; i++)
	{
		releaseComputeTexture(m_ProbeSH[i].SHR);
		releaseComputeTexture(m_ProbeSH[i].SHG);
		releaseComputeTexture(m_ProbeSH[i].SHB);
		releaseComputeTexture(m_ProbeSH[i].Aux);
	}
	releaseComputeTexture(m_DiffuseIndirect);
	releaseComputeTexture(m_ReflectionTexture);
	releaseComputeTexture(m_RCAtlas);
	for (int i = 0; i < 3; i++)
	{
		releaseComputeTexture(m_RCSH[i]);
	}

	if (m_DepthAtlas)
	{
		m_RHI->DeferredRelease(m_DepthAtlas);
		m_DepthAtlas = nullptr;
	}
}


ID3D12Device* FLumenSceneData::Device()
{
	return m_RHI->GetDevice();
}


ID3D12GraphicsCommandList* FLumenSceneData::CommandList()
{
	return m_RHI->GetGraphicsCommandList();
}


bool FLumenSceneData::IsHardwareRayTracingSupported() const
{
	return m_HardwareRayTracing && m_HardwareRayTracing->IsAvailable();
}


// ============================================================
//  Init
// ============================================================
void FLumenSceneData::Init()
{
	InitAtlases();
	InitScreenTextures();
	InitBuffers();
	InitComputePipelines();

	// DXR TLAS (対応環境のみ有効化される)
	m_HardwareRayTracing = std::make_unique<FLumenHardwareRayTracing>(m_RHI);
	m_HardwareRayTracing->Init(MAX_LUMEN_OBJECTS);
}


// ------------------------------------------------------------
//  コンピュートテクスチャ生成 (2D: Depth=1 / 3D: Depth>1)
// ------------------------------------------------------------
void FLumenSceneData::CreateComputeTexture(FLumenComputeTexture& Texture, const wchar_t* Name,
	unsigned int Width, unsigned int Height, unsigned int Depth,
	DXGI_FORMAT Format, bool bStartInReadState)
{
	const bool bVolume = (Depth > 1);

	D3D12_RESOURCE_DESC desc{};
	desc.Dimension = bVolume
		? D3D12_RESOURCE_DIMENSION_TEXTURE3D
		: D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	desc.Width = Width;
	desc.Height = Height;
	desc.DepthOrArraySize = (UINT16)Depth;
	desc.MipLevels = 1;
	desc.Format = Format;
	desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
	desc.SampleDesc.Count = 1;
	desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

	const D3D12_RESOURCE_STATES readState =
		D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

	HRESULT hr = Device()->CreateCommittedResource(
		&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
		D3D12_HEAP_FLAG_NONE,
		&desc,
		bStartInReadState ? readState : D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
		nullptr,
		IID_PPV_ARGS(&Texture.Resource));
	assert(SUCCEEDED(hr));
	Texture.Resource->SetName(Name);

	// SRV
	Texture.SRVIndex = m_RHI->AllocateDescriptor();
	D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
	srvDesc.Format = Format;
	srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	if (bVolume)
	{
		srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
		srvDesc.Texture3D.MipLevels = 1;
	}
	else
	{
		srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
		srvDesc.Texture2D.MipLevels = 1;
	}
	Device()->CreateShaderResourceView(Texture.Resource.Get(), &srvDesc,
		m_RHI->GetCPUDescriptorHandle(Texture.SRVIndex));
	Texture.SRVHandle = m_RHI->GetGPUDescriptorHandle(Texture.SRVIndex);

	// UAV
	Texture.UAVIndex = m_RHI->AllocateDescriptor();
	D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
	uavDesc.Format = Format;
	if (bVolume)
	{
		uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
		uavDesc.Texture3D.WSize = Depth;
	}
	else
	{
		uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
	}
	Device()->CreateUnorderedAccessView(Texture.Resource.Get(), nullptr, &uavDesc,
		m_RHI->GetCPUDescriptorHandle(Texture.UAVIndex));

	Texture.bInReadState = bStartInReadState;
}


// ------------------------------------------------------------
//  Surface Cache アトラス群
// ------------------------------------------------------------
void FLumenSceneData::InitAtlases()
{
	ID3D12GraphicsCommandList* cl = CommandList();

	// ---- キャプチャアトラス (MRT。RHI の RENDER_TARGET を利用) ----
	m_AlbedoAtlas = m_RHI->CreateRenderTarget(LUMEN_ATLAS_WIDTH, LUMEN_ATLAS_HEIGHT, DXGI_FORMAT_R8G8B8A8_UNORM);
	m_NormalAtlas = m_RHI->CreateRenderTarget(LUMEN_ATLAS_WIDTH, LUMEN_ATLAS_HEIGHT, DXGI_FORMAT_R8G8B8A8_UNORM);
	m_EmissiveAtlas = m_RHI->CreateRenderTarget(LUMEN_ATLAS_WIDTH, LUMEN_ATLAS_HEIGHT, DXGI_FORMAT_R11G11B10_FLOAT);

	m_AlbedoAtlas->Resource->SetName(L"LumenAlbedoAtlas");
	m_NormalAtlas->Resource->SetName(L"LumenNormalAtlas");
	m_EmissiveAtlas->Resource->SetName(L"LumenEmissiveAtlas");

	// 生成直後は PIXEL_SHADER_RESOURCE。コンピュートからも読むため
	// (PIXEL | NON_PIXEL) を常在の読み取り状態にする。
	{
		D3D12_RESOURCE_BARRIER barriers[3] = {
			CD3DX12_RESOURCE_BARRIER::Transition(m_AlbedoAtlas->Resource.Get(),
				D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
				D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
			CD3DX12_RESOURCE_BARRIER::Transition(m_NormalAtlas->Resource.Get(),
				D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
				D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
			CD3DX12_RESOURCE_BARRIER::Transition(m_EmissiveAtlas->Resource.Get(),
				D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
				D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
		};
		cl->ResourceBarrier(_countof(barriers), barriers);
	}

	// ---- 深度アトラス (R32_TYPELESS -> DSV D32 / SRV R32) ----
	{
		D3D12_RESOURCE_DESC desc{};
		desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
		desc.Width = LUMEN_ATLAS_WIDTH;
		desc.Height = LUMEN_ATLAS_HEIGHT;
		desc.DepthOrArraySize = 1;
		desc.MipLevels = 1;
		desc.Format = DXGI_FORMAT_R32_TYPELESS;
		desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
		desc.SampleDesc.Count = 1;
		desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

		D3D12_CLEAR_VALUE clearValue{};
		clearValue.Format = DXGI_FORMAT_D32_FLOAT;
		clearValue.DepthStencil.Depth = 1.0f;

		HRESULT hr = Device()->CreateCommittedResource(
			&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
			D3D12_HEAP_FLAG_NONE,
			&desc,
			D3D12_RESOURCE_STATE_DEPTH_WRITE,
			&clearValue,
			IID_PPV_ARGS(&m_DepthAtlas));
		assert(SUCCEEDED(hr));
		m_DepthAtlas->SetName(L"LumenDepthAtlas");

		// DSV (専有ヒープ)
		D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
		heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
		heapDesc.NumDescriptors = 1;
		heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;

		hr = Device()->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&m_DepthAtlasDSVHeap));
		assert(SUCCEEDED(hr));

		m_DepthAtlasDSV = m_DepthAtlasDSVHeap->GetCPUDescriptorHandleForHeapStart();

		D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
		dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
		dsvDesc.Format = DXGI_FORMAT_D32_FLOAT;
		Device()->CreateDepthStencilView(m_DepthAtlas.Get(), &dsvDesc, m_DepthAtlasDSV);

		// SRV (R32_FLOAT)
		m_DepthAtlasSRVIndex = m_RHI->AllocateDescriptor();

		D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
		srvDesc.Format = DXGI_FORMAT_R32_FLOAT;
		srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
		srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		srvDesc.Texture2D.MipLevels = 1;
		Device()->CreateShaderResourceView(m_DepthAtlas.Get(), &srvDesc,
			m_RHI->GetCPUDescriptorHandle(m_DepthAtlasSRVIndex));

		// 初期状態 DEPTH_WRITE -> 常在の読み取り状態へ
		cl->ResourceBarrier(1,
			&CD3DX12_RESOURCE_BARRIER::Transition(m_DepthAtlas.Get(),
				D3D12_RESOURCE_STATE_DEPTH_WRITE,
				D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
	}

	// ---- ライティングアトラス (RGBA16F, UAV) ----
	// COPY_DEST で生成 -> ゼロ充填 -> 各パスの期待状態へ遷移。
	// (未書き込みタイルの不定値 (NaN 等) が Radiosity のフィードバックで
	//  伝播するのを防ぐため、初期化は必須)
	auto createZeroFilledTexture = [this](FLumenComputeTexture& tex, const wchar_t* name,
		unsigned int width, unsigned int height)
		{
			D3D12_RESOURCE_DESC desc{};
			desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
			desc.Width = width;
			desc.Height = height;
			desc.DepthOrArraySize = 1;
			desc.MipLevels = 1;
			desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
			desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
			desc.SampleDesc.Count = 1;
			desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

			HRESULT hr = Device()->CreateCommittedResource(
				&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
				D3D12_HEAP_FLAG_NONE,
				&desc,
				D3D12_RESOURCE_STATE_COPY_DEST,
				nullptr,
				IID_PPV_ARGS(&tex.Resource));
			assert(SUCCEEDED(hr));
			tex.Resource->SetName(name);

			tex.SRVIndex = m_RHI->AllocateDescriptor();
			D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
			srvDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
			srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
			srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
			srvDesc.Texture2D.MipLevels = 1;
			Device()->CreateShaderResourceView(tex.Resource.Get(), &srvDesc,
				m_RHI->GetCPUDescriptorHandle(tex.SRVIndex));
			tex.SRVHandle = m_RHI->GetGPUDescriptorHandle(tex.SRVIndex);

			tex.UAVIndex = m_RHI->AllocateDescriptor();
			D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
			uavDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
			uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
			Device()->CreateUnorderedAccessView(tex.Resource.Get(), nullptr, &uavDesc,
				m_RHI->GetCPUDescriptorHandle(tex.UAVIndex));
		};

	createZeroFilledTexture(m_DirectLightingAtlas, L"LumenDirectLightingAtlas",
		LUMEN_ATLAS_WIDTH, LUMEN_ATLAS_HEIGHT);
	createZeroFilledTexture(m_IndirectLightingAtlas, L"LumenIndirectLightingAtlas",
		LUMEN_ATLAS_WIDTH, LUMEN_ATLAS_HEIGHT);
	createZeroFilledTexture(m_FinalLightingAtlas, L"LumenFinalLightingAtlas",
		LUMEN_ATLAS_WIDTH, LUMEN_ATLAS_HEIGHT);
	createZeroFilledTexture(m_RCAtlas, L"LumenRadianceCacheAtlas",
		LUMEN_RC_ATLAS_SIZE, LUMEN_RC_ATLAS_SIZE);

	// ---- ゼロ充填 (アップロードスクラッチからコピー) ----
	{
		const unsigned int rowPitch = LUMEN_ATLAS_WIDTH * 8;	// RGBA16F (8192B, 256 アライン済み)
		const UINT64 scratchSize = (UINT64)rowPitch * LUMEN_ATLAS_HEIGHT;

		ComPtr<ID3D12Resource> scratch;
		HRESULT hr = Device()->CreateCommittedResource(
			&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD),
			D3D12_HEAP_FLAG_NONE,
			&CD3DX12_RESOURCE_DESC::Buffer(scratchSize),
			D3D12_RESOURCE_STATE_GENERIC_READ,
			nullptr,
			IID_PPV_ARGS(&scratch));
		assert(SUCCEEDED(hr));

		void* mapped = nullptr;
		scratch->Map(0, nullptr, &mapped);
		memset(mapped, 0, (size_t)scratchSize);
		scratch->Unmap(0, nullptr);

		auto zeroFill = [&](FLumenComputeTexture& tex, unsigned int width, unsigned int height)
			{
				D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
				footprint.Footprint.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
				footprint.Footprint.Width = width;
				footprint.Footprint.Height = height;
				footprint.Footprint.Depth = 1;
				footprint.Footprint.RowPitch = width * 8;

				CD3DX12_TEXTURE_COPY_LOCATION dst(tex.Resource.Get(), 0);
				CD3DX12_TEXTURE_COPY_LOCATION src(scratch.Get(), footprint);
				cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
			};

		zeroFill(m_DirectLightingAtlas, LUMEN_ATLAS_WIDTH, LUMEN_ATLAS_HEIGHT);
		zeroFill(m_IndirectLightingAtlas, LUMEN_ATLAS_WIDTH, LUMEN_ATLAS_HEIGHT);
		zeroFill(m_FinalLightingAtlas, LUMEN_ATLAS_WIDTH, LUMEN_ATLAS_HEIGHT);
		zeroFill(m_RCAtlas, LUMEN_RC_ATLAS_SIZE, LUMEN_RC_ATLAS_SIZE);

		// 各パスの期待状態へ: Direct / Indirect / RCAtlas = UAV, Final = READ
		D3D12_RESOURCE_BARRIER barriers[4] = {
			CD3DX12_RESOURCE_BARRIER::Transition(m_DirectLightingAtlas.Resource.Get(),
				D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
			CD3DX12_RESOURCE_BARRIER::Transition(m_IndirectLightingAtlas.Resource.Get(),
				D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
			CD3DX12_RESOURCE_BARRIER::Transition(m_FinalLightingAtlas.Resource.Get(),
				D3D12_RESOURCE_STATE_COPY_DEST,
				D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
			CD3DX12_RESOURCE_BARRIER::Transition(m_RCAtlas.Resource.Get(),
				D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
		};
		cl->ResourceBarrier(_countof(barriers), barriers);

		m_DirectLightingAtlas.bInReadState = false;
		m_IndirectLightingAtlas.bInReadState = false;
		m_FinalLightingAtlas.bInReadState = true;
		m_RCAtlas.bInReadState = false;

		// コピーを即時完了させてからスクラッチを解放する
		m_RHI->FlushAndResetCommandList();
	}
}


// ------------------------------------------------------------
//  Global SDF / スクリーンプローブ / 反射 / Radiance Cache
// ------------------------------------------------------------
void FLumenSceneData::InitScreenTextures()
{
	const unsigned int width = (unsigned int)m_RHI->GetBackBufferWidth();
	const unsigned int height = (unsigned int)m_RHI->GetBackBufferHeight();

	// ---- Global Distance Field (128^3 R16F x2) ----
	CreateComputeTexture(m_GlobalSDF[0], L"LumenGlobalSDF0",
		LUMEN_GLOBAL_SDF_RESOLUTION, LUMEN_GLOBAL_SDF_RESOLUTION, LUMEN_GLOBAL_SDF_RESOLUTION,
		DXGI_FORMAT_R16_FLOAT, true);
	CreateComputeTexture(m_GlobalSDF[1], L"LumenGlobalSDF1",
		LUMEN_GLOBAL_SDF_RESOLUTION, LUMEN_GLOBAL_SDF_RESOLUTION, LUMEN_GLOBAL_SDF_RESOLUTION,
		DXGI_FORMAT_R16_FLOAT, true);

	// ---- Screen Probe Gather ----
	m_NumProbesX = (width + LUMEN_PROBE_DOWNSAMPLE - 1) / LUMEN_PROBE_DOWNSAMPLE;
	m_NumProbesY = (height + LUMEN_PROBE_DOWNSAMPLE - 1) / LUMEN_PROBE_DOWNSAMPLE;

	CreateComputeTexture(m_ProbeGeo, L"LumenProbeGeo",
		m_NumProbesX, m_NumProbesY, 1, DXGI_FORMAT_R16G16B16A16_FLOAT, false);
	CreateComputeTexture(m_ProbeTraceRadiance, L"LumenProbeTraceRadiance",
		m_NumProbesX * LUMEN_PROBE_OCTA_RES, m_NumProbesY * LUMEN_PROBE_OCTA_RES, 1,
		DXGI_FORMAT_R16G16B16A16_FLOAT, false);
	CreateComputeTexture(m_ProbeFilteredRadiance, L"LumenProbeFilteredRadiance",
		m_NumProbesX * LUMEN_PROBE_OCTA_RES, m_NumProbesY * LUMEN_PROBE_OCTA_RES, 1,
		DXGI_FORMAT_R16G16B16A16_FLOAT, false);

	for (int i = 0; i < 2; i++)
	{
		CreateComputeTexture(m_ProbeSH[i].SHR, L"LumenProbeSHR",
			m_NumProbesX, m_NumProbesY, 1, DXGI_FORMAT_R16G16B16A16_FLOAT, false);
		CreateComputeTexture(m_ProbeSH[i].SHG, L"LumenProbeSHG",
			m_NumProbesX, m_NumProbesY, 1, DXGI_FORMAT_R16G16B16A16_FLOAT, false);
		CreateComputeTexture(m_ProbeSH[i].SHB, L"LumenProbeSHB",
			m_NumProbesX, m_NumProbesY, 1, DXGI_FORMAT_R16G16B16A16_FLOAT, false);
		CreateComputeTexture(m_ProbeSH[i].Aux, L"LumenProbeAux",
			m_NumProbesX, m_NumProbesY, 1, DXGI_FORMAT_R16G16B16A16_FLOAT, false);
	}

	CreateComputeTexture(m_DiffuseIndirect, L"LumenDiffuseIndirect",
		width, height, 1, DXGI_FORMAT_R16G16B16A16_FLOAT, true);

	// ---- Reflections ----
	CreateComputeTexture(m_ReflectionTexture, L"LumenReflections",
		width, height, 1, DXGI_FORMAT_R16G16B16A16_FLOAT, true);

	// ---- Radiance Cache SH ボリューム (16^3 x3) ----
	for (int i = 0; i < 3; i++)
	{
		static const wchar_t* names[3] = {
			L"LumenRCSH_R", L"LumenRCSH_G", L"LumenRCSH_B" };
		CreateComputeTexture(m_RCSH[i], names[i],
			LUMEN_RC_PROBES_PER_AXIS, LUMEN_RC_PROBES_PER_AXIS, LUMEN_RC_PROBES_PER_AXIS,
			DXGI_FORMAT_R16G16B16A16_FLOAT, true);
	}
}


// ------------------------------------------------------------
//  オブジェクト / カード / パスパラメータのアップロードバッファ
// ------------------------------------------------------------
void FLumenSceneData::InitBuffers()
{
	for (int i = 0; i < 2; i++)
	{
		// ---- オブジェクトバッファ (t24) ----
		{
			const UINT64 size = sizeof(FLumenSceneObjectData) * MAX_LUMEN_OBJECTS;

			HRESULT hr = Device()->CreateCommittedResource(
				&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD),
				D3D12_HEAP_FLAG_NONE,
				&CD3DX12_RESOURCE_DESC::Buffer(size),
				D3D12_RESOURCE_STATE_GENERIC_READ,
				nullptr,
				IID_PPV_ARGS(&m_ObjectBuffer[i]));
			assert(SUCCEEDED(hr));

			hr = m_ObjectBuffer[i]->Map(0, nullptr, (void**)&m_ObjectBufferPointer[i]);
			assert(SUCCEEDED(hr));
			memset(m_ObjectBufferPointer[i], 0, (size_t)size);

			m_ObjectBufferSRVIndex[i] = m_RHI->AllocateDescriptor();

			D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
			srvDesc.Format = DXGI_FORMAT_UNKNOWN;
			srvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
			srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
			srvDesc.Buffer.NumElements = MAX_LUMEN_OBJECTS;
			srvDesc.Buffer.StructureByteStride = sizeof(FLumenSceneObjectData);
			Device()->CreateShaderResourceView(m_ObjectBuffer[i].Get(), &srvDesc,
				m_RHI->GetCPUDescriptorHandle(m_ObjectBufferSRVIndex[i]));
		}

		// ---- カードバッファ (t25) ----
		{
			const UINT64 size = sizeof(FLumenCardGPUData) * MAX_LUMEN_CARDS;

			HRESULT hr = Device()->CreateCommittedResource(
				&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD),
				D3D12_HEAP_FLAG_NONE,
				&CD3DX12_RESOURCE_DESC::Buffer(size),
				D3D12_RESOURCE_STATE_GENERIC_READ,
				nullptr,
				IID_PPV_ARGS(&m_CardBuffer[i]));
			assert(SUCCEEDED(hr));

			hr = m_CardBuffer[i]->Map(0, nullptr, (void**)&m_CardBufferPointer[i]);
			assert(SUCCEEDED(hr));
			memset(m_CardBufferPointer[i], 0, (size_t)size);

			m_CardBufferSRVIndex[i] = m_RHI->AllocateDescriptor();

			D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
			srvDesc.Format = DXGI_FORMAT_UNKNOWN;
			srvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
			srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
			srvDesc.Buffer.NumElements = MAX_LUMEN_CARDS;
			srvDesc.Buffer.StructureByteStride = sizeof(FLumenCardGPUData);
			Device()->CreateShaderResourceView(m_CardBuffer[i].Get(), &srvDesc,
				m_RHI->GetCPUDescriptorHandle(m_CardBufferSRVIndex[i]));
		}

		// ---- パスパラメータ (b0, PASS_PARAM_SLOTS x PASS_PARAM_STRIDE) ----
		{
			HRESULT hr = Device()->CreateCommittedResource(
				&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD),
				D3D12_HEAP_FLAG_NONE,
				&CD3DX12_RESOURCE_DESC::Buffer(PASS_PARAM_SLOTS * PASS_PARAM_STRIDE),
				D3D12_RESOURCE_STATE_GENERIC_READ,
				nullptr,
				IID_PPV_ARGS(&m_PassParamBuffer[i]));
			assert(SUCCEEDED(hr));

			hr = m_PassParamBuffer[i]->Map(0, nullptr, (void**)&m_PassParamPointer[i]);
			assert(SUCCEEDED(hr));
			memset(m_PassParamPointer[i], 0, PASS_PARAM_SLOTS * PASS_PARAM_STRIDE);
		}
	}
}


// ------------------------------------------------------------
//  コンピュートルートシグネチャ + PSO 群
//  (HLSL LumenSceneLightingCommon.hlsl のレイアウトと 1:1)
//    [0]      b0      ルート CBV
//    [1..28]  t0..t27 SRV テーブル
//    [29..36] u0..u7  UAV テーブル
//    [37]     t28     TLAS (ルート SRV)
// ------------------------------------------------------------
void FLumenSceneData::InitComputePipelines()
{
	const unsigned int NUM_SRV = 28;	// t0..t27
	const unsigned int NUM_UAV = 8;		// u0..u7

	D3D12_ROOT_PARAMETER  rootParameters[1 + NUM_SRV + NUM_UAV + 1]{};
	D3D12_DESCRIPTOR_RANGE ranges[NUM_SRV + NUM_UAV]{};

	// [0] ルート CBV (b0)
	rootParameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
	rootParameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
	rootParameters[0].Descriptor.ShaderRegister = 0;

	// [1..28] SRV テーブル (t0..t27)
	for (unsigned int i = 0; i < NUM_SRV; i++)
	{
		ranges[i].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
		ranges[i].NumDescriptors = 1;
		ranges[i].BaseShaderRegister = i;
		ranges[i].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

		rootParameters[1 + i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
		rootParameters[1 + i].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
		rootParameters[1 + i].DescriptorTable.NumDescriptorRanges = 1;
		rootParameters[1 + i].DescriptorTable.pDescriptorRanges = &ranges[i];
	}

	// [29..36] UAV テーブル (u0..u7)
	for (unsigned int i = 0; i < NUM_UAV; i++)
	{
		ranges[NUM_SRV + i].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
		ranges[NUM_SRV + i].NumDescriptors = 1;
		ranges[NUM_SRV + i].BaseShaderRegister = i;
		ranges[NUM_SRV + i].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

		rootParameters[1 + NUM_SRV + i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
		rootParameters[1 + NUM_SRV + i].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
		rootParameters[1 + NUM_SRV + i].DescriptorTable.NumDescriptorRanges = 1;
		rootParameters[1 + NUM_SRV + i].DescriptorTable.pDescriptorRanges = &ranges[NUM_SRV + i];
	}

	// [37] TLAS (ルート SRV t28。HWRT バリアントのみ参照)
	rootParameters[1 + NUM_SRV + NUM_UAV].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
	rootParameters[1 + NUM_SRV + NUM_UAV].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
	rootParameters[1 + NUM_SRV + NUM_UAV].Descriptor.ShaderRegister = NUM_SRV; // t28

	// s0: リニアクランプ (アトラス / SDF / キューブ共用)
	D3D12_STATIC_SAMPLER_DESC samplerDesc{};
	samplerDesc.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
	samplerDesc.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	samplerDesc.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	samplerDesc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	samplerDesc.MipLODBias = 0.0f;
	samplerDesc.MaxAnisotropy = 1;
	samplerDesc.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
	samplerDesc.BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK;
	samplerDesc.MinLOD = 0.0f;
	samplerDesc.MaxLOD = D3D12_FLOAT32_MAX;
	samplerDesc.ShaderRegister = 0;
	samplerDesc.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

	D3D12_ROOT_SIGNATURE_DESC desc{};
	desc.NumParameters = _countof(rootParameters);
	desc.pParameters = rootParameters;
	desc.NumStaticSamplers = 1;
	desc.pStaticSamplers = &samplerDesc;

	ComPtr<ID3DBlob> blob;
	ComPtr<ID3DBlob> errorBlob;
	HRESULT hr = D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errorBlob);
	assert(SUCCEEDED(hr));

	hr = Device()->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
		IID_PPV_ARGS(&m_ComputeRootSignature));
	assert(SUCCEEDED(hr));

	// ---- SWRT PSO 群 ----
	m_PSODirectLighting = CreateComputePipeline("Shader/cso/LumenSceneDirectLighting_CS.cso");
	m_PSORadiosity = CreateComputePipeline("Shader/cso/LumenRadiosity_CS.cso");
	m_PSOCombine = CreateComputePipeline("Shader/cso/LumenSceneCombine_CS.cso");
	m_PSOGlobalSDF = CreateComputePipeline("Shader/cso/LumenGlobalDistanceField_CS.cso");
	m_PSOProbeSetup = CreateComputePipeline("Shader/cso/LumenScreenProbeSetup_CS.cso");
	m_PSOProbeTrace = CreateComputePipeline("Shader/cso/LumenScreenProbeTrace_CS.cso");
	m_PSOProbeFilter = CreateComputePipeline("Shader/cso/LumenScreenProbeFilter_CS.cso");
	m_PSOProbeSH = CreateComputePipeline("Shader/cso/LumenScreenProbeSH_CS.cso");
	m_PSOProbeIntegrate = CreateComputePipeline("Shader/cso/LumenScreenProbeIntegrate_CS.cso");
	m_PSOReflections = CreateComputePipeline("Shader/cso/LumenReflections_CS.cso");
	m_PSORCTrace = CreateComputePipeline("Shader/cso/LumenRadianceCache_CS.cso");
	m_PSORCSH = CreateComputePipeline("Shader/cso/LumenRadianceCacheSH_CS.cso");

	// ---- HWRT (RayQuery, SM 6.5) バリアント ----
	// DXR 非対応環境 / cso 不在 / 生成失敗時は null のまま SWRT を使う。
	if (m_RHI->IsRayTracingSupported())
	{
		m_PSODirectLightingRT = TryCreateComputePipeline("Shader/cso/LumenSceneDirectLightingRT_CS.cso");
		m_PSORadiosityRT = TryCreateComputePipeline("Shader/cso/LumenRadiosityRT_CS.cso");
		m_PSOProbeTraceRT = TryCreateComputePipeline("Shader/cso/LumenScreenProbeTraceRT_CS.cso");
		m_PSOReflectionsRT = TryCreateComputePipeline("Shader/cso/LumenReflectionsRT_CS.cso");
		m_PSORCTraceRT = TryCreateComputePipeline("Shader/cso/LumenRadianceCacheRT_CS.cso");
	}
}


ComPtr<ID3D12PipelineState> FLumenSceneData::CreateComputePipeline(const char* csoFile)
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


ComPtr<ID3D12PipelineState> FLumenSceneData::TryCreateComputePipeline(const char* csoFile)
{
	std::vector<char> cs;
	{
		std::ifstream file(csoFile, std::ios_base::in | std::ios_base::binary);
		if (!file)
		{
			char msg[256];
			sprintf_s(msg, "[LumenScene] HWRT cso not found (fallback to SWRT): %s\n", csoFile);
			OutputDebugStringA(msg);
			return nullptr;
		}
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
	if (FAILED(hr))
	{
		char msg[256];
		sprintf_s(msg, "[LumenScene] HWRT PSO creation failed (fallback to SWRT): %s (hr=0x%08X)\n",
			csoFile, (unsigned int)hr);
		OutputDebugStringA(msg);
		return nullptr;
	}
	return pso;
}


// ============================================================
//  MeshCards 生成
// ============================================================
void FLumenSceneData::BuildMeshCards(const FPrimitiveSceneProxy* Proxy, FLumenObjectSlot& Slot) const
{
	const FBXModel* mesh = Proxy->GetDistanceFieldMesh();
	const FBoxSphereBounds localBounds = mesh->GetLocalBounds();

	const XMVECTOR center = XMLoadFloat3(&localBounds.Origin);
	const XMVECTOR boxExtent = XMLoadFloat3(&localBounds.BoxExtent);

	static const XMFLOAT3 directions[LUMEN_CARDS_PER_OBJECT] = {
		{ 1.0f, 0.0f, 0.0f }, { -1.0f, 0.0f, 0.0f },
		{ 0.0f, 1.0f, 0.0f }, { 0.0f, -1.0f, 0.0f },
		{ 0.0f, 0.0f, 1.0f }, { 0.0f, 0.0f, -1.0f },
	};

	for (unsigned int i = 0; i < LUMEN_CARDS_PER_OBJECT; i++)
	{
		FLumenCardLocal& card = Slot.Cards[i];

		const XMVECTOR normal = XMLoadFloat3(&directions[i]);
		const XMVECTOR forward = XMVectorNegate(normal);	// カメラは面の外側から内側を見る

		// ±Y カードは up を Z へ退避 (LookTo の縮退防止)
		XMVECTOR up = (fabsf(directions[i].y) > 0.5f)
			? XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f)
			: XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);

		const XMVECTOR right = XMVector3Normalize(XMVector3Cross(up, forward));
		const XMVECTOR realUp = XMVector3Cross(forward, right);

		// カード半幅 = 境界半幅を各カード軸へ射影 (軸整列なので絶対値の内積)
		const float ex = XMVectorGetX(XMVector3Dot(XMVectorAbs(right), boxExtent));
		const float ey = XMVectorGetX(XMVector3Dot(XMVectorAbs(realUp), boxExtent));
		const float ez = XMVectorGetX(XMVector3Dot(XMVectorAbs(forward), boxExtent));

		// 境界ちょうどのジオメトリがクリップ / 欠けしないようマージンを付与
		const float exM = ex * 1.02f + 0.005f;
		const float eyM = ey * 1.02f + 0.005f;
		const float ezM = ez * 1.05f + 0.01f;

		const XMVECTOR eye = XMVectorAdd(center, XMVectorScale(normal, ezM));

		const XMMATRIX view = XMMatrixLookToLH(eye, forward, realUp);
		const XMMATRIX proj = XMMatrixOrthographicLH(2.0f * exM, 2.0f * eyM, 0.0f, 2.0f * ezM);

		XMStoreFloat4x4(&card.LocalViewMatrix, view);
		XMStoreFloat4x4(&card.ProjectionMatrix, proj);
		XMStoreFloat4x4(&card.CardToLocal, XMMatrixInverse(nullptr, view));
		card.Extent = { exM, eyM, ezM };
	}
}


// ============================================================
//  スロット管理
// ============================================================
int FLumenSceneData::AllocateSlot(const UPrimitiveComponent* Component,
	const FPrimitiveSceneProxy* Proxy)
{
	for (unsigned int i = 0; i < MAX_LUMEN_OBJECTS; i++)
	{
		if (m_Slots[i].Proxy == nullptr)
		{
			m_Slots[i].Component = Component;
			m_Slots[i].Proxy = Proxy;
			memset(m_Slots[i].bCaptured, 0, sizeof(m_Slots[i].bCaptured));

			BuildMeshCards(Proxy, m_Slots[i]);

			// 全カードのキャプチャを予約
			for (unsigned int c = 0; c < LUMEN_CARDS_PER_OBJECT; c++)
			{
				m_CaptureQueue.push_back({ i, c });
			}
			return (int)i;
		}
	}
	return -1;	// スロット枯渇 (このプリミティブは Lumen に参加しない)
}


void FLumenSceneData::FreeSlot(unsigned int SlotIndex)
{
	m_Slots[SlotIndex].Component = nullptr;
	m_Slots[SlotIndex].Proxy = nullptr;
	memset(m_Slots[SlotIndex].bCaptured, 0, sizeof(m_Slots[SlotIndex].bCaptured));

	// 旧プロキシのキャプチャ要求を破棄 (スロット再利用時の誤キャプチャ防止)
	for (auto it = m_CaptureQueue.begin(); it != m_CaptureQueue.end();)
	{
		it = (it->SlotIndex == SlotIndex) ? m_CaptureQueue.erase(it) : (it + 1);
	}
}


XMFLOAT4 FLumenSceneData::GetAtlasUVScaleBias(unsigned int GlobalCardIndex)
{
	const unsigned int tileX = GlobalCardIndex % LUMEN_ATLAS_TILES_X;
	const unsigned int tileY = GlobalCardIndex / LUMEN_ATLAS_TILES_X;

	return XMFLOAT4(
		1.0f / (float)LUMEN_ATLAS_TILES_X,
		1.0f / (float)LUMEN_ATLAS_TILES_Y,
		(float)tileX / (float)LUMEN_ATLAS_TILES_X,
		(float)tileY / (float)LUMEN_ATLAS_TILES_Y);
}


// ============================================================
//  UpdateLumenScene
// ============================================================
void FLumenSceneData::UpdateLumenScene(FScene* Scene)
{
	m_BufferFrame ^= 1;
	m_FrameNumber++;

	const unsigned int prevProbesX = m_Stats.NumProbesX;
	m_Stats = Stats{};
	m_Stats.NumProbesX = m_NumProbesX;
	m_Stats.NumProbesY = m_NumProbesY;
	(void)prevProbesX;

	bool slotSeen[MAX_LUMEN_OBJECTS] = {};

	if (m_Params.bEnabled && Scene != nullptr)
	{
		for (const FPrimitiveSceneInfo& info : Scene->GetPrimitives())
		{
			const FPrimitiveSceneProxy* proxy = info.Proxy.get();
			if (proxy == nullptr || !proxy->IsVisible() || !proxy->AffectsDistanceField() ||
				proxy->GetDistanceFieldMesh() == nullptr)
			{
				continue;
			}

			// 既存スロットを検索 ((コンポーネント, プロキシ) ペア一致。
			// プロキシ再生成 = マテリアル変更時はプロキシポインタが
			// 変わるため自動的に再割当 -> 再キャプチャされる)
			int slotIndex = -1;
			for (unsigned int i = 0; i < MAX_LUMEN_OBJECTS; i++)
			{
				if (m_Slots[i].Proxy == proxy && m_Slots[i].Component == info.Component)
				{
					slotIndex = (int)i;
					break;
				}
			}

			if (slotIndex < 0)
			{
				slotIndex = AllocateSlot(info.Component, proxy);
			}

			if (slotIndex >= 0)
			{
				slotSeen[slotIndex] = true;
			}
		}
	}

	// 消えたプロキシのスロットを解放
	for (unsigned int i = 0; i < MAX_LUMEN_OBJECTS; i++)
	{
		if (m_Slots[i].Proxy != nullptr && !slotSeen[i])
		{
			FreeSlot(i);
		}
	}

	// ---- GPU データ詰め直し ----
	unsigned int highestUsed = 0;

	for (unsigned int i = 0; i < MAX_LUMEN_OBJECTS; i++)
	{
		FLumenSceneObjectData& obj = m_ObjectData[i];
		obj = FLumenSceneObjectData{};
		obj.CardOffset = i * LUMEN_CARDS_PER_OBJECT;
		obj.NumCards = LUMEN_CARDS_PER_OBJECT;

		for (unsigned int c = 0; c < LUMEN_CARDS_PER_OBJECT; c++)
		{
			m_CardData[i * LUMEN_CARDS_PER_OBJECT + c] = FLumenCardGPUData{};
		}

		const FLumenObjectSlot& slot = m_Slots[i];
		if (slot.Proxy == nullptr)
		{
			continue;
		}

		const FBXModel* mesh = slot.Proxy->GetDistanceFieldMesh();
		const FDistanceFieldMeshInfo& df = mesh->GetDistanceField();

		// ---- SDF: ワールド -> ボリューム [-1,1] ----
		// (FShadowSceneRenderer::UpdateDistanceFieldObjects と同一の変換)
		const XMMATRIX localToWorld = XMLoadFloat4x4(&slot.Proxy->GetLocalToWorld());
		const XMMATRIX volumeToLocal =
			XMMatrixScaling(df.LocalBoundsExtent.x, df.LocalBoundsExtent.y, df.LocalBoundsExtent.z) *
			XMMatrixTranslation(df.LocalBoundsCenter.x, df.LocalBoundsCenter.y, df.LocalBoundsCenter.z);
		const XMMATRIX worldToVolume = XMMatrixInverse(nullptr, volumeToLocal * localToWorld);

		const float scaleX = XMVectorGetX(XMVector3Length(localToWorld.r[0]));
		const float scaleY = XMVectorGetX(XMVector3Length(localToWorld.r[1]));
		const float scaleZ = XMVectorGetX(XMVector3Length(localToWorld.r[2]));
		const float maxScale = fmaxf(scaleX, fmaxf(scaleY, scaleZ));
		const float distanceScaleWorld = df.DistanceScaleLocal * maxScale;

		XMStoreFloat4x4(&obj.WorldToVolume, XMMatrixTranspose(worldToVolume));
		obj.VolumeUVScaleAndDistance = {
			df.UVScale.x, df.UVScale.y, df.UVScale.z, distanceScaleWorld };
		// w = SDF 1 ボクセルのワールド幅 (DistanceFieldShadowing.hlsl の
		//     voxelWorld = distanceScale / 128 と同じ換算)
		obj.VolumeUVAdd = {
			df.UVAdd.x, df.UVAdd.y, df.UVAdd.z, distanceScaleWorld * 0.0078125f };
		obj.bValid = 1;

		highestUsed = i + 1;
		m_Stats.NumObjects++;

		// ---- カード: ローカルカード x LocalToWorld ----
		for (unsigned int c = 0; c < LUMEN_CARDS_PER_OBJECT; c++)
		{
			const unsigned int globalCard = i * LUMEN_CARDS_PER_OBJECT + c;
			const FLumenCardLocal& local = slot.Cards[c];
			FLumenCardGPUData& gpu = m_CardData[globalCard];

			const XMMATRIX cardToLocal = XMLoadFloat4x4(&local.CardToLocal);
			const XMMATRIX cardToWorld = cardToLocal * localToWorld;
			const XMMATRIX worldToCard = XMMatrixInverse(nullptr, cardToWorld);

			XMStoreFloat4x4(&gpu.WorldToCard, XMMatrixTranspose(worldToCard));
			XMStoreFloat4x4(&gpu.CardToWorld, XMMatrixTranspose(cardToWorld));

			// カード法線 (面の外向き) = カード空間 -Z のワールド方向
			const XMVECTOR worldNormal = XMVector3Normalize(
				XMVector3TransformNormal(XMVectorSet(0.0f, 0.0f, -1.0f, 0.0f), cardToWorld));
			XMFLOAT3 dir;
			XMStoreFloat3(&dir, worldNormal);
			gpu.CardDirection = { dir.x, dir.y, dir.z, 0.0f };

			gpu.CardExtentAndValid = {
				local.Extent.x, local.Extent.y, local.Extent.z,
				slot.bCaptured[c] ? 1.0f : 0.0f };
			gpu.AtlasUVScaleBias = GetAtlasUVScaleBias(globalCard);

			if (slot.bCaptured[c])
			{
				m_Stats.NumValidCards++;
			}
		}
	}

	m_NumObjects = highestUsed;
	m_Stats.NumPendingCaptures = (unsigned int)m_CaptureQueue.size();

	// ---- アップロード ----
	memcpy(m_ObjectBufferPointer[m_BufferFrame], m_ObjectData, sizeof(m_ObjectData));
	memcpy(m_CardBufferPointer[m_BufferFrame], m_CardData, sizeof(m_CardData));

	// ---- HWRT: TLAS 再構築 ----
	UpdateTLAS();
}


// ============================================================
//  UpdateTLAS (HWRT)
//  スロット列から D3D12_RAYTRACING_INSTANCE_DESC を詰め直し、
//  TLAS のインプレース再構築を記録する。InstanceID = Lumen
//  スロット番号 (= Surface Cache 採光のオブジェクトインデックス)。
// ============================================================
void FLumenSceneData::UpdateTLAS()
{
	m_bHWRTActiveThisFrame = false;

	if (!m_HardwareRayTracing || !m_HardwareRayTracing->IsAvailable() ||
		!m_Params.bEnabled || !m_Params.bUseHardwareRayTracing)
	{
		if (m_HardwareRayTracing && m_HardwareRayTracing->IsAvailable())
		{
			m_HardwareRayTracing->BuildTLAS(0);	// 無効フレーム: TLAS なし
		}
		return;
	}

	D3D12_RAYTRACING_INSTANCE_DESC* instances = m_HardwareRayTracing->BeginInstances();
	if (instances == nullptr)
	{
		return;
	}

	unsigned int numInstances = 0;

	for (unsigned int i = 0; i < MAX_LUMEN_OBJECTS; i++)
	{
		const FLumenObjectSlot& slot = m_Slots[i];
		if (slot.Proxy == nullptr)
		{
			continue;
		}

		const FBXModel* mesh = slot.Proxy->GetDistanceFieldMesh();
		if (mesh == nullptr || !mesh->HasBLAS())
		{
			continue;
		}

		D3D12_RAYTRACING_INSTANCE_DESC& inst = instances[numInstances];
		memset(&inst, 0, sizeof(inst));

		// D3D12 の Transform3x4 は「列ベクトルを変換する行優先 3x4」。
		// 本エンジンの行ベクトル規約 (v x M) とは転置関係:
		//   Transform[r][c] = LocalToWorld[c][r]
		const XMFLOAT4X4& m = slot.Proxy->GetLocalToWorld();
		for (int r = 0; r < 3; r++)
		{
			for (int c = 0; c < 4; c++)
			{
				inst.Transform[r][c] = m.m[c][r];
			}
		}

		inst.InstanceID = i;	// Lumen スロット番号 (シェーダの HitObject)
		inst.InstanceMask = 0xFF;
		// SDF 経路と同じく両面を遮蔽させる (キャプチャも両面)
		inst.Flags = D3D12_RAYTRACING_INSTANCE_FLAG_TRIANGLE_CULL_DISABLE;
		inst.AccelerationStructure = mesh->GetBLASAddress();

		numInstances++;
	}

	m_HardwareRayTracing->BuildTLAS(numInstances);
	m_Stats.NumTLASInstances = m_HardwareRayTracing->GetNumInstances();
}


// ============================================================
//  RenderCardCaptures
// ============================================================
void FLumenSceneData::RenderCardCaptures()
{
	if (!m_Params.bEnabled || m_CaptureQueue.empty())
	{
		return;
	}

	ID3D12GraphicsCommandList* cl = CommandList();

	// ---- READ -> RENDER_TARGET / DEPTH_WRITE ----
	{
		D3D12_RESOURCE_BARRIER barriers[4] = {
			CD3DX12_RESOURCE_BARRIER::Transition(m_AlbedoAtlas->Resource.Get(),
				D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
				D3D12_RESOURCE_STATE_RENDER_TARGET),
			CD3DX12_RESOURCE_BARRIER::Transition(m_NormalAtlas->Resource.Get(),
				D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
				D3D12_RESOURCE_STATE_RENDER_TARGET),
			CD3DX12_RESOURCE_BARRIER::Transition(m_EmissiveAtlas->Resource.Get(),
				D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
				D3D12_RESOURCE_STATE_RENDER_TARGET),
			CD3DX12_RESOURCE_BARRIER::Transition(m_DepthAtlas.Get(),
				D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
				D3D12_RESOURCE_STATE_DEPTH_WRITE),
		};
		cl->ResourceBarrier(_countof(barriers), barriers);
	}

	// MRT: Albedo / Normal / Emissive + 深度アトラス
	D3D12_CPU_DESCRIPTOR_HANDLE rtvs[3] = {
		m_AlbedoAtlas->RTVHandle, m_NormalAtlas->RTVHandle, m_EmissiveAtlas->RTVHandle };
	cl->OMSetRenderTargets(3, rtvs, FALSE, &m_DepthAtlasDSV);

	const int budget = max(1, m_Params.CaptureBudgetPerFrame);

	for (int n = 0; n < budget && !m_CaptureQueue.empty(); )
	{
		const FCaptureRequest request = m_CaptureQueue.front();
		m_CaptureQueue.pop_front();

		FLumenObjectSlot& slot = m_Slots[request.SlotIndex];
		if (slot.Proxy == nullptr)
		{
			continue;	// 解放済みスロット (予算は消費しない)
		}

		const FLumenCardLocal& card = slot.Cards[request.CardIndex];
		const unsigned int globalCard = request.SlotIndex * LUMEN_CARDS_PER_OBJECT + request.CardIndex;

		// ---- タイルのビューポート / シザー / クリア ----
		const LONG x = (LONG)((globalCard % LUMEN_ATLAS_TILES_X) * LUMEN_CARD_RESOLUTION);
		const LONG y = (LONG)((globalCard / LUMEN_ATLAS_TILES_X) * LUMEN_CARD_RESOLUTION);

		D3D12_VIEWPORT vp{ (FLOAT)x, (FLOAT)y,
			(FLOAT)LUMEN_CARD_RESOLUTION, (FLOAT)LUMEN_CARD_RESOLUTION, 0.0f, 1.0f };
		D3D12_RECT sc{ x, y, x + (LONG)LUMEN_CARD_RESOLUTION, y + (LONG)LUMEN_CARD_RESOLUTION };
		cl->RSSetViewports(1, &vp);
		cl->RSSetScissorRects(1, &sc);

		// クリア: a=0 = 無効テクセル (キャプチャ PS が a=1 を書く)
		const FLOAT clearColor[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
		cl->ClearRenderTargetView(m_AlbedoAtlas->RTVHandle, clearColor, 1, &sc);
		cl->ClearRenderTargetView(m_NormalAtlas->RTVHandle, clearColor, 1, &sc);
		cl->ClearRenderTargetView(m_EmissiveAtlas->RTVHandle, clearColor, 1, &sc);
		cl->ClearDepthStencilView(m_DepthAtlasDSV, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 1, &sc);

		// ---- b0 = カードビュー (ローカル空間オルソ) ----
		VIEW_CONSTANT viewConstant{};
		XMStoreFloat4x4(&viewConstant.View,
			XMMatrixTranspose(XMLoadFloat4x4(&card.LocalViewMatrix)));
		XMStoreFloat4x4(&viewConstant.Projection,
			XMMatrixTranspose(XMLoadFloat4x4(&card.ProjectionMatrix)));
		m_RHI->SetConstant(RenderManager::CONSTANT_TYPE::VIEW,
			&viewConstant, sizeof(viewConstant));

		// ---- 描画 (b1 = 単位行列はプロキシ側で積む) ----
		slot.Proxy->DrawCardCapture(m_RHI);

		slot.bCaptured[request.CardIndex] = true;
		m_Stats.NumCapturedThisFrame++;
		n++;
	}

	m_Stats.NumPendingCaptures = (unsigned int)m_CaptureQueue.size();

	// ---- RENDER_TARGET / DEPTH_WRITE -> READ ----
	{
		D3D12_RESOURCE_BARRIER barriers[4] = {
			CD3DX12_RESOURCE_BARRIER::Transition(m_AlbedoAtlas->Resource.Get(),
				D3D12_RESOURCE_STATE_RENDER_TARGET,
				D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
			CD3DX12_RESOURCE_BARRIER::Transition(m_NormalAtlas->Resource.Get(),
				D3D12_RESOURCE_STATE_RENDER_TARGET,
				D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
			CD3DX12_RESOURCE_BARRIER::Transition(m_EmissiveAtlas->Resource.Get(),
				D3D12_RESOURCE_STATE_RENDER_TARGET,
				D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
			CD3DX12_RESOURCE_BARRIER::Transition(m_DepthAtlas.Get(),
				D3D12_RESOURCE_STATE_DEPTH_WRITE,
				D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
		};
		cl->ResourceBarrier(_countof(barriers), barriers);
	}

	// ---- フル解像度ビューポート / シザーを復元 ----
	D3D12_VIEWPORT fullVP{ 0.0f, 0.0f,
		(FLOAT)m_RHI->GetBackBufferWidth(), (FLOAT)m_RHI->GetBackBufferHeight(), 0.0f, 1.0f };
	D3D12_RECT fullSC{ 0, 0,
		(LONG)m_RHI->GetBackBufferWidth(), (LONG)m_RHI->GetBackBufferHeight() };
	cl->RSSetViewports(1, &fullVP);
	cl->RSSetScissorRects(1, &fullSC);
}


// ============================================================
//  パスパラメータ / 状態遷移 / バインドヘルパー
// ============================================================
D3D12_GPU_VIRTUAL_ADDRESS FLumenSceneData::WritePassParams(unsigned int SlotIndex,
	const FLumenPassParams& Params)
{
	unsigned char* dst = m_PassParamPointer[m_BufferFrame] + SlotIndex * PASS_PARAM_STRIDE;
	memcpy(dst, &Params, sizeof(Params));
	return m_PassParamBuffer[m_BufferFrame]->GetGPUVirtualAddress()
		+ (UINT64)SlotIndex * PASS_PARAM_STRIDE;
}


FLumenSceneData::FLumenPassParams FLumenSceneData::MakeBasePassParams(
	const FLumenFrameInputs& Inputs) const
{
	const bool bHasDirectional =
		(Inputs.DirectionalLightColor.x + Inputs.DirectionalLightColor.y +
			Inputs.DirectionalLightColor.z) > 0.0f;

	FLumenPassParams params{};
	params.CardStartIndex = 0;
	params.NumCardsToProcess = MAX_LUMEN_CARDS;
	params.PassNumLumenObjects = m_NumObjects;
	params.PassNumLocalLights = Inputs.NumLocalLights;

	params.PassDirectionalLightDirection = {
		Inputs.DirectionalLightDirection.x, Inputs.DirectionalLightDirection.y,
		Inputs.DirectionalLightDirection.z, bHasDirectional ? 1.0f : 0.0f };
	params.PassDirectionalLightColor = Inputs.DirectionalLightColor;
	params.PassAtlasParams = {
		1.0f / (float)LUMEN_ATLAS_WIDTH, 1.0f / (float)LUMEN_ATLAS_HEIGHT,
		(float)LUMEN_CARD_RESOLUTION, (float)m_FrameNumber };
	params.PassTraceParams = {
		m_Params.MaxTraceDistance, m_Params.SurfaceBias,
		(float)max(1, m_Params.NumRadiosityRays), m_Params.EmissiveBoost };
	params.PassGlobalSDF0 = m_GlobalSDFParams[0];
	params.PassGlobalSDF1 = m_GlobalSDFParams[1];
	params.PassProbeParams0 = {
		(float)m_NumProbesX, (float)m_NumProbesY,
		(float)LUMEN_PROBE_DOWNSAMPLE, (float)LUMEN_PROBE_OCTA_RES };
	// w = 履歴有効 (テンポラル蓄積 / 前フレーム採光の可否)。
	// スクリーントレースの有効フラグは PassReflectionParams.w に分離
	// (スクリーントレース OFF でもテンポラル蓄積は生かすため)
	params.PassProbeParams1 = {
		(float)Inputs.ScreenWidth, (float)Inputs.ScreenHeight,
		m_Params.TemporalAlpha,
		Inputs.bHistoryValid ? 1.0f : 0.0f };
	params.PassCameraOrigin = {
		Inputs.CameraOrigin.x, Inputs.CameraOrigin.y, Inputs.CameraOrigin.z,
		m_Params.DetailTraceDistance };
	params.PassPrevCameraOrigin = {
		Inputs.PrevCameraOrigin.x, Inputs.PrevCameraOrigin.y, Inputs.PrevCameraOrigin.z,
		m_Params.ScreenTraceThickness };
	params.PassRCParams0 = m_RCVolumeParams0;
	params.PassRCParams1 = {
		(float)LUMEN_RC_PROBES_PER_AXIS, 0.0f, 0.0f, m_Params.SkySampleMip };
	params.PassReflectionParams = {
		m_Params.ReflectionMaxRoughness, m_Params.ReflectionFadeStart,
		m_Params.ReflectionIntensity,
		m_Params.bScreenSpaceTrace ? 1.0f : 0.0f };

	params.PassViewProjection = Inputs.ViewProjectionT;
	params.PassInvViewProjection = Inputs.InvViewProjectionT;
	params.PassPrevViewProjection = Inputs.PrevViewProjectionT;

	return params;
}


void FLumenSceneData::TransitionComputeTexture(FLumenComputeTexture& Texture, bool bToRead)
{
	if (Texture.bInReadState == bToRead)
	{
		return;
	}

	const D3D12_RESOURCE_STATES readState =
		D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

	CommandList()->ResourceBarrier(1,
		&CD3DX12_RESOURCE_BARRIER::Transition(
			Texture.Resource.Get(),
			bToRead ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS : readState,
			bToRead ? readState : D3D12_RESOURCE_STATE_UNORDERED_ACCESS));

	Texture.bInReadState = bToRead;
}


ID3D12PipelineState* FLumenSceneData::SelectTracePSO(
	ID3D12PipelineState* SWRT, ID3D12PipelineState* HWRT) const
{
	return (m_bHWRTActiveThisFrame && HWRT != nullptr) ? HWRT : SWRT;
}


// 共通 SRV/UAV テーブルのバインド。ルートパラメータ = レジスタ + 1
// (SRV) / レジスタ + 29 (UAV)。パス固有 (t19.. / u4..) は各パスが
// ディスパッチ直前に上書きする。
void FLumenSceneData::BindCommonComputeState(const FLumenFrameInputs& Inputs)
{
	ID3D12GraphicsCommandList* cl = CommandList();

	cl->SetComputeRootSignature(m_ComputeRootSignature.Get());

	auto bindSRV = [this, cl](unsigned int reg, unsigned int srvIndex)
		{
			cl->SetComputeRootDescriptorTable(1 + reg, m_RHI->GetGPUDescriptorHandle(srvIndex));
		};
	auto bindUAV = [this, cl](unsigned int reg, unsigned int uavIndex)
		{
			cl->SetComputeRootDescriptorTable(29 + reg, m_RHI->GetGPUDescriptorHandle(uavIndex));
		};

	bindSRV(0, FDistanceFieldAtlas::Get().GetAtlasSRVIndex());		// t0
	bindSRV(1, m_ObjectBufferSRVIndex[m_BufferFrame]);				// t1
	bindSRV(2, m_CardBufferSRVIndex[m_BufferFrame]);				// t2
	bindSRV(3, Inputs.LightBufferSRVIndex);							// t3
	bindSRV(4, m_AlbedoAtlas->SRVIndex);							// t4
	bindSRV(5, m_NormalAtlas->SRVIndex);							// t5
	bindSRV(6, m_EmissiveAtlas->SRVIndex);							// t6
	bindSRV(7, m_DepthAtlasSRVIndex);								// t7
	bindSRV(8, m_DirectLightingAtlas.SRVIndex);						// t8
	bindSRV(9, m_IndirectLightingAtlas.SRVIndex);					// t9
	bindSRV(10, m_FinalLightingAtlas.SRVIndex);						// t10
	bindSRV(11, Inputs.IrradianceSRVIndex);							// t11
	bindSRV(12, m_GlobalSDF[0].SRVIndex);							// t12
	bindSRV(13, m_GlobalSDF[1].SRVIndex);							// t13
	bindSRV(14, Inputs.PrefilterSRVIndex);							// t14
	bindSRV(15, Inputs.GBufferNormalSRVIndex);						// t15
	bindSRV(16, Inputs.SceneDepthSRVIndex);							// t16
	bindSRV(17, Inputs.LinearDepthSRVIndex);						// t17
	bindSRV(18, Inputs.PrevSceneColorSRVIndex);						// t18

	bindUAV(0, m_DirectLightingAtlas.UAVIndex);						// u0
	bindUAV(1, m_IndirectLightingAtlas.UAVIndex);					// u1
	bindUAV(2, m_FinalLightingAtlas.UAVIndex);						// u2
	bindUAV(3, m_GlobalSDF[0].UAVIndex);							// u3 (GDF ビルドが上書き)

	// TLAS (ルート SRV t28)。HWRT アクティブ時のみバインド
	// (RT バリアント PSO を使うディスパッチだけが参照する)
	if (m_bHWRTActiveThisFrame && m_HardwareRayTracing)
	{
		cl->SetComputeRootShaderResourceView(37, m_HardwareRayTracing->GetTLASAddress());
	}
}


void FLumenSceneData::DispatchRadiosityRange(unsigned int StartCard, unsigned int NumCards,
	unsigned int ParamSlot, const FLumenPassParams& BaseParams)
{
	if (NumCards == 0)
	{
		return;
	}

	FLumenPassParams params = BaseParams;
	params.CardStartIndex = StartCard;
	params.NumCardsToProcess = NumCards;

	CommandList()->SetComputeRootConstantBufferView(0, WritePassParams(ParamSlot, params));
	CommandList()->Dispatch(LUMEN_CARD_RESOLUTION / 8, LUMEN_CARD_RESOLUTION / 8, NumCards);
}


// ============================================================
//  Global Distance Field 再構築
//  カメラ追従のクリップマップ x2 を毎フレームフルビルドする
//  (128^3 x オブジェクトループ。ボクセルスナップでちらつき防止)。
// ============================================================
void FLumenSceneData::UpdateGlobalDistanceField(const FLumenFrameInputs& Inputs)
{
	// 無効時はトレース側の分岐用に半径 0 を伝える
	if (!m_Params.bGlobalSDF || m_NumObjects == 0)
	{
		m_GlobalSDFParams[0] = { 0.0f, 0.0f, 0.0f, 0.0f };
		m_GlobalSDFParams[1] = { 0.0f, 0.0f, 0.0f, 0.0f };
		return;
	}

	ID3D12GraphicsCommandList* cl = CommandList();

	const float halfExtents[LUMEN_GLOBAL_SDF_CLIPMAPS] = {
		fmaxf(m_Params.GlobalSDFExtent0, 1.0f),
		fmaxf(m_Params.GlobalSDFExtent0, 1.0f) * 4.0f };

	cl->SetPipelineState(m_PSOGlobalSDF.Get());

	for (unsigned int i = 0; i < LUMEN_GLOBAL_SDF_CLIPMAPS; i++)
	{
		// ボクセルグリッドへスナップ (カメラ移動によるちらつき防止)
		const float voxel = (2.0f * halfExtents[i]) / (float)LUMEN_GLOBAL_SDF_RESOLUTION;
		XMFLOAT4 params = {
			floorf(Inputs.CameraOrigin.x / voxel + 0.5f) * voxel,
			floorf(Inputs.CameraOrigin.y / voxel + 0.5f) * voxel,
			floorf(Inputs.CameraOrigin.z / voxel + 0.5f) * voxel,
			halfExtents[i] };
		m_GlobalSDFParams[i] = params;

		TransitionComputeTexture(m_GlobalSDF[i], false);	// -> UAV

		// u3 = ビルド対象クリップマップ
		cl->SetComputeRootDescriptorTable(29 + 3,
			m_RHI->GetGPUDescriptorHandle(m_GlobalSDF[i].UAVIndex));

		FLumenPassParams passParams = MakeBasePassParams(Inputs);
		passParams.CardStartIndex = i;	// クリップマップ番号
		// ビルド対象自身の中心 / 半径 (MakeBasePassParams はスナップ前の
		// 値を持ち得るため、確定値で上書きする)
		if (i == 0) { passParams.PassGlobalSDF0 = params; }
		else { passParams.PassGlobalSDF1 = params; }

		cl->SetComputeRootConstantBufferView(0, WritePassParams(4 + i, passParams));

		const unsigned int groups = LUMEN_GLOBAL_SDF_RESOLUTION / 8;
		cl->Dispatch(groups, groups, groups);

		TransitionComputeTexture(m_GlobalSDF[i], true);		// トレースが読む
	}
}


// ============================================================
//  Radiance Cache 更新 (トレース [予算制] -> SH ボリューム化)
// ============================================================
void FLumenSceneData::UpdateRadianceCache(const FLumenFrameInputs& Inputs)
{
	if (!m_Params.bRadianceCache)
	{
		return;
	}

	ID3D12GraphicsCommandList* cl = CommandList();

	// ---- ボリューム配置 (カメラ中心, プローブ間隔スナップ) ----
	const float spacing = fmaxf(m_Params.RadianceCacheSpacing, 0.1f);
	const float half = spacing * (float)LUMEN_RC_PROBES_PER_AXIS * 0.5f;
	m_RCVolumeParams0 = {
		floorf(Inputs.CameraOrigin.x / spacing + 0.5f) * spacing - half,
		floorf(Inputs.CameraOrigin.y / spacing + 0.5f) * spacing - half,
		floorf(Inputs.CameraOrigin.z / spacing + 0.5f) * spacing - half,
		spacing };

	const unsigned int totalProbes =
		LUMEN_RC_PROBES_PER_AXIS * LUMEN_RC_PROBES_PER_AXIS * LUMEN_RC_PROBES_PER_AXIS;
	const unsigned int budget = (unsigned int)max(1,
		min(m_Params.RadianceCacheProbesPerFrame, (int)totalProbes));

	FLumenPassParams params = MakeBasePassParams(Inputs);
	params.PassRCParams0 = m_RCVolumeParams0;
	params.PassRCParams1 = {
		(float)LUMEN_RC_PROBES_PER_AXIS, (float)m_RCCursor, (float)budget,
		m_Params.SkySampleMip };

	// ---- 1. プローブトレース (予算分。ラップはシェーダ側の剰余) ----
	TransitionComputeTexture(m_RCAtlas, false);		// -> UAV

	cl->SetPipelineState(SelectTracePSO(m_PSORCTrace.Get(), m_PSORCTraceRT.Get()));
	cl->SetComputeRootDescriptorTable(29 + 4,
		m_RHI->GetGPUDescriptorHandle(m_RCAtlas.UAVIndex));	// u4
	cl->SetComputeRootConstantBufferView(0, WritePassParams(6, params));
	cl->Dispatch(1, 1, budget);

	m_RCCursor = (m_RCCursor + budget) % totalProbes;

	TransitionComputeTexture(m_RCAtlas, true);		// SH 化が読む

	// ---- 2. SH L1 ボリューム化 (全プローブ) ----
	for (int i = 0; i < 3; i++)
	{
		TransitionComputeTexture(m_RCSH[i], false);	// -> UAV
	}

	cl->SetPipelineState(m_PSORCSH.Get());
	cl->SetComputeRootDescriptorTable(1 + 19,
		m_RHI->GetGPUDescriptorHandle(m_RCAtlas.SRVIndex));	// t19
	cl->SetComputeRootDescriptorTable(29 + 4, m_RHI->GetGPUDescriptorHandle(m_RCSH[0].UAVIndex)); // u4
	cl->SetComputeRootDescriptorTable(29 + 5, m_RHI->GetGPUDescriptorHandle(m_RCSH[1].UAVIndex)); // u5
	cl->SetComputeRootDescriptorTable(29 + 6, m_RHI->GetGPUDescriptorHandle(m_RCSH[2].UAVIndex)); // u6
	cl->SetComputeRootConstantBufferView(0, WritePassParams(7, params));

	const unsigned int shGroups = LUMEN_RC_PROBES_PER_AXIS / 8;
	cl->Dispatch(shGroups, shGroups, LUMEN_RC_PROBES_PER_AXIS);

	for (int i = 0; i < 3; i++)
	{
		TransitionComputeTexture(m_RCSH[i], true);	// 半透明パスが読む
	}
}


// ============================================================
//  RenderLumenSceneLighting
//  GDF 再構築 -> Direct -> Radiosity -> Combine -> Radiance Cache
// ============================================================
void FLumenSceneData::RenderLumenSceneLighting(const FLumenFrameInputs& Inputs)
{
	if (!m_Params.bEnabled)
	{
		return;
	}

	// HWRT の有効判定 (TLAS はこのフレームの UpdateTLAS で構築済み)
	// RT バリアント PSO が 1 つも無い (SM 6.5 の cso 不在 / 生成失敗) 場合は
	// TLAS があっても SWRT にしかならないため ACTIVE にしない
	// (ImGui の表示が実態と食い違うのと、TLAS 再構築が無駄になるのを防ぐ)
	const bool bHasAnyRTPSO =
		m_PSODirectLightingRT || m_PSORadiosityRT || m_PSOProbeTraceRT ||
		m_PSOReflectionsRT || m_PSORCTraceRT;

	m_bHWRTActiveThisFrame =
		m_HardwareRayTracing && m_HardwareRayTracing->HasTLAS() &&
		m_Params.bUseHardwareRayTracing && bHasAnyRTPSO;
	m_Stats.bHardwareRayTracingActive = m_bHWRTActiveThisFrame;

	ID3D12GraphicsCommandList* cl = CommandList();

	// ---- 共通バインド ----
	BindCommonComputeState(Inputs);

	// ---- Global Distance Field 再構築 ----
	UpdateGlobalDistanceField(Inputs);

	if (m_NumObjects > 0)
	{
		FLumenPassParams baseParams = MakeBasePassParams(Inputs);

		//======================================================
		// Pass 1: 直接光 (全カード。遮蔽トレース付き)
		//======================================================
		TransitionComputeTexture(m_DirectLightingAtlas, false);	// -> UAV

		cl->SetPipelineState(SelectTracePSO(m_PSODirectLighting.Get(), m_PSODirectLightingRT.Get()));
		cl->SetComputeRootConstantBufferView(0, WritePassParams(0, baseParams));
		cl->Dispatch(LUMEN_CARD_RESOLUTION / 8, LUMEN_CARD_RESOLUTION / 8, MAX_LUMEN_CARDS);

		TransitionComputeTexture(m_DirectLightingAtlas, true);	// Combine が t8 で読む

		//======================================================
		// Pass 2: Radiosity (ラウンドロビン予算。前フレームの
		//         FinalLighting (t10, READ 状態) を採光 -> 多バウンス)
		//======================================================
		{
			TransitionComputeTexture(m_IndirectLightingAtlas, false);	// -> UAV

			cl->SetPipelineState(SelectTracePSO(m_PSORadiosity.Get(), m_PSORadiosityRT.Get()));

			const unsigned int budget =
				min((unsigned int)max(1, m_Params.RadiosityCardsPerFrame), MAX_LUMEN_CARDS);
			const unsigned int start = m_RadiosityCardCursor % MAX_LUMEN_CARDS;
			const unsigned int firstCount = min(budget, MAX_LUMEN_CARDS - start);

			DispatchRadiosityRange(start, firstCount, 1, baseParams);
			DispatchRadiosityRange(0, budget - firstCount, 2, baseParams);	// リングラップ分

			m_RadiosityCardCursor = (start + budget) % MAX_LUMEN_CARDS;

			TransitionComputeTexture(m_IndirectLightingAtlas, true);	// Combine が t9 で読む
		}

		//======================================================
		// Pass 3: 合成 (全カード)
		//   FinalLighting = (Direct + Indirect) * Albedo / π
		//                 + Emissive * EmissiveBoost
		//   ★ ここで Emissive が光源として Surface Cache に乗る ★
		//======================================================
		TransitionComputeTexture(m_FinalLightingAtlas, false);	// -> UAV

		cl->SetPipelineState(m_PSOCombine.Get());
		cl->SetComputeRootConstantBufferView(0, WritePassParams(3, baseParams));
		cl->Dispatch(LUMEN_CARD_RESOLUTION / 8, LUMEN_CARD_RESOLUTION / 8, MAX_LUMEN_CARDS);

		// デファード (t26) / 次フレームの Radiosity (t10) が読む
		TransitionComputeTexture(m_FinalLightingAtlas, true);
	}

	// ---- Radiance Cache (半透明 GI / スカイキャッシュ) ----
	UpdateRadianceCache(Inputs);
}


// ============================================================
//  RenderLumenScreenGI
//  Screen Probe Gather (Setup -> Trace -> Filter -> SH+Temporal ->
//  Integrate) + Reflections。RenderLighting 内 (G-Buffer / 深度が
//  読み取り状態、FinalLighting 確定後) に呼ぶこと。
// ============================================================
void FLumenSceneData::RenderLumenScreenGI(const FLumenFrameInputs& Inputs)
{
	if (!m_Params.bEnabled)
	{
		return;
	}

	const bool bProbeGather = (m_Params.GatherMode == 2);
	const bool bReflections = m_Params.bReflections;

	if (!bProbeGather && !bReflections)
	{
		return;
	}

	ID3D12GraphicsCommandList* cl = CommandList();

	// ライトグリッド等が別のコンピュート RS を設定しているため再バインド
	BindCommonComputeState(Inputs);

	FLumenPassParams params = MakeBasePassParams(Inputs);

	auto bindSRV = [this, cl](unsigned int reg, unsigned int srvIndex)
		{
			cl->SetComputeRootDescriptorTable(1 + reg, m_RHI->GetGPUDescriptorHandle(srvIndex));
		};
	auto bindUAV = [this, cl](unsigned int reg, unsigned int uavIndex)
		{
			cl->SetComputeRootDescriptorTable(29 + reg, m_RHI->GetGPUDescriptorHandle(uavIndex));
		};

	if (bProbeGather)
	{
		FLumenProbeSHSet& curSH = m_ProbeSH[m_ProbeSHFrame];
		FLumenProbeSHSet& prevSH = m_ProbeSH[m_ProbeSHFrame ^ 1];

		const unsigned int probeGroupsX = (m_NumProbesX + 7) / 8;
		const unsigned int probeGroupsY = (m_NumProbesY + 7) / 8;

		//======================================================
		// 1. プローブ配置 (G-Buffer からジオメトリ確定)
		//======================================================
		TransitionComputeTexture(m_ProbeGeo, false);

		cl->SetPipelineState(m_PSOProbeSetup.Get());
		bindUAV(4, m_ProbeGeo.UAVIndex);						// u4
		cl->SetComputeRootConstantBufferView(0, WritePassParams(8, params));
		cl->Dispatch(probeGroupsX, probeGroupsY, 1);

		TransitionComputeTexture(m_ProbeGeo, true);

		//======================================================
		// 2. プローブトレース (hemi-octahedral 8x8:
		//    スクリーン -> SDF/HWRT -> スカイ)
		//======================================================
		TransitionComputeTexture(m_ProbeTraceRadiance, false);

		cl->SetPipelineState(SelectTracePSO(m_PSOProbeTrace.Get(), m_PSOProbeTraceRT.Get()));
		bindSRV(19, m_ProbeGeo.SRVIndex);						// t19
		bindUAV(4, m_ProbeTraceRadiance.UAVIndex);				// u4
		cl->SetComputeRootConstantBufferView(0, WritePassParams(9, params));
		cl->Dispatch(m_NumProbesX, m_NumProbesY, 1);

		TransitionComputeTexture(m_ProbeTraceRadiance, true);

		//======================================================
		// 3. 空間フィルタ (3x3 プローブ近傍)
		//======================================================
		TransitionComputeTexture(m_ProbeFilteredRadiance, false);

		cl->SetPipelineState(m_PSOProbeFilter.Get());
		bindSRV(19, m_ProbeGeo.SRVIndex);						// t19
		bindSRV(20, m_ProbeTraceRadiance.SRVIndex);				// t20
		bindUAV(4, m_ProbeFilteredRadiance.UAVIndex);			// u4
		cl->SetComputeRootConstantBufferView(0, WritePassParams(10, params));
		cl->Dispatch(m_NumProbesX, m_NumProbesY, 1);

		TransitionComputeTexture(m_ProbeFilteredRadiance, true);

		//======================================================
		// 4. SH L1 射影 + テンポラル蓄積 (前フレームをリプロジェクション)
		//======================================================
		TransitionComputeTexture(curSH.SHR, false);
		TransitionComputeTexture(curSH.SHG, false);
		TransitionComputeTexture(curSH.SHB, false);
		TransitionComputeTexture(curSH.Aux, false);
		TransitionComputeTexture(prevSH.SHR, true);
		TransitionComputeTexture(prevSH.SHG, true);
		TransitionComputeTexture(prevSH.SHB, true);
		TransitionComputeTexture(prevSH.Aux, true);

		cl->SetPipelineState(m_PSOProbeSH.Get());
		bindSRV(19, m_ProbeGeo.SRVIndex);						// t19
		bindSRV(20, m_ProbeFilteredRadiance.SRVIndex);			// t20
		bindSRV(21, prevSH.SHR.SRVIndex);						// t21
		bindSRV(22, prevSH.SHG.SRVIndex);						// t22
		bindSRV(23, prevSH.SHB.SRVIndex);						// t23
		bindSRV(24, prevSH.Aux.SRVIndex);						// t24
		bindUAV(4, curSH.SHR.UAVIndex);							// u4
		bindUAV(5, curSH.SHG.UAVIndex);							// u5
		bindUAV(6, curSH.SHB.UAVIndex);							// u6
		bindUAV(7, curSH.Aux.UAVIndex);							// u7
		cl->SetComputeRootConstantBufferView(0, WritePassParams(11, params));
		cl->Dispatch(probeGroupsX, probeGroupsY, 1);

		TransitionComputeTexture(curSH.SHR, true);
		TransitionComputeTexture(curSH.SHG, true);
		TransitionComputeTexture(curSH.SHB, true);
		TransitionComputeTexture(curSH.Aux, true);

		//======================================================
		// 5. フル解像度積分 -> DiffuseIndirect (t28)
		//======================================================
		TransitionComputeTexture(m_DiffuseIndirect, false);

		cl->SetPipelineState(m_PSOProbeIntegrate.Get());
		bindSRV(19, m_ProbeGeo.SRVIndex);						// t19
		bindSRV(21, curSH.SHR.SRVIndex);						// t21
		bindSRV(22, curSH.SHG.SRVIndex);						// t22
		bindSRV(23, curSH.SHB.SRVIndex);						// t23
		bindSRV(24, curSH.Aux.SRVIndex);						// t24
		cl->SetComputeRootConstantBufferView(0, WritePassParams(12, params));
		bindUAV(4, m_DiffuseIndirect.UAVIndex);					// u4

		const unsigned int screenGroupsX = (Inputs.ScreenWidth + 7) / 8;
		const unsigned int screenGroupsY = (Inputs.ScreenHeight + 7) / 8;
		cl->Dispatch(screenGroupsX, screenGroupsY, 1);

		TransitionComputeTexture(m_DiffuseIndirect, true);		// デファードが t28 で読む

		m_ProbeSHFrame ^= 1;	// ピンポン
	}

	//======================================================
	// Reflections (t29)
	//======================================================
	if (bReflections)
	{
		TransitionComputeTexture(m_ReflectionTexture, false);

		cl->SetPipelineState(SelectTracePSO(m_PSOReflections.Get(), m_PSOReflectionsRT.Get()));
		bindSRV(19, Inputs.GBufferBSRVIndex);					// t19 (ラフネス)
		bindUAV(4, m_ReflectionTexture.UAVIndex);				// u4
		cl->SetComputeRootConstantBufferView(0, WritePassParams(13, params));

		const unsigned int screenGroupsX = (Inputs.ScreenWidth + 7) / 8;
		const unsigned int screenGroupsY = (Inputs.ScreenHeight + 7) / 8;
		cl->Dispatch(screenGroupsX, screenGroupsY, 1);

		TransitionComputeTexture(m_ReflectionTexture, true);	// デファードが t29 で読む
	}
}


// ============================================================
//  BindLumenResources (デファード: t24-t32)
// ============================================================
void FLumenSceneData::BindLumenResources()
{
	m_RHI->BindRootTableBySRVIndex(
		(unsigned int)RenderManager::TEXTURE_TYPE::LUMEN_SCENE_OBJECTS,
		m_ObjectBufferSRVIndex[m_BufferFrame]);
	m_RHI->BindRootTableBySRVIndex(
		(unsigned int)RenderManager::TEXTURE_TYPE::LUMEN_CARDS,
		m_CardBufferSRVIndex[m_BufferFrame]);
	m_RHI->BindRootTableBySRVIndex(
		(unsigned int)RenderManager::TEXTURE_TYPE::LUMEN_FINAL_LIGHTING,
		m_FinalLightingAtlas.SRVIndex);
	m_RHI->BindRootTableBySRVIndex(
		(unsigned int)RenderManager::TEXTURE_TYPE::LUMEN_DEPTH_ATLAS,
		m_DepthAtlasSRVIndex);
	m_RHI->BindRootTableBySRVIndex(
		(unsigned int)RenderManager::TEXTURE_TYPE::LUMEN_DIFFUSE_INDIRECT,
		m_DiffuseIndirect.SRVIndex);
	m_RHI->BindRootTableBySRVIndex(
		(unsigned int)RenderManager::TEXTURE_TYPE::LUMEN_REFLECTIONS,
		m_ReflectionTexture.SRVIndex);

	BindTranslucencyResources();
}


// ============================================================
//  BindTranslucencyResources (t30-t32: Radiance Cache SH)
// ============================================================
void FLumenSceneData::BindTranslucencyResources()
{
	m_RHI->BindRootTableBySRVIndex(
		(unsigned int)RenderManager::TEXTURE_TYPE::LUMEN_RC_SH_R, m_RCSH[0].SRVIndex);
	m_RHI->BindRootTableBySRVIndex(
		(unsigned int)RenderManager::TEXTURE_TYPE::LUMEN_RC_SH_G, m_RCSH[1].SRVIndex);
	m_RHI->BindRootTableBySRVIndex(
		(unsigned int)RenderManager::TEXTURE_TYPE::LUMEN_RC_SH_B, m_RCSH[2].SRVIndex);
}


// ============================================================
//  FillLumenConstant (b6)
// ============================================================
void FLumenSceneData::FillLumenConstant(LUMEN_CONSTANT& Out) const
{
	const bool bEnabled = m_Params.bEnabled;

	Out.NumLumenObjects = m_NumObjects;
	Out.LumenGatherMode = bEnabled ? (unsigned int)max(0, min(2, m_Params.GatherMode)) : 0u;
	Out.bLumenScreenGI = (Out.LumenGatherMode == 1u) ? 1u : 0u;
	Out.LumenNumScreenCones = (unsigned int)max(1, min(8, m_Params.NumScreenCones));
	Out.LumenDebugMode = m_Params.DebugMode;

	Out.LumenGIIntensity = m_Params.GIIntensity;
	Out.LumenMaxTraceDistance = m_Params.MaxTraceDistance;

	// 半球を N コーンに分割したときのコーン半角 tan
	// (立体角 2π/N -> cosθ = 1 - 1/N)
	const float coneCos = fmaxf(1.0f - 1.0f / (float)Out.LumenNumScreenCones, 0.1f);
	Out.LumenConeTanAngle = sqrtf(fmaxf(1.0f - coneCos * coneCos, 0.0f)) / coneCos;

	Out.LumenSkyOcclusionStrength = m_Params.SkyOcclusionStrength;
	Out.LumenSurfaceBias = m_Params.SurfaceBias;

	Out.bLumenReflections = (bEnabled && m_Params.bReflections) ? 1u : 0u;
	Out.LumenReflectionMaxRoughness = m_Params.ReflectionMaxRoughness;
	Out.LumenReflectionIntensity = m_Params.ReflectionIntensity;

	Out.bLumenTranslucencyGI =
		(bEnabled && m_Params.bRadianceCache && m_Params.bTranslucencyGI) ? 1u : 0u;
	Out.LumenTranslucencyGIIntensity = m_Params.TranslucencyGIIntensity;

	const float spacing = fmaxf(m_Params.RadianceCacheSpacing, 0.1f);
	Out.LumenRadianceCacheParams0 = m_RCVolumeParams0;
	Out.LumenRadianceCacheParams1 = {
		(float)LUMEN_RC_PROBES_PER_AXIS,
		(bEnabled && m_Params.bRadianceCache) ? 1.0f : 0.0f,
		1.0f / spacing,
		0.0f };
}
