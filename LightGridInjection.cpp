#include "Main.h"
#include "RenderManager.h"
#include "LightGridInjection.h"
#include "SceneRenderer.h"
#include "Scene.h"
#include "LightRendering.h"
#include "D3DX12.h"
#include <fstream>
#include <vector>
#include <cstring>
#include <cmath>

// ============================================================
//  FLightGridInjection : タイルドライトカリング (compute).
// ============================================================

FLightGridInjection::FLightGridInjection(RenderManager* owner)
	: m_Owner(owner)
{
}

FLightGridInjection::~FLightGridInjection()
{
	for (int i = 0; i < 2; ++i)
	{
		if (m_ParamBuffer[i] && m_ParamPtr[i])
			m_ParamBuffer[i]->Unmap(0, nullptr);
	}
}

ID3D12Device* FLightGridInjection::Device()
{
	return m_Owner->GetDevice();
}

ID3D12GraphicsCommandList* FLightGridInjection::CommandList()
{
	return m_Owner->GetGraphicsCommandList();
}

ComPtr<ID3D12PipelineState> FLightGridInjection::CreateComputePipeline(const char* csoFile)
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
	desc.pRootSignature = m_RootSignature.Get();
	desc.CS.pShaderBytecode = cs.data();
	desc.CS.BytecodeLength = cs.size();

	ComPtr<ID3D12PipelineState> pso;
	HRESULT hr = Device()->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pso));
	assert(SUCCEEDED(hr));
	return pso;
}

// ------------------------------------------------------------
//  GetLightGridZParams 相当。
//  指数 Z スライス分布 (B, O, S):
//    Slice = log2(Depth * B + O) * S
//  スライスが近景に密集しすぎないよう near を 9.5cm (= 0.095m)
//  押し出す (0.095 * 100)。far ちょうどが
//  最終スライス (LIGHT_GRID_SIZE_Z - 1) の開始境界に一致する。
// ------------------------------------------------------------
static XMFLOAT3 ComputeLightGridZParams(float NearPlane, float FarPlane)
{
	const float NearOffset = 0.095f;	// [m]
	const float S = 4.05f;				// スライス分布定数

	float N = NearPlane + NearOffset;
	float F = (FarPlane > N + 1.0f) ? FarPlane : (N + 1.0f);

	float O = (F - N * exp2f((float)(LIGHT_GRID_SIZE_Z - 1) / S)) / (F - N);
	float B = (1.0f - O) / N;

	return XMFLOAT3(B, O, S);
}

void FLightGridInjection::FillForwardLightData(FORWARD_LIGHT_CONSTANT& Out, float NearPlane, float FarPlane) const
{
	Out.NumGridCells = m_NumCells;
	Out.CulledGridSizeX = m_GridSizeX;
	Out.CulledGridSizeY = m_GridSizeY;
	Out.CulledGridSizeZ = m_GridSizeZ;
	Out.LightGridPixelSizeShift = LIGHT_GRID_PIXEL_SIZE_SHIFT;
	Out.MaxCulledLightsPerCell = MAX_CULLED_LIGHTS_PER_CELL;
	Out.LightGridZParams = ComputeLightGridZParams(NearPlane, FarPlane);
	Out.LightGridDebugMode = m_Params.DebugMode;
	Out.bUseLightGrid = m_Params.bUseLightGrid ? 1u : 0u;
}

void FLightGridInjection::SetViewSize(unsigned int Width, unsigned int Height)
{
	Width = (Width < 1u) ? 1u : Width;
	Height = (Height < 1u) ? 1u : Height;

	unsigned int gridX = (Width + LIGHT_GRID_PIXEL_SIZE - 1) / LIGHT_GRID_PIXEL_SIZE;
	unsigned int gridY = (Height + LIGHT_GRID_PIXEL_SIZE - 1) / LIGHT_GRID_PIXEL_SIZE;

	// 容量超過は設計上起きない (R <= 2 x O)。Release では容量へ丸めて範囲外書き込みを防ぐ
	assert(gridX <= m_CapacityGridX && gridY <= m_CapacityGridY && "FLightGridInjection::SetViewSize exceeds capacity");
	if (gridX > m_CapacityGridX || gridY > m_CapacityGridY)
	{
		OutputDebugStringA("[LightGrid] SetViewSize exceeds the allocated capacity; clamped\n");
		gridX = (gridX > m_CapacityGridX) ? m_CapacityGridX : gridX;
		gridY = (gridY > m_CapacityGridY) ? m_CapacityGridY : gridY;
	}

	m_ViewWidth = Width;
	m_ViewHeight = Height;
	m_GridSizeX = gridX;
	m_GridSizeY = gridY;
	m_GridSizeZ = LIGHT_GRID_SIZE_Z;
	m_NumCells = m_GridSizeX * m_GridSizeY * m_GridSizeZ;
}

void FLightGridInjection::Init(unsigned int CapacityWidth, unsigned int CapacityHeight)
{
	// ------------------------------------------------------------
	//  容量 (CapacityWidth x CapacityHeight のスクリーン分のセル)。
	//  全バッファはこの容量で一度だけ確保し、フレーム毎の次元は SetViewSize が決める
	//  (1080p 出力 x 2 = 3840x2160 -> 60 x 34 x 32 = 65280 セル)
	// ------------------------------------------------------------
	CapacityWidth = (CapacityWidth < 1u) ? 1u : CapacityWidth;
	CapacityHeight = (CapacityHeight < 1u) ? 1u : CapacityHeight;
	m_CapacityGridX = (CapacityWidth + LIGHT_GRID_PIXEL_SIZE - 1) / LIGHT_GRID_PIXEL_SIZE;
	m_CapacityGridY = (CapacityHeight + LIGHT_GRID_PIXEL_SIZE - 1) / LIGHT_GRID_PIXEL_SIZE;
	m_CapacityCells = m_CapacityGridX * m_CapacityGridY * LIGHT_GRID_SIZE_Z;

	// 次元の既定値は容量いっぱい (呼び出し側が直後に SetViewSize で今フレームの値を入れる)
	SetViewSize(CapacityWidth, CapacityHeight);

	const unsigned int capacityCells = m_CapacityCells;
	const unsigned int maxLinks = capacityCells * MAX_CULLED_LIGHTS_PER_CELL;

	// ------------------------------------------------------------
	//  コンピュートルートシグネチャ (グラフィックス RS から独立):
	//   [0] CBV  b0 (FLightGridParams)
	//   [1] SRV table t0 (ForwardLightBuffer)
	//   [2..6] UAV table u0..u4
	//  デスクリプタはフリーリストから個別確保され連続性が無いため、
	//  1 テーブル = 1 デスクリプタで分割する (AutoExposure と同じ)。
	// ------------------------------------------------------------
	{
		D3D12_DESCRIPTOR_RANGE rangeSRV{};
		rangeSRV.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
		rangeSRV.NumDescriptors = 1;
		rangeSRV.BaseShaderRegister = 0; // t0
		rangeSRV.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

		D3D12_DESCRIPTOR_RANGE rangeUAV[5]{};
		for (int i = 0; i < 5; ++i)
		{
			rangeUAV[i].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
			rangeUAV[i].NumDescriptors = 1;
			rangeUAV[i].BaseShaderRegister = i; // u0..u4
			rangeUAV[i].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
		}

		D3D12_ROOT_PARAMETER params[7]{};
		params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
		params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
		params[0].Descriptor.ShaderRegister = 0;
		params[0].Descriptor.RegisterSpace = 0;

		params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
		params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
		params[1].DescriptorTable.NumDescriptorRanges = 1;
		params[1].DescriptorTable.pDescriptorRanges = &rangeSRV;

		for (int i = 0; i < 5; ++i)
		{
			params[2 + i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
			params[2 + i].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
			params[2 + i].DescriptorTable.NumDescriptorRanges = 1;
			params[2 + i].DescriptorTable.pDescriptorRanges = &rangeUAV[i];
		}

		D3D12_ROOT_SIGNATURE_DESC rs{};
		rs.NumParameters = _countof(params);
		rs.pParameters = params;
		rs.NumStaticSamplers = 0;
		rs.pStaticSamplers = nullptr;
		rs.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

		ComPtr<ID3DBlob> blob, err;
		HRESULT hr = D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err);
		assert(SUCCEEDED(hr));
		hr = Device()->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
			IID_PPV_ARGS(&m_RootSignature));
		assert(SUCCEEDED(hr));
	}

	m_PSOInjection = CreateComputePipeline("Shader/cso/LightGridInjection_CS.cso");
	m_PSOCompact = CreateComputePipeline("Shader/cso/LightGridCompact_CS.cso");

	// ------------------------------------------------------------
	//  バッファ生成ヘルパ (DEFAULT ヒープ, UAV 許可, 初期 UAV ステート)
	// ------------------------------------------------------------
	auto createUAVBuffer = [&](UINT64 size, const wchar_t* name) -> ComPtr<ID3D12Resource>
		{
			D3D12_HEAP_PROPERTIES prop{};
			prop.Type = D3D12_HEAP_TYPE_DEFAULT;

			D3D12_RESOURCE_DESC d{};
			d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
			d.Width = size;
			d.Height = 1;
			d.DepthOrArraySize = 1;
			d.MipLevels = 1;
			d.Format = DXGI_FORMAT_UNKNOWN;
			d.SampleDesc.Count = 1;
			d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
			d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

			ComPtr<ID3D12Resource> resource;
			HRESULT hr = Device()->CreateCommittedResource(&prop, D3D12_HEAP_FLAG_NONE,
				&d, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
				IID_PPV_ARGS(&resource));
			assert(SUCCEEDED(hr));
			resource->SetName(name);
			return resource;
		};

	auto createStructuredUAV = [&](ID3D12Resource* resource, unsigned int numElements, unsigned int stride) -> unsigned int
		{
			unsigned int index = m_Owner->AllocateDescriptor();
			D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
			uav.Format = DXGI_FORMAT_UNKNOWN;
			uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
			uav.Buffer.FirstElement = 0;
			uav.Buffer.NumElements = numElements;
			uav.Buffer.StructureByteStride = stride;
			uav.Buffer.CounterOffsetInBytes = 0;
			uav.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_NONE;
			Device()->CreateUnorderedAccessView(resource, nullptr, &uav,
				m_Owner->GetCPUDescriptorHandle(index));
			return index;
		};

	auto createStructuredSRV = [&](ID3D12Resource* resource, unsigned int numElements, unsigned int stride) -> unsigned int
		{
			unsigned int index = m_Owner->AllocateDescriptor();
			D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
			srv.Format = DXGI_FORMAT_UNKNOWN;
			srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
			srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
			srv.Buffer.FirstElement = 0;
			srv.Buffer.NumElements = numElements;
			srv.Buffer.StructureByteStride = stride;
			srv.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
			Device()->CreateShaderResourceView(resource, &srv,
				m_Owner->GetCPUDescriptorHandle(index));
			return index;
		};

	// ------------------------------------------------------------
	//  中間バッファ (常時 UNORDERED_ACCESS)
	// ------------------------------------------------------------
	m_StartOffsetGrid = createUAVBuffer((UINT64)capacityCells * sizeof(unsigned int), L"LightGridStartOffset");
	m_StartOffsetUAVIndex = createStructuredUAV(m_StartOffsetGrid.Get(), capacityCells, sizeof(unsigned int));

	m_CulledLightLinks = createUAVBuffer((UINT64)maxLinks * 8, L"LightGridCulledLightLinks");
	m_LinksUAVIndex = createStructuredUAV(m_CulledLightLinks.Get(), maxLinks, 8); // uint2

	m_Allocator = createUAVBuffer(2 * sizeof(unsigned int), L"LightGridAllocator");

	// Raw (ByteAddress) UAV。InterlockedAdd で 2 つのカウンタを回す
	// (shader-visible 側と下の CPU 専用クリアヒープ側の 2 つのビューを、この同じ desc から作る)
	D3D12_UNORDERED_ACCESS_VIEW_DESC allocatorUAVDesc{};
	allocatorUAVDesc.Format = DXGI_FORMAT_R32_TYPELESS;
	allocatorUAVDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
	allocatorUAVDesc.Buffer.FirstElement = 0;
	allocatorUAVDesc.Buffer.NumElements = 2;
	allocatorUAVDesc.Buffer.StructureByteStride = 0;
	allocatorUAVDesc.Buffer.CounterOffsetInBytes = 0;
	allocatorUAVDesc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;

	m_AllocatorUAVIndex = m_Owner->AllocateDescriptor();
	Device()->CreateUnorderedAccessView(m_Allocator.Get(), nullptr, &allocatorUAVDesc,
		m_Owner->GetCPUDescriptorHandle(m_AllocatorUAVIndex));

	// ------------------------------------------------------------
	//  出力バッファ (デファードパスが t19/t20 で読む)
	// ------------------------------------------------------------
	m_NumCulledLightsGrid = createUAVBuffer((UINT64)capacityCells * 2 * sizeof(unsigned int), L"LightGridNumCulledLights");
	m_NumCulledUAVIndex = createStructuredUAV(m_NumCulledLightsGrid.Get(), capacityCells * 2, sizeof(unsigned int));
	m_NumCulledSRVIndex = createStructuredSRV(m_NumCulledLightsGrid.Get(), capacityCells * 2, sizeof(unsigned int));

	m_CulledLightDataGrid = createUAVBuffer((UINT64)maxLinks * sizeof(unsigned int), L"LightGridCulledLightData");
	m_DataGridUAVIndex = createStructuredUAV(m_CulledLightDataGrid.Get(), maxLinks, sizeof(unsigned int));
	m_DataGridSRVIndex = createStructuredSRV(m_CulledLightDataGrid.Get(), maxLinks, sizeof(unsigned int));

	// ------------------------------------------------------------
	//  ClearUnorderedAccessViewUint 用 CPU 専用ヒープ (アロケータ)
	// ------------------------------------------------------------
	{
		D3D12_DESCRIPTOR_HEAP_DESC hd{};
		hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
		hd.NumDescriptors = 1; // slot0 = allocator raw UAV
		hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE; // CPU-visible only
		HRESULT hr = Device()->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&m_ClearHeap));
		assert(SUCCEEDED(hr));

		Device()->CreateUnorderedAccessView(m_Allocator.Get(), nullptr, &allocatorUAVDesc,
			m_ClearHeap->GetCPUDescriptorHandleForHeapStart());
	}

	// ------------------------------------------------------------
	//  b0 アップロードバッファ x2 (FLightGridParams, 256 アライン)
	// ------------------------------------------------------------
	{
		D3D12_HEAP_PROPERTIES prop{};
		prop.Type = D3D12_HEAP_TYPE_UPLOAD;

		D3D12_RESOURCE_DESC d{};
		d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		d.Width = (sizeof(FLightGridParams) + 255) & ~255u;
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
			m_ParamBuffer[i]->SetName(L"LightGridParams");
			m_ParamBuffer[i]->Map(0, nullptr, &m_ParamPtr[i]);
		}
	}
}

void FLightGridInjection::Dispatch(const VIEW_CONSTANT& ViewConstant,
	unsigned int lightBufferSRVIndex,
	unsigned int numLocalLights)
{
	ID3D12GraphicsCommandList* cl = CommandList();
	ID3D12DescriptorHeap* heap = m_Owner->GetSRVDescriptorHeap();
	cl->SetDescriptorHeaps(1, &heap);

	// ---- 出力バッファを UAV へ戻す (前フレームは SRV で終わっている) ----
	if (m_bOutputsInSRVState)
	{
		D3D12_RESOURCE_BARRIER toUAV[2] =
		{
			CD3DX12_RESOURCE_BARRIER::Transition(m_NumCulledLightsGrid.Get(),
				D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
				D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
			CD3DX12_RESOURCE_BARRIER::Transition(m_CulledLightDataGrid.Get(),
				D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
				D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
		};
		cl->ResourceBarrier(_countof(toUAV), toUAV);
	}

	// ---- b0 パラメータ (ダブルバッファをフリップして書く) ----
	m_ParamFrame ^= 1;

	FLightGridParams p{};
	p.ViewMatrix = ViewConstant.View;					// 転置済みのまま渡す (mul(v,M) 規約)
	p.CulledGridSizeX = m_GridSizeX;
	p.CulledGridSizeY = m_GridSizeY;
	p.CulledGridSizeZ = m_GridSizeZ;
	p.NumLocalLights = (numLocalLights < MAX_LOCAL_LIGHTS) ? numLocalLights : MAX_LOCAL_LIGHTS;
	p.LightGridZParams = ComputeLightGridZParams(ViewConstant.NearFar.x, ViewConstant.NearFar.y);
	p.LightGridPixelSize = LIGHT_GRID_PIXEL_SIZE;
	// 対称透視射影の対角成分は転置の影響を受けない
	p.InvProjScaleX = 1.0f / ViewConstant.Projection._11;
	p.InvProjScaleY = 1.0f / ViewConstant.Projection._22;
	// 今フレームのレンダー解像度 (SetViewSize)。バックバッファではない
	p.ScreenWidth = (float)m_ViewWidth;
	p.ScreenHeight = (float)m_ViewHeight;
	p.MaxCulledLightsPerCell = MAX_CULLED_LIGHTS_PER_CELL;
	// リンク / データの上限は今フレームのセル数で決める (容量確保でも従来と同値)
	p.MaxCulledLightLinks = m_NumCells * MAX_CULLED_LIGHTS_PER_CELL;
	p.CulledLightDataCapacity = m_NumCells * MAX_CULLED_LIGHTS_PER_CELL;
	p.NearPlane = ViewConstant.NearFar.x;
	std::memcpy(m_ParamPtr[m_ParamFrame], &p, sizeof(p));

	// ---- アロケータ ([0]=NextLink, [1]=NextData) をゼロクリア ----
	{
		const UINT zero[4] = { 0, 0, 0, 0 };
		cl->ClearUnorderedAccessViewUint(
			m_Owner->GetGPUDescriptorHandle(m_AllocatorUAVIndex),	// shader-visible
			m_ClearHeap->GetCPUDescriptorHandleForHeapStart(),		// non-shader-visible
			m_Allocator.Get(), zero, 0, nullptr);
		cl->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::UAV(m_Allocator.Get()));
	}

	// ---- ルートシグネチャ + 共通バインド (両パスで共有) ----
	cl->SetComputeRootSignature(m_RootSignature.Get());
	cl->SetComputeRootConstantBufferView(0, m_ParamBuffer[m_ParamFrame]->GetGPUVirtualAddress());
	cl->SetComputeRootDescriptorTable(1, m_Owner->GetGPUDescriptorHandle(lightBufferSRVIndex));	// t0
	cl->SetComputeRootDescriptorTable(2, m_Owner->GetGPUDescriptorHandle(m_StartOffsetUAVIndex));	// u0
	cl->SetComputeRootDescriptorTable(3, m_Owner->GetGPUDescriptorHandle(m_LinksUAVIndex));		// u1
	cl->SetComputeRootDescriptorTable(4, m_Owner->GetGPUDescriptorHandle(m_AllocatorUAVIndex));	// u2
	cl->SetComputeRootDescriptorTable(5, m_Owner->GetGPUDescriptorHandle(m_NumCulledUAVIndex));	// u3
	cl->SetComputeRootDescriptorTable(6, m_Owner->GetGPUDescriptorHandle(m_DataGridUAVIndex));	// u4

	const unsigned int TG = 4;	// THREADGROUP_SIZE (4x4x4)
	UINT gx = (m_GridSizeX + TG - 1) / TG;
	UINT gy = (m_GridSizeY + TG - 1) / TG;
	UINT gz = (m_GridSizeZ + TG - 1) / TG;

	// ---- Pass 1 : injection (セルごとのリンクリスト構築) ----
	{
		cl->SetPipelineState(m_PSOInjection.Get());
		cl->Dispatch(gx, gy, gz);

		D3D12_RESOURCE_BARRIER uavDone[3] =
		{
			CD3DX12_RESOURCE_BARRIER::UAV(m_StartOffsetGrid.Get()),
			CD3DX12_RESOURCE_BARRIER::UAV(m_CulledLightLinks.Get()),
			CD3DX12_RESOURCE_BARRIER::UAV(m_Allocator.Get()),
		};
		cl->ResourceBarrier(_countof(uavDone), uavDone);
	}

	// ---- Pass 2 : compact (連続領域へ圧縮) ----
	{
		cl->SetPipelineState(m_PSOCompact.Get());
		cl->Dispatch(gx, gy, gz);

		D3D12_RESOURCE_BARRIER uavDone[2] =
		{
			CD3DX12_RESOURCE_BARRIER::UAV(m_NumCulledLightsGrid.Get()),
			CD3DX12_RESOURCE_BARRIER::UAV(m_CulledLightDataGrid.Get()),
		};
		cl->ResourceBarrier(_countof(uavDone), uavDone);
	}

	// ---- 出力を SRV (t19/t20) ステートへ ----
	{
		D3D12_RESOURCE_BARRIER toSRV[2] =
		{
			CD3DX12_RESOURCE_BARRIER::Transition(m_NumCulledLightsGrid.Get(),
				D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
				D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
			CD3DX12_RESOURCE_BARRIER::Transition(m_CulledLightDataGrid.Get(),
				D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
				D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
		};
		cl->ResourceBarrier(_countof(toSRV), toSRV);
	}

	m_bOutputsInSRVState = true;
}


// ============================================================
//  FSceneRenderer::ComputeLightGrid
//  FSceneRenderer::ComputeLightGrid に相当する
//  「ライトデータの詰め込み」部分。ソート済みライトを巡回して
//    - ローカルライト          -> ライトバッファ (t13) [0, NumLocalLights)
//    - ディレクショナルライト  -> ライトバッファ (t13) [NumLocalLights, +NumDirectionalLights)
//                                 + フォワードディレクショナルライトの選択 (b3)
//                                 (ForwardShadingPriority が最大、同値なら最も明るいもの)
//    - Lumen 用ライトバッファ  (描画距離内 かつ bAffectGlobalIllumination。ディレクショナル含む)
//  を作り、FORWARD_LIGHT 定数 (b3) を解決する。
//  グリッド本体の構築 (Injection -> Compact) は RenderLighting 先頭の
//  FLightGridInjection::Dispatch。
// ============================================================

// FLightRenderParameters -> ライトバッファの 1 要素
static void PackForwardLocalLightData(const FLightSceneProxy* Proxy, const FLightRenderParameters& P, FForwardLocalLightData& Out)
{
	Out.LightPositionAndInvRadius = { P.WorldPosition.x, P.WorldPosition.y, P.WorldPosition.z, P.InvRadius };
	// 逆二乗減衰は FalloffExponent = 0 で表す
	const float FalloffExponent = Proxy->IsInverseSquared() ? 0.0f : P.FalloffExponent;
	Out.LightColorAndFalloffExponent = { P.Color.x, P.Color.y, P.Color.z, FalloffExponent };
	Out.LightDirectionAndSpecularScale = { P.Direction.x, P.Direction.y, P.Direction.z, P.SpecularScale };
	Out.SpotAnglesAndSourceRadiusPacked = { P.SpotAngles.x, P.SpotAngles.y, P.SourceRadius, P.SourceLength };
	Out.LightTangentAndSoftSourceRadius = { P.Tangent.x, P.Tangent.y, P.Tangent.z, P.SoftSourceRadius };
	Out.RectBarnDoorAndScales = { P.RectLightBarnCosAngle, P.RectLightBarnLength, P.DiffuseScale, Proxy->GetVolumetricScatteringIntensity() };
	// コンタクトシャドウ: 長さの符号でワールド空間 (負) / スクリーン空間 (正) を表す
	const float ContactShadowLength = Proxy->GetContactShadowLength() * (Proxy->IsContactShadowLengthInWS() ? -1.0f : 1.0f);
	Out.ContactShadowParams = { ContactShadowLength, Proxy->GetContactShadowCastingIntensity(), Proxy->GetContactShadowNonCastingIntensity(), 0.0f };
	Out.LightType = (unsigned int)Proxy->GetLightType();
	Out.Flags = (Proxy->CastsDynamicShadow() ? LIGHT_FLAG_CAST_DYNAMIC_SHADOW : 0u)
		| (Proxy->AffectsTranslucentLighting() ? LIGHT_FLAG_AFFECT_TRANSLUCENT_LIGHTING : 0u)
		| (Proxy->CastsVolumetricShadow() ? LIGHT_FLAG_CAST_VOLUMETRIC_SHADOW : 0u);
	Out.Pad0 = 0u;
	Out.Pad1 = 0u;
}

void FSceneRenderer::ComputeLightGrid(FScene* Scene, const FSortedLightSetSceneInfo& SortedLightSet)
{
	// 書き込み先をフリップ (GPU が読んでいる前フレーム分を避ける)
	m_LightBufferFrame ^= 1;
	FForwardLocalLightData* LightBuffer = m_LightBufferPointer[m_LightBufferFrame];
	FForwardLocalLightData* LumenLightBuffer = m_LumenLightBufferPointer[m_LightBufferFrame];

	// ---- 既定値: ディレクショナルライト不在 = 無光 ----
	// 方向は normalize(0) の NaN を避けるため上向きを入れておく
	m_ViewConstant.DirectionalLightDirection = { 0.0f, 1.0f, 0.0f, 0.0f };
	m_ViewConstant.DirectionalLightColor = { 0.0f, 0.0f, 0.0f, 1.0f };

	FORWARD_LIGHT_CONSTANT& ForwardLightData = m_ForwardLightConstant;
	ForwardLightData.HasDirectionalLight = 0u;
	ForwardLightData.DirectionalLightBufferIndex = 0xFFFFFFFFu;
	ForwardLightData.DirectionalLightFlags = 0u;
	ForwardLightData.DirectionalLightColor = { 0.0f, 0.0f, 0.0f };
	ForwardLightData.DirectionalLightVolumetricScatteringIntensity = 0.0f;
	ForwardLightData.DirectionalLightDirection = { 0.0f, 1.0f, 0.0f };
	ForwardLightData.DirectionalLightSourceRadius = 0.0f;
	ForwardLightData.DirectionalLightSoftSourceRadius = 0.0f;
	ForwardLightData.DirectionalLightSpecularScale = 1.0f;
	ForwardLightData.DirectionalLightDiffuseScale = 1.0f;

	// シャドウ構築 (RenderShadowDepths) 用のプロキシ列を集め直す。
	// m_FrameLocalLights はライトバッファ (t13) と必ず同順になる
	m_FrameLocalLights.clear();
	m_ViewInfo.SelectedForwardDirectionalLightProxy = nullptr;

	const XMFLOAT3 ViewOrigin = { m_ViewConstant.WorldCameraOrigin.x, m_ViewConstant.WorldCameraOrigin.y, m_ViewConstant.WorldCameraOrigin.z };

	// ディレクショナルライトはローカルライトの後ろに並べるので、いったん別に集める
	struct FDirectionalLightEntry
	{
		const FLightSceneProxy* Proxy;
		FLightRenderParameters  Parameters;
	};
	std::vector<FDirectionalLightEntry> DirectionalLights;

	unsigned int NumLocalLights = 0;

	for (const FSortedLightSceneInfo& SortedLightInfo : SortedLightSet.SortedLights)
	{
		const FLightSceneProxy* LightProxy = SortedLightInfo.LightSceneInfo->Proxy;

		FLightRenderParameters LightParameters;
		LightProxy->GetLightShaderParameters(LightParameters);

		const unsigned int LightType = SortedLightInfo.SortKey.Fields.LightType;
		if (LightType == LightType_Point || LightType == LightType_Spot || LightType == LightType_Rect)
		{
			if (NumLocalLights >= MAX_LOCAL_LIGHTS)
			{
				continue;	// バッファ満杯 (固定長)
			}

			// 距離フェード (画面上のサイズ / MaxDrawDistance) を色に掛ける
			const float LightFade = GetLightFadeFactor(ViewOrigin, LightProxy);
			LightParameters.Color.x *= LightFade;
			LightParameters.Color.y *= LightFade;
			LightParameters.Color.z *= LightFade;

			PackForwardLocalLightData(LightProxy, LightParameters, LightBuffer[NumLocalLights]);
			m_FrameLocalLights.push_back(LightProxy);	// t16 との 1:1 対応用
			NumLocalLights++;
		}
		else if (LightType == LightType_Directional && DirectionalLights.size() < MAX_DIRECTIONAL_LIGHTS)
		{
			DirectionalLights.push_back({ LightProxy, LightParameters });
		}
	}

	// ---- ディレクショナルライト: ライトバッファの末尾 + フォワード用 1 灯の選択 ----
	// フォワードシェーディング (半透明 / Volumetric Fog) と CSM が使う 1 灯を選ぶ。
	// 優先度が高いもの、同じなら最も明るいもの (ライトを可視にした順に依らないようにする)
	int   SelectedForwardDirectionalLightPriority = -1;
	float SelectedForwardDirectionalLightIntensitySq = 0.0f;

	for (unsigned int DirectionalIndex = 0; DirectionalIndex < (unsigned int)DirectionalLights.size(); ++DirectionalIndex)
	{
		const FLightSceneProxy* LightProxy = DirectionalLights[DirectionalIndex].Proxy;
		const FLightRenderParameters& LightParameters = DirectionalLights[DirectionalIndex].Parameters;
		const unsigned int BufferIndex = NumLocalLights + DirectionalIndex;

		PackForwardLocalLightData(LightProxy, LightParameters, LightBuffer[BufferIndex]);

		const float LightLuminance = GetLightColorLuminance(LightProxy->GetColor());
		const int LightForwardShadingPriority = LightProxy->GetDirectionalLightForwardShadingPriority();
		if (LightForwardShadingPriority > SelectedForwardDirectionalLightPriority
			|| (LightForwardShadingPriority == SelectedForwardDirectionalLightPriority && LightLuminance > SelectedForwardDirectionalLightIntensitySq))
		{
			SelectedForwardDirectionalLightPriority = LightForwardShadingPriority;
			SelectedForwardDirectionalLightIntensitySq = LightLuminance;
			m_ViewInfo.SelectedForwardDirectionalLightProxy = LightProxy;

			const FForwardLocalLightData& Packed = LightBuffer[BufferIndex];
			ForwardLightData.HasDirectionalLight = 1u;
			ForwardLightData.DirectionalLightBufferIndex = BufferIndex;
			ForwardLightData.DirectionalLightFlags = Packed.Flags;
			ForwardLightData.DirectionalLightColor = LightParameters.Color;
			ForwardLightData.DirectionalLightVolumetricScatteringIntensity = LightProxy->GetVolumetricScatteringIntensity();
			ForwardLightData.DirectionalLightDirection = LightParameters.Direction;
			ForwardLightData.DirectionalLightSourceRadius = LightParameters.SourceRadius;
			ForwardLightData.DirectionalLightSoftSourceRadius = LightParameters.SoftSourceRadius;
			ForwardLightData.DirectionalLightSpecularScale = LightParameters.SpecularScale;
			ForwardLightData.DirectionalLightDiffuseScale = LightParameters.DiffuseScale;

			// b0 (View) の代表ディレクショナルライトも同じ 1 灯。
			// DirectionalLightDirection は「受光面 -> ライト」(LightParameters.Direction と同じ向き)
			m_ViewConstant.DirectionalLightDirection = { LightParameters.Direction.x, LightParameters.Direction.y, LightParameters.Direction.z, 0.0f };
			m_ViewConstant.DirectionalLightColor = { LightParameters.Color.x, LightParameters.Color.y, LightParameters.Color.z, 1.0f };
		}
	}

	const unsigned int NumDirectionalLights = (unsigned int)DirectionalLights.size();

	// ---- Lumen 用ライトリスト ----
	// Surface Cache は画面外も照らされるので、フラスタムではなく描画距離で選ぶ (bOffscreen = true)。
	// GI に寄与しないライトは除き、色に IndirectLightingScale を掛ける。
	// ディレクショナルライトも同じバッファに LightType = Directional の要素として積む
	unsigned int NumLumenLights = 0;
	for (const FLightSceneInfoCompact& LightSceneInfoCompact : Scene->GetLights())
	{
		const FLightSceneInfo* LightSceneInfo = LightSceneInfoCompact.LightSceneInfo;
		if (LightSceneInfo == nullptr)
		{
			continue;
		}

		const FLightSceneProxy* LightProxy = LightSceneInfo->Proxy;
		if (NumLumenLights < MAX_FORWARD_LIGHT_BUFFER_ENTRIES
			&& LightSceneInfo->ShouldRenderLightViewIndependent()
			&& LightSceneInfo->ShouldRenderLight(m_ViewInfo, true)
			&& LightProxy->AffectGlobalIllumination()
			&& LightProxy->GetIndirectLightingScale() > 0.0f)
		{
			FLightRenderParameters LightParameters;
			LightProxy->GetLightShaderParameters(LightParameters);

			const float Scale = (LightProxy->IsLocalLight() ? GetLightFadeFactor(ViewOrigin, LightProxy) : 1.0f)
				* LightProxy->GetIndirectLightingScale();
			LightParameters.Color.x *= Scale;
			LightParameters.Color.y *= Scale;
			LightParameters.Color.z *= Scale;

			PackForwardLocalLightData(LightProxy, LightParameters, LumenLightBuffer[NumLumenLights]);
			NumLumenLights++;
		}
	}
	m_NumLumenLights = NumLumenLights;

	ForwardLightData.NumLocalLights = NumLocalLights;
	ForwardLightData.NumDirectionalLights = NumDirectionalLights;

	m_LightStats.NumLocalLights = (int)NumLocalLights;
	m_LightStats.NumDirectionalLights = (int)NumDirectionalLights;
	m_LightStats.NumLumenLights = (int)NumLumenLights;

	// ---- ライトグリッド (タイルドライトカリング) パラメータ ----
	// VIEW 定数は RenderBasePass 先頭で解決済みなので NearFar を参照できる。
	// グリッド本体の構築 (Dispatch) は RenderLighting 先頭で行う。
	if (m_LightGrid)
	{
		m_LightGrid->FillForwardLightData(
			ForwardLightData,
			m_ViewConstant.NearFar.x, m_ViewConstant.NearFar.y);
	}
}
