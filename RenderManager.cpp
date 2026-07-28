#include "Main.h"
#include "RenderManager.h"

#include "D3DX12.h"
#include "DDSTextureLoader12.h"

#include "ImGUI/imgui.h"
#include "ImGUI/imgui_impl_win32.h"
#include "ImGUI/imgui_impl_dx12.h"


RenderManager* RenderManager::m_Instance = nullptr;


// ============================================================
//  Lifetime
// ============================================================
RenderManager::RenderManager()
{
	m_Instance = this;
	Init();
}

RenderManager::~RenderManager()
{
	/*
	#if defined(_DEBUG)
		// ReportLiveDeviceObjects
		{
			ComPtr<ID3D12DebugDevice> debugInterface;
			if (SUCCEEDED(m_Device->QueryInterface(IID_PPV_ARGS(&debugInterface))))
			{
				debugInterface->ReportLiveDeviceObjects(D3D12_RLDO_DETAIL | D3D12_RLDO_IGNORE_INTERNAL);
			}
		}
	#endif
	*/
}


// ============================================================
//  Initialization
// ============================================================
void RenderManager::Init()
{
	m_WindowMode = true;
	m_WindowHandle = GetWindow();

	RECT rc{};
	GetClientRect(m_WindowHandle, &rc);
	m_BackBufferWidth = rc.right - rc.left;
	m_BackBufferHeight = rc.bottom - rc.top;

	m_Frame[0] = 1;
	m_Frame[1] = 0;
	m_RTIndex = 0;

	InitViewport();
	InitDevice();
	InitCommandObjects();
	InitSwapChain();
	InitBackBufferRTV();
	InitDepthBuffer();
	InitDescriptorHeaps();
	InitImGui();
	InitConstantBuffers();
	InitRootSignature();
	InitPipelines();
}


void RenderManager::InitViewport()
{
	m_Viewport.TopLeftX = 0.0f;
	m_Viewport.TopLeftY = 0.0f;
	m_Viewport.Width = (FLOAT)m_BackBufferWidth;
	m_Viewport.Height = (FLOAT)m_BackBufferHeight;
	m_Viewport.MinDepth = 0.0f;
	m_Viewport.MaxDepth = 1.0f;

	m_ScissorRect.top = 0;
	m_ScissorRect.left = 0;
	m_ScissorRect.right = m_BackBufferWidth;
	m_ScissorRect.bottom = m_BackBufferHeight;
}


void RenderManager::InitDevice()
{
	HRESULT hr;

#if defined(_DEBUG)
	// デバッグレイヤー有効化
	{
		ComPtr<ID3D12Debug1> debugController;
		if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController))))
		{
			debugController->EnableDebugLayer();
			//debugController->SetEnableGPUBasedValidation(true);
		}
	}
	/*
		// DRED
		{
			ComPtr<ID3D12DeviceRemovedExtendedDataSettings1> d3dDredSettings1;
			if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&d3dDredSettings1))))
			{
				d3dDredSettings1->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
				d3dDredSettings1->SetBreadcrumbContextEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
				d3dDredSettings1->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
			}
		}
	*/
#endif

	UINT flag{};
	hr = CreateDXGIFactory2(flag, IID_PPV_ARGS(&m_Factory));
	assert(SUCCEEDED(hr));

	hr = m_Factory->EnumAdapters(0, (IDXGIAdapter**)m_Adapter.GetAddressOf());
	assert(SUCCEEDED(hr));

	hr = D3D12CreateDevice(m_Adapter.Get(), D3D_FEATURE_LEVEL_11_1, IID_PPV_ARGS(&m_Device));
	assert(SUCCEEDED(hr));

#if defined(_DEBUG)
	// デバッグレイヤーのエラーメッセージが出た瞬間にブレークさせる。
	// 不正な API 呼び出しは放置すると後段で D3D12Core 内の
	// アクセス違反 (0xC0000005) という分かりにくい形で落ちるため、
	// 原因の呼び出し箇所そのもので停止するようにする。
	{
		ComPtr<ID3D12InfoQueue> infoQueue;
		if (SUCCEEDED(m_Device.As(&infoQueue)))
		{
			infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, TRUE);
			infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, TRUE);
		}
	}
#endif
}


void RenderManager::InitCommandObjects()
{
	HRESULT hr;

	// コマンドキュー + フェンス
	{
		D3D12_COMMAND_QUEUE_DESC commandQueueDesc{};
		commandQueueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
		commandQueueDesc.Priority = 0;
		commandQueueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
		commandQueueDesc.NodeMask = 0;

		hr = m_Device->CreateCommandQueue(&commandQueueDesc, IID_PPV_ARGS(&m_CommandQueue));
		assert(SUCCEEDED(hr));

		m_FenceEvent = CreateEventEx(nullptr, FALSE, FALSE, EVENT_ALL_ACCESS);
		assert(m_FenceEvent);

		hr = m_Device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_Fence));
		assert(SUCCEEDED(hr));
	}

	// コマンドアロケータ + リスト
	{
		hr = m_Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_GraphicsCommandAllocator[0]));
		assert(SUCCEEDED(hr));
		m_GraphicsCommandAllocator[0]->SetName(L"GraphicsCommandAllocator[0]");

		hr = m_Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_GraphicsCommandAllocator[1]));
		assert(SUCCEEDED(hr));
		m_GraphicsCommandAllocator[1]->SetName(L"GraphicsCommandAllocator[1]");

		hr = m_Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_GraphicsCommandAllocator[0].Get(), nullptr, IID_PPV_ARGS(&m_GraphicsCommandList));
		assert(SUCCEEDED(hr));
		m_GraphicsCommandList->SetName(L"GraphicsCommandList");
	}
}


void RenderManager::InitSwapChain()
{
	DXGI_SWAP_CHAIN_DESC swapChainDesc{};
	swapChainDesc.BufferDesc.Width = m_BackBufferWidth;
	swapChainDesc.BufferDesc.Height = m_BackBufferHeight;
	swapChainDesc.OutputWindow = m_WindowHandle;
	swapChainDesc.Windowed = m_WindowMode;
	swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	swapChainDesc.BufferCount = 2;
	swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
	swapChainDesc.Flags = 0;
	swapChainDesc.BufferDesc.RefreshRate.Numerator = 60;
	swapChainDesc.BufferDesc.RefreshRate.Denominator = 1;
	swapChainDesc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	swapChainDesc.BufferDesc.ScanlineOrdering = DXGI_MODE_SCANLINE_ORDER_UNSPECIFIED;
	swapChainDesc.BufferDesc.Scaling = DXGI_MODE_SCALING_UNSPECIFIED;
	swapChainDesc.SampleDesc.Count = 1;
	swapChainDesc.SampleDesc.Quality = 0;

	ComPtr<IDXGISwapChain> swapChain{};
	HRESULT hr = m_Factory->CreateSwapChain(m_CommandQueue.Get(), &swapChainDesc, &swapChain);
	assert(SUCCEEDED(hr));

	hr = swapChain.As(&m_SwapChain);
	assert(SUCCEEDED(hr));

	m_RTIndex = m_SwapChain->GetCurrentBackBufferIndex();
}


void RenderManager::InitBackBufferRTV()
{
	D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
	heapDesc.NumDescriptors = 2;
	heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
	heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
	heapDesc.NodeMask = 0;

	HRESULT hr = m_Device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&m_RenderTargetDescriptorHeap));
	assert(SUCCEEDED(hr));

	UINT size = m_Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
	for (UINT i = 0; i < 2; ++i)
	{
		hr = m_SwapChain->GetBuffer(i, IID_PPV_ARGS(&m_RenderTarget[i]));
		assert(SUCCEEDED(hr));
		m_RenderTarget[i]->SetName(L"RenderTarget");

		m_RenderTargetHandle[i] = m_RenderTargetDescriptorHeap->GetCPUDescriptorHandleForHeapStart();
		m_RenderTargetHandle[i].ptr += size * i;
		m_Device->CreateRenderTargetView(m_RenderTarget[i].Get(), nullptr, m_RenderTargetHandle[i]);
	}
}


void RenderManager::InitDepthBuffer()
{
	HRESULT hr;

	// デプスバッファ用デスクリプタヒープ (DSV)
	{
		D3D12_DESCRIPTOR_HEAP_DESC descriptorHeapDesc{};
		descriptorHeapDesc.NumDescriptors = 1;
		descriptorHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
		descriptorHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
		descriptorHeapDesc.NodeMask = 0;

		hr = m_Device->CreateDescriptorHeap(&descriptorHeapDesc, IID_PPV_ARGS(&m_DepthBufferDescriptorHeap));
		assert(SUCCEEDED(hr));
	}

	// デプスバッファ生成
	// R32_TYPELESS で生成し、DSV は D32_FLOAT、SRV は R32_FLOAT として読む。
	{
		D3D12_RESOURCE_DESC resourceDesc{};
		resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
		resourceDesc.Width = m_BackBufferWidth;
		resourceDesc.Height = m_BackBufferHeight;
		resourceDesc.DepthOrArraySize = 1;
		resourceDesc.MipLevels = 1;
		resourceDesc.Format = DXGI_FORMAT_R32_TYPELESS;
		resourceDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
		resourceDesc.SampleDesc.Count = 1;
		resourceDesc.SampleDesc.Quality = 0;
		resourceDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

		D3D12_CLEAR_VALUE clearValue{};
		clearValue.Format = DXGI_FORMAT_D32_FLOAT;
		clearValue.DepthStencil.Depth = 1.0f;
		clearValue.DepthStencil.Stencil = 0;

		hr = m_Device->CreateCommittedResource(
			&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
			D3D12_HEAP_FLAG_NONE,
			&resourceDesc,
			D3D12_RESOURCE_STATE_DEPTH_WRITE,
			&clearValue,
			IID_PPV_ARGS(&m_DepthBuffer));
		assert(SUCCEEDED(hr));
		m_DepthBuffer->SetName(L"DepthBuffer");

		D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
		dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
		dsvDesc.Format = DXGI_FORMAT_D32_FLOAT;
		dsvDesc.Texture2D.MipSlice = 0;
		dsvDesc.Flags = D3D12_DSV_FLAG_NONE;

		m_DepthBufferHandle = m_DepthBufferDescriptorHeap->GetCPUDescriptorHandleForHeapStart();
		m_Device->CreateDepthStencilView(m_DepthBuffer.Get(), &dsvDesc, m_DepthBufferHandle);
	}
}


void RenderManager::InitDescriptorHeaps()
{
	// SRV/CBV/UAV ヒープ (シェーダ可視)
	{
		D3D12_DESCRIPTOR_HEAP_DESC desc{};
		desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
		desc.NumDescriptors = SRV_DESCRIPTOR_MAX;
		desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
		desc.NodeMask = 0;

		m_Device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&m_SRVDescriptorHeap));

		for (unsigned int i = 0; i < SRV_DESCRIPTOR_MAX; i++)
			m_SRVDescriptorPool.push_back(i);
	}

	// RTV ヒープ (オフスクリーン用)
	{
		D3D12_DESCRIPTOR_HEAP_DESC desc{};
		desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
		desc.NumDescriptors = RTV_DESCRIPTOR_MAX;
		desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
		desc.NodeMask = 0;

		m_Device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&m_RTVDescriptorHeap));

		for (unsigned int i = 0; i < RTV_DESCRIPTOR_MAX; i++)
			m_RTVDescriptorPool.push_back(i);
	}
}


void RenderManager::InitImGui()
{
	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGui::GetIO();

	//io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
	//io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;

	ImGui::StyleColorsDark();

	// ImGui はフォントアトラス用に SRV ヒープ先頭の 1 枠を使う。
	ImGui_ImplWin32_Init(m_WindowHandle);
	ImGui_ImplDX12_Init(m_Device.Get(), 2,
		DXGI_FORMAT_R8G8B8A8_UNORM, m_SRVDescriptorHeap.Get(),
		m_SRVDescriptorHeap->GetCPUDescriptorHandleForHeapStart(),
		m_SRVDescriptorHeap->GetGPUDescriptorHandleForHeapStart());

	m_SRVDescriptorPool.pop_front();
}


void RenderManager::InitConstantBuffers()
{
	for (int i = 0; i < 2; i++)
	{
		// アップロードヒープに連続した定数バッファ領域を確保
		{
			D3D12_HEAP_PROPERTIES properties{};
			properties.Type = D3D12_HEAP_TYPE_UPLOAD;
			properties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
			properties.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
			properties.CreationNodeMask = 0;
			properties.VisibleNodeMask = 0;

			D3D12_RESOURCE_DESC desc{};
			desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
			desc.Width = CONSTANT_BUFFER_SIZE * CONSTANT_BUFFER_MAX;
			desc.Height = 1;
			desc.DepthOrArraySize = 1;
			desc.MipLevels = 1;
			desc.Format = DXGI_FORMAT_UNKNOWN;
			desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
			desc.SampleDesc.Count = 1;
			desc.SampleDesc.Quality = 0;

			HRESULT hr = m_Device->CreateCommittedResource(&properties,
				D3D12_HEAP_FLAG_NONE,
				&desc,
				D3D12_RESOURCE_STATE_GENERIC_READ,
				nullptr,
				IID_PPV_ARGS(&m_ConstantBuffer[i]));
			assert(SUCCEEDED(hr));
		}

		HRESULT hr = m_ConstantBuffer[i]->Map(0, nullptr, (void**)&m_ConstantBufferPointer[i]);
		assert(SUCCEEDED(hr));

		// 各スロットに CBV を生成し、確保したヒープインデックスを記録
		for (int j = 0; j < CONSTANT_BUFFER_MAX; j++)
		{
			unsigned int index = AllocateSRVSlot();

			D3D12_CONSTANT_BUFFER_VIEW_DESC desc{};
			desc.BufferLocation = m_ConstantBuffer[i]->GetGPUVirtualAddress() + j * CONSTANT_BUFFER_SIZE;
			desc.SizeInBytes = CONSTANT_BUFFER_SIZE;

			D3D12_CPU_DESCRIPTOR_HANDLE handle = OffsetCPUHandle(
				m_SRVDescriptorHeap->GetCPUDescriptorHandleForHeapStart(),
				index, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

			m_Device->CreateConstantBufferView(&desc, handle);
			m_ConstantBufferView[i][j] = index;
		}

		m_ConstantBufferIndex[i] = 0;
	}
}


void RenderManager::InitRootSignature()
{
	const unsigned int ROOT_PARAM_COUNT = (unsigned int)TEXTURE_TYPE::COUNT;
	const unsigned int CBV_COUNT = (unsigned int)CONSTANT_TYPE::SHADOW + 1; // VIEW/PRIMITIVE/MATERIAL/FORWARD_LIGHT/POST_PROCESS/SHADOW

	D3D12_ROOT_PARAMETER  rootParameters[ROOT_PARAM_COUNT]{};
	D3D12_DESCRIPTOR_RANGE range[ROOT_PARAM_COUNT]{};

	// 定数バッファ (b0..b4)
	for (unsigned int i = 0; i < CBV_COUNT; i++)
	{
		range[i].NumDescriptors = 1;
		range[i].BaseShaderRegister = i;
		range[i].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_CBV;
		range[i].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

		rootParameters[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
		rootParameters[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
		rootParameters[i].DescriptorTable.NumDescriptorRanges = 1;
		rootParameters[i].DescriptorTable.pDescriptorRanges = &range[i];
	}

	// テクスチャ (t0..tN)
	for (unsigned int i = CBV_COUNT; i < ROOT_PARAM_COUNT; i++)
	{
		range[i].NumDescriptors = 1;
		range[i].BaseShaderRegister = i - CBV_COUNT;
		range[i].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
		range[i].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

		rootParameters[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
		rootParameters[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
		rootParameters[i].DescriptorTable.NumDescriptorRanges = 1;
		rootParameters[i].DescriptorTable.pDescriptorRanges = &range[i];
	}

	// サンプラー
	D3D12_STATIC_SAMPLER_DESC samplerDesc[3]{};
	// s0: 異方性ラップ (アルベドなど通常テクスチャ用)
	samplerDesc[0].Filter = D3D12_FILTER_ANISOTROPIC;
	samplerDesc[0].AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
	samplerDesc[0].AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
	samplerDesc[0].AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
	samplerDesc[0].MipLODBias = 0.0f;
	samplerDesc[0].MaxAnisotropy = 4;
	samplerDesc[0].ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
	samplerDesc[0].BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
	samplerDesc[0].MinLOD = 0.0f;
	samplerDesc[0].MaxLOD = D3D12_FLOAT32_MAX;
	samplerDesc[0].ShaderRegister = 0;
	samplerDesc[0].RegisterSpace = 0;
	samplerDesc[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

	// s1: 線形クランプ (G-Buffer / IBL / LUT などフルスクリーン参照用)
	samplerDesc[1].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
	samplerDesc[1].AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	samplerDesc[1].AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	samplerDesc[1].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	samplerDesc[1].MipLODBias = 0.0f;
	samplerDesc[1].MaxAnisotropy = 16;
	samplerDesc[1].ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
	samplerDesc[1].BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
	samplerDesc[1].MinLOD = 0.0f;
	samplerDesc[1].MaxLOD = D3D12_FLOAT32_MAX;
	samplerDesc[1].ShaderRegister = 1;
	samplerDesc[1].RegisterSpace = 0;
	samplerDesc[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

	// s2: シャドウ比較サンプラ (SampleCmp 用。バイリニア比較 PCF)
	// ボーダー白 = シャドウマップ外は「影なし」扱い
	samplerDesc[2].Filter = D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
	samplerDesc[2].AddressU = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
	samplerDesc[2].AddressV = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
	samplerDesc[2].AddressW = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
	samplerDesc[2].MipLODBias = 0.0f;
	samplerDesc[2].MaxAnisotropy = 1;
	samplerDesc[2].ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
	samplerDesc[2].BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
	samplerDesc[2].MinLOD = 0.0f;
	samplerDesc[2].MaxLOD = D3D12_FLOAT32_MAX;
	samplerDesc[2].ShaderRegister = 2;
	samplerDesc[2].RegisterSpace = 0;
	samplerDesc[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

	D3D12_ROOT_SIGNATURE_DESC rootSignatureDesc{};
	rootSignatureDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
	rootSignatureDesc.NumParameters = _countof(rootParameters);
	rootSignatureDesc.pParameters = rootParameters;
	rootSignatureDesc.NumStaticSamplers = _countof(samplerDesc);
	rootSignatureDesc.pStaticSamplers = samplerDesc;

	ComPtr<ID3DBlob> blob{};
	ComPtr<ID3DBlob> errorBlob{};
	HRESULT hr = D3D12SerializeRootSignature(&rootSignatureDesc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errorBlob);
	if (FAILED(hr))
	{
		// 失敗理由の本文 (パラメータ数超過など) をそのまま出力する
		OutputDebugStringA("[RenderManager] D3D12SerializeRootSignature failed:\n");
		if (errorBlob && errorBlob->GetBufferPointer())
		{
			OutputDebugStringA((const char*)errorBlob->GetBufferPointer());
		}
		assert(false && "D3D12SerializeRootSignature failed");
		return;
	}

	hr = m_Device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&m_RootSignature));
	if (FAILED(hr))
	{
		char msg[256];
		sprintf_s(msg, "[RenderManager] CreateRootSignature failed (hr=0x%08X)\n", (unsigned int)hr);
		OutputDebugStringA(msg);
		assert(false && "CreateRootSignature failed");
	}
}



void RenderManager::InitPipelines()
{
	const DXGI_FORMAT ldr[] = { DXGI_FORMAT_R8G8B8A8_UNORM };
	const DXGI_FORMAT hdr[] = { DXGI_FORMAT_R16G16B16A16_FLOAT };
	const DXGI_FORMAT depth[] = { DXGI_FORMAT_R32G32_FLOAT };
	// ワールド座標 G-Buffer は持たない (深度から再構築)。
	// RT0 = GBufferC (BaseColor / Substrate では DiffuseAlbedo),
	// RT1 = GBufferA (Normal),
	// RT2 = GBufferB (Metallic/Specular/Roughness/AO),
	// RT3 = SubstrateMaterial0 (Slab パック 0, RGBA32_UINT),
	// RT4 = SubstrateMaterial1 (Slab パック 1, RGBA32_UINT)
	// SceneTextures.cpp の GBuffers vector と枚数 / フォーマット一致必須。
	const DXGI_FORMAT gbuffer[] =
	{
		DXGI_FORMAT_R16G16B16A16_FLOAT,
		DXGI_FORMAT_R16G16B16A16_FLOAT,
		DXGI_FORMAT_R16G16B16A16_FLOAT,
		DXGI_FORMAT_R32G32B32A32_UINT,
		DXGI_FORMAT_R32G32B32A32_UINT,
	};

	m_PipelineState["Unlit"] =
		CreatePipeline("Shader/cso/UnlitVS.cso", "Shader/cso/UnlitPS.cso", ldr, _countof(ldr));

	m_PipelineState["Lit"] =
		CreatePipeline("Shader/cso/LitVS.cso", "Shader/cso/LitPS.cso", hdr, _countof(hdr));

	m_PipelineState["BasePass"] =
		CreatePipeline("Shader/cso/GeometryVS.cso", "Shader/cso/GeometryPS.cso", gbuffer, _countof(gbuffer));

	m_PipelineState["LinearDepth"] =
		CreatePipeline("Shader/cso/DeferredVS.cso", "Shader/cso/LinearDepthPS.cso", depth, _countof(depth));

	m_PipelineState["DeferredLighting"] =
		CreatePipeline("Shader/cso/DeferredVS.cso", "Shader/cso/DeferredPS.cso", hdr, _countof(hdr));

	// Tonemap pass: HDR SceneColor (+bloom) -> Post chain -> SDR back buffer
	m_PipelineState["PostProcessTonemap"] =
		CreatePipeline("Shader/cso/DeferredVS.cso", "Shader/cso/TonemapPS.cso", ldr, _countof(ldr));

	// ---- Bloom passes (all render to HDR R16G16B16A16 mips) ----
	m_PipelineState["PostProcessBloomThreshold"] =
		CreatePipeline("Shader/cso/DeferredVS.cso", "Shader/cso/BloomThresholdPS.cso", hdr, _countof(hdr));
	m_PipelineState["PostProcessBloomDownsample"] =
		CreatePipeline("Shader/cso/DeferredVS.cso", "Shader/cso/BloomDownsamplePS.cso", hdr, _countof(hdr));
	m_PipelineState["PostProcessBloomUpsample"] =
		CreatePipeline("Shader/cso/DeferredVS.cso", "Shader/cso/BloomUpsamplePS.cso", hdr, _countof(hdr));

	// ---- Depth of Field passes ----
	// CoC/prep -> half-res HDR (RGBA16F). Blur passes ping-pong at half
	// res. Composite writes back to full-res HDR SceneColor.
	m_PipelineState["PostProcessDOFCoC"] =
		CreatePipeline("Shader/cso/DeferredVS.cso", "Shader/cso/DOFCoCPS.cso", hdr, _countof(hdr));
	m_PipelineState["PostProcessDOFBlur"] =
		CreatePipeline("Shader/cso/DeferredVS.cso", "Shader/cso/DOFBlurPS.cso", hdr, _countof(hdr));
	m_PipelineState["PostProcessDOFComposite"] =
		CreatePipeline("Shader/cso/DeferredVS.cso", "Shader/cso/DOFCompositePS.cso", hdr, _countof(hdr));

	// ---- Shadow depth pass (深度のみ, RTV なし) ----
	// ラスタライザの定数 + スロープスケールバイアスでシャドウアクネを
	// 抑え、受光側バイアス (深度 / 法線オフセット) と併用する。
	m_PipelineState["ShadowDepth"] =
		CreatePipeline("Shader/cso/ShadowDepthVS.cso", "Shader/cso/ShadowDepthPS.cso", nullptr, 0, 1000, 1.5f);

	// ---- Two Sided ベースパス (bTwoSided: カリング無効) ----
	// GeometryPS 側は SV_IsFrontFace で裏面の法線を反転する。
	m_PipelineState["BasePassTwoSided"] =
		CreatePipeline("Shader/cso/GeometryVS.cso", "Shader/cso/GeometryPS.cso", gbuffer, _countof(gbuffer),
			0, 0.0f, EBlendStatePreset::Opaque, ECullModePreset::None);

	// ---- トランスルーセンシーパス (BLEND_Translucent / BLEND_Additive) ----
	// デファードライティング後の HDR SceneColor へフォワードで合成する
	// (FSceneRenderer::RenderTranslucency)。深度はテストのみ (DepthRead)。
	// シェーダは両モード共通 (TranslucentPS)、ブレンドステートのみ異なる:
	//   Translucent = SrcAlpha / InvSrcAlpha, Additive = SrcAlpha / One
	m_PipelineState["Translucency"] =
		CreatePipeline("Shader/cso/GeometryVS.cso", "Shader/cso/TranslucentPS.cso", hdr, _countof(hdr),
			0, 0.0f, EBlendStatePreset::Translucent, ECullModePreset::Back, EDepthStatePreset::DepthRead);
	m_PipelineState["TranslucencyTwoSided"] =
		CreatePipeline("Shader/cso/GeometryVS.cso", "Shader/cso/TranslucentPS.cso", hdr, _countof(hdr),
			0, 0.0f, EBlendStatePreset::Translucent, ECullModePreset::None, EDepthStatePreset::DepthRead);
	m_PipelineState["TranslucencyAdditive"] =
		CreatePipeline("Shader/cso/GeometryVS.cso", "Shader/cso/TranslucentPS.cso", hdr, _countof(hdr),
			0, 0.0f, EBlendStatePreset::Additive, ECullModePreset::Back, EDepthStatePreset::DepthRead);
	// ---- 半透明深度プリパス (単層トランスルーセンシー) ----
	// プリミティブごとに 2 パスで描く方式 (FSceneRenderer::RenderTranslucency):
	//   1. DepthPrepass: カラー書き込み無効 + 深度書き込みで、その
	//      プリミティブの「最前面」深度だけを深度バッファへ焼く
	//   2. Equal: EQUAL 比較 + 深度書き込み無効で、最前面に一致する
	//      フラグメントのみ着色 (通常の SrcAlpha / InvSrcAlpha 合成)
	// これによりメッシュ内部の背面側ポリゴンが後から手前を上書きする
	// 自己前後逆転 (三角形順依存) が構造的に発生しなくなる。
	m_PipelineState["TranslucencyDepthPrepass"] =
		CreatePipeline("Shader/cso/GeometryVS.cso", "Shader/cso/TranslucentPS.cso", hdr, _countof(hdr),
			0, 0.0f, EBlendStatePreset::NoColorWrite, ECullModePreset::Back, EDepthStatePreset::DepthWrite);
	m_PipelineState["TranslucencyDepthPrepassTwoSided"] =
		CreatePipeline("Shader/cso/GeometryVS.cso", "Shader/cso/TranslucentPS.cso", hdr, _countof(hdr),
			0, 0.0f, EBlendStatePreset::NoColorWrite, ECullModePreset::None, EDepthStatePreset::DepthWrite);
	m_PipelineState["TranslucencyEqual"] =
		CreatePipeline("Shader/cso/GeometryVS.cso", "Shader/cso/TranslucentPS.cso", hdr, _countof(hdr),
			0, 0.0f, EBlendStatePreset::Translucent, ECullModePreset::Back, EDepthStatePreset::DepthReadEqual);
	m_PipelineState["TranslucencyEqualTwoSided"] =
		CreatePipeline("Shader/cso/GeometryVS.cso", "Shader/cso/TranslucentPS.cso", hdr, _countof(hdr),
			0, 0.0f, EBlendStatePreset::Translucent, ECullModePreset::None, EDepthStatePreset::DepthReadEqual);

	m_PipelineState["TranslucencyAdditiveTwoSided"] =
		CreatePipeline("Shader/cso/GeometryVS.cso", "Shader/cso/TranslucentPS.cso", hdr, _countof(hdr),
			0, 0.0f, EBlendStatePreset::Additive, ECullModePreset::None, EDepthStatePreset::DepthRead);

	// ---- シャドウ深度バリアント ----
	// TwoSided はカリング無効 (両面が影を落とす)。Masked は
	// ShadowDepthMaskedPS が OpacityMask を clip する。
	// Translucent / Additive はシャドウマップに描かない (UE5 既定)。
	m_PipelineState["ShadowDepthTwoSided"] =
		CreatePipeline("Shader/cso/ShadowDepthVS.cso", "Shader/cso/ShadowDepthPS.cso", nullptr, 0, 1000, 1.5f,
			EBlendStatePreset::Opaque, ECullModePreset::None);
	m_PipelineState["ShadowDepthMasked"] =
		CreatePipeline("Shader/cso/ShadowDepthVS.cso", "Shader/cso/ShadowDepthMaskedPS.cso", nullptr, 0, 1000, 1.5f);
	m_PipelineState["ShadowDepthMaskedTwoSided"] =
		CreatePipeline("Shader/cso/ShadowDepthVS.cso", "Shader/cso/ShadowDepthMaskedPS.cso", nullptr, 0, 1000, 1.5f,
			EBlendStatePreset::Opaque, ECullModePreset::None);

	// ---- 全 PSO の生成結果を検証 ----
	// 生成に失敗した PSO が 1 つでもあれば名前を列挙する。null PSO は
	// 描画時に D3D12Core 内のアクセス違反として現れるため、
	// 起動直後の出力ウィンドウで原因パスを特定できるようにする。
	for (const auto& pair : m_PipelineState)
	{
		if (pair.second.Get() == nullptr)
		{
			char msg[256];
			sprintf_s(msg, "[RenderManager] pipeline creation FAILED: %s\n", pair.first.c_str());
			OutputDebugStringA(msg);
		}
	}
}



// ============================================================
//  Frame synchronization
// ============================================================
void RenderManager::WaitGPU()
{
	m_CommandQueue->Signal(m_Fence.Get(), m_Frame[m_RTIndex]);

	m_Fence->SetEventOnCompletion(m_Frame[m_RTIndex], m_FenceEvent);
	WaitForSingleObjectEx(m_FenceEvent, INFINITE, FALSE);

	m_Frame[m_RTIndex]++;
}


// ============================================================
//  Frame begin (RHI): ヒープ / ルートシグネチャ / 定数リング / ビューポート
//  パス列 (G-Buffer -> デファード -> ポスプロ) は FSceneRenderer が駆動する。
// ============================================================
void RenderManager::BeginFrame()
{
	// シェーダ可視デスクリプタヒープ + ルートシグネチャ
	ID3D12DescriptorHeap* dh[] = { m_SRVDescriptorHeap.Get() };
	m_GraphicsCommandList->SetDescriptorHeaps(_countof(dh), dh);
	m_GraphicsCommandList->SetGraphicsRootSignature(m_RootSignature.Get());

	// 定数バッファのリングインデックス初期化
	m_ConstantBufferIndex[m_RTIndex] = 0;

	// ビューポート / シザー
	m_GraphicsCommandList->RSSetViewports(1, &m_Viewport);
	m_GraphicsCommandList->RSSetScissorRects(1, &m_ScissorRect);
}


// ============================================================
//  Frame end (RHI): Close -> Execute -> Present -> 前フレーム待ち -> Reset
// ============================================================
void RenderManager::Present()
{
	HRESULT hr;

	//======================================================
	// コマンド発行
	//======================================================
	{
		hr = m_GraphicsCommandList->Close();
		assert(SUCCEEDED(hr));

		ID3D12CommandList* const command_lists[1] = { m_GraphicsCommandList.Get() };
		m_CommandQueue->ExecuteCommandLists(1, command_lists);
		m_CommandQueue->Signal(m_Fence.Get(), m_Frame[m_RTIndex]);
	}

	hr = m_SwapChain->Present(1, 0);
	assert(SUCCEEDED(hr));


	//======================================================
	// 前フレーム待ち
	//======================================================
	{
		UINT64 frame = m_Frame[m_RTIndex];

		m_RTIndex = m_SwapChain->GetCurrentBackBufferIndex();

		if (m_Fence->GetCompletedValue() < m_Frame[m_RTIndex])
		{
			m_Fence->SetEventOnCompletion(m_Frame[m_RTIndex], m_FenceEvent);
			WaitForSingleObjectEx(m_FenceEvent, INFINITE, FALSE);
		}

		m_Frame[m_RTIndex] = frame + 1;
	}

	hr = m_GraphicsCommandAllocator[m_RTIndex]->Reset();
	assert(SUCCEEDED(hr));

	hr = m_GraphicsCommandList->Reset(m_GraphicsCommandAllocator[m_RTIndex].Get(), nullptr);
	assert(SUCCEEDED(hr));
}



// ============================================================
//  Texture creation
// ============================================================
std::unique_ptr<TEXTURE> RenderManager::LoadTexture(const char* FileName, bool sRGB)
{
	std::unique_ptr<TEXTURE> texture = std::make_unique<TEXTURE>();

	std::unique_ptr<uint8_t[]>          ddsData;
	std::vector<D3D12_SUBRESOURCE_DATA> subresouceData;

	wchar_t wFileName[MAX_PATH];
	size_t  size;
	mbstowcs_s(&size, wFileName, FileName, MAX_PATH);

	// BaseColor/Albedo は sRGB 入力なので sRGB としてサンプリングし、
	// ハードウェアにリニア展開させる (ライティングは常にリニア空間)。
	// データテクスチャ (normal / ORM / roughness / HDR env) はリニア (sRGB=false)。
	unsigned int loadFlags = sRGB ? DDS_LOADER_FORCE_SRGB : DDS_LOADER_DEFAULT;

	HRESULT hr = LoadDDSTextureFromFileEx(
		m_Device.Get(), wFileName, 0,
		D3D12_RESOURCE_FLAG_NONE, loadFlags,
		&texture->Resource, ddsData, subresouceData);
	assert(SUCCEEDED(hr));

	texture->Resource->SetName(wFileName);

	D3D12_RESOURCE_DESC desc = texture->Resource->GetDesc();

	// フォーマットごとの bpp / ブロックサイズ (WriteToSubresource の幅・高さ算出用)
	unsigned int bpp, block;
	switch (desc.Format)
	{
		// BC1: 4bpp ブロック圧縮 (UNORM / sRGB 同レイアウト)
	case DXGI_FORMAT_BC1_UNORM:
	case DXGI_FORMAT_BC1_UNORM_SRGB:
		bpp = 4;  block = 4;  break;

		// BC2/BC3/BC7: 8bpp ブロック圧縮 (UNORM / sRGB 同レイアウト)
	case DXGI_FORMAT_BC2_UNORM:
	case DXGI_FORMAT_BC2_UNORM_SRGB:
	case DXGI_FORMAT_BC3_UNORM:
	case DXGI_FORMAT_BC3_UNORM_SRGB:
	case DXGI_FORMAT_BC7_UNORM:
	case DXGI_FORMAT_BC7_UNORM_SRGB:
		bpp = 8;  block = 4;  break;

		// BC6H: 8bpp ブロック圧縮 HDR (sRGB バリアントなし)
	case DXGI_FORMAT_BC6H_UF16:
	case DXGI_FORMAT_BC6H_SF16:
		bpp = 8;  block = 4;  break;

		// 非圧縮 32bit (R8G8B8A8 / B8G8R8A8, UNORM or sRGB)
	default:
		bpp = 32; block = 1;  break;
	}

	for (unsigned int a = 0; a < desc.DepthOrArraySize; a++)
	{
		for (unsigned int m = 0; m < desc.MipLevels; m++)
		{
			unsigned int s = a * desc.MipLevels + m;

			unsigned int width = (unsigned int)subresouceData[s].RowPitch * 8 / bpp / block;
			unsigned int height = (unsigned int)subresouceData[s].SlicePitch / (unsigned int)subresouceData[s].RowPitch * block;

			D3D12_BOX box = { 0, 0, 0, width, height, 1 };

			hr = texture->Resource->WriteToSubresource(s, &box, subresouceData[s].pData,
				(UINT)subresouceData[s].RowPitch, (UINT)subresouceData[s].SlicePitch);
			assert(SUCCEEDED(hr));
		}
	}

	m_GraphicsCommandList->ResourceBarrier(1,
		&CD3DX12_RESOURCE_BARRIER::Transition(
			texture->Resource.Get(),
			D3D12_RESOURCE_STATE_COPY_DEST,
			D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE));

	texture->SRVIndex = CreateShaderResourceView(texture->Resource.Get());
	return texture;
}


// ============================================================
//  Render target creation
// ============================================================
std::unique_ptr<RENDER_TARGET> RenderManager::CreateRenderTarget(unsigned int Width, unsigned int Height, DXGI_FORMAT Format, unsigned int MipLevels)
{
	D3D12_HEAP_PROPERTIES properties{};
	properties.Type = D3D12_HEAP_TYPE_DEFAULT;
	properties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
	properties.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
	properties.CreationNodeMask = 0;
	properties.VisibleNodeMask = 0;

	D3D12_RESOURCE_DESC desc{};
	desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	desc.Width = Width;
	desc.Height = Height;
	desc.DepthOrArraySize = 1;
	desc.MipLevels = MipLevels;
	desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
	desc.SampleDesc.Count = 1;
	desc.SampleDesc.Quality = 0;
	desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
	desc.Format = Format;

	D3D12_CLEAR_VALUE clearValue{};
	clearValue.Color[0] = 0.0f;
	clearValue.Color[1] = 0.0f;
	clearValue.Color[2] = 0.0f;
	clearValue.Color[3] = 1.0f;
	clearValue.Format = Format;

	auto renderTarget = std::make_unique<RENDER_TARGET>();

	HRESULT hr = m_Device->CreateCommittedResource(&properties,
		D3D12_HEAP_FLAG_NONE,
		&desc,
		D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
		&clearValue,
		IID_PPV_ARGS(&renderTarget->Resource));
	assert(SUCCEEDED(hr));

	renderTarget->SRVIndex = CreateShaderResourceView(renderTarget->Resource.Get());
	renderTarget->SRVHandle = GetShaderResourceViewHandle(renderTarget->SRVIndex);
	renderTarget->RTVIndex = CreateRenderTargetView(renderTarget->Resource.Get());
	renderTarget->RTVHandle = GetRenderTargetViewHandle(renderTarget->RTVIndex);

	return renderTarget;
}


// ============================================================
//  Binding helpers
// ============================================================
void RenderManager::BindRootTableBySRVIndex(unsigned int RootParameter, unsigned int SRVIndex)
{
	D3D12_GPU_DESCRIPTOR_HANDLE handle = OffsetGPUHandle(
		m_SRVDescriptorHeap->GetGPUDescriptorHandleForHeapStart(),
		SRVIndex, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

	m_GraphicsCommandList->SetGraphicsRootDescriptorTable(RootParameter, handle);
}


void RenderManager::SetConstant(CONSTANT_TYPE Type, const void* Constant, unsigned int Size)
{
	const unsigned int slot = m_ConstantBufferIndex[m_RTIndex];
	assert(slot < CONSTANT_BUFFER_MAX);

	memcpy(m_ConstantBufferPointer[m_RTIndex] + CONSTANT_BUFFER_SIZE * slot, Constant, Size);

	BindRootTableBySRVIndex((unsigned int)Type, m_ConstantBufferView[m_RTIndex][slot]);

	m_ConstantBufferIndex[m_RTIndex]++;
}


void RenderManager::SetTexture(TEXTURE_TYPE Type, const TEXTURE* Texture)
{
	BindRootTableBySRVIndex((unsigned int)Type, Texture->SRVIndex);
}

void RenderManager::SetTexture(TEXTURE_TYPE Type, const RENDER_TARGET* Texture)
{
	BindRootTableBySRVIndex((unsigned int)Type, Texture->SRVIndex);
}


// ============================================================
//  Vertex / index buffers
// ============================================================
std::unique_ptr<VERTEX_BUFFER> RenderManager::CreateVertexBuffer(unsigned int Stride, unsigned int Size)
{
	auto vertexBuffer = std::make_unique<VERTEX_BUFFER>();

	HRESULT hr = m_Device->CreateCommittedResource(
		&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD),
		D3D12_HEAP_FLAG_NONE,
		&CD3DX12_RESOURCE_DESC::Buffer(Stride * Size),
		D3D12_RESOURCE_STATE_GENERIC_READ,
		nullptr,
		IID_PPV_ARGS(&vertexBuffer->Resource));
	assert(SUCCEEDED(hr));

	vertexBuffer->Stride = Stride;
	vertexBuffer->Size = Size;

	return vertexBuffer;
}

void RenderManager::SetVertexBuffer(const VERTEX_BUFFER* VertexBuffer)
{
	D3D12_VERTEX_BUFFER_VIEW vertexView{};
	vertexView.BufferLocation = VertexBuffer->Resource->GetGPUVirtualAddress();
	vertexView.StrideInBytes = VertexBuffer->Stride;
	vertexView.SizeInBytes = VertexBuffer->Stride * VertexBuffer->Size;

	m_GraphicsCommandList->IASetVertexBuffers(0, 1, &vertexView);
}


std::unique_ptr<INDEX_BUFFER> RenderManager::CreateIndexBuffer(unsigned int Size)
{
	auto indexBuffer = std::make_unique<INDEX_BUFFER>();

	HRESULT hr = m_Device->CreateCommittedResource(
		&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD),
		D3D12_HEAP_FLAG_NONE,
		&CD3DX12_RESOURCE_DESC::Buffer(sizeof(unsigned int) * Size),
		D3D12_RESOURCE_STATE_GENERIC_READ,
		nullptr,
		IID_PPV_ARGS(&indexBuffer->Resource));
	assert(SUCCEEDED(hr));

	indexBuffer->Size = Size;

	return indexBuffer;
}

void RenderManager::SetIndexBuffer(const INDEX_BUFFER* IndexBuffer)
{
	D3D12_INDEX_BUFFER_VIEW indexView{};
	indexView.BufferLocation = IndexBuffer->Resource->GetGPUVirtualAddress();
	indexView.SizeInBytes = sizeof(unsigned int) * IndexBuffer->Size;
	indexView.Format = DXGI_FORMAT_R32_UINT;

	m_GraphicsCommandList->IASetIndexBuffer(&indexView);
}


void RenderManager::SetPipelineState(const char* PipelineName)
{
	auto it = m_PipelineState.find(PipelineName);
	ID3D12PipelineState* pipeline = (it != m_PipelineState.end()) ? it->second.Get() : nullptr;

	if (pipeline == nullptr)
	{
		// null PSO を SetPipelineState に渡すと D3D12Core 内の
		// アクセス違反になる。名前を出力して呼び出しをスキップする
		// (Release でも出力ウィンドウで原因を特定できる)。
		char msg[256];
		sprintf_s(msg, "[RenderManager] SetPipelineState: pipeline is null or missing: %s\n", PipelineName);
		OutputDebugStringA(msg);
		assert(false && "SetPipelineState: null pipeline");
		return;
	}

	m_GraphicsCommandList->SetPipelineState(pipeline);
}


// ============================================================
//  Pipeline state creation
// ============================================================
// ============================================================
//  整数 (UINT / SINT) フォーマット判定
//  整数 RTV はハードウェアブレンド不可のため、ブレンドステート
//  構築時に BlendEnable を強制的に FALSE にする必要がある。
//  (BlendEnable = TRUE のままだと CreateGraphicsPipelineState が
//   E_INVALIDARG で失敗し、null PSO -> 描画時に D3D12Core 内の
//   アクセス違反 (0xC0000005) として現れる)
// ============================================================
static bool IsIntegerFormat(DXGI_FORMAT Format)
{
	switch (Format)
	{
	case DXGI_FORMAT_R32G32B32A32_UINT:
	case DXGI_FORMAT_R32G32B32A32_SINT:
	case DXGI_FORMAT_R32G32B32_UINT:
	case DXGI_FORMAT_R32G32B32_SINT:
	case DXGI_FORMAT_R16G16B16A16_UINT:
	case DXGI_FORMAT_R16G16B16A16_SINT:
	case DXGI_FORMAT_R32G32_UINT:
	case DXGI_FORMAT_R32G32_SINT:
	case DXGI_FORMAT_R10G10B10A2_UINT:
	case DXGI_FORMAT_R8G8B8A8_UINT:
	case DXGI_FORMAT_R8G8B8A8_SINT:
	case DXGI_FORMAT_R16G16_UINT:
	case DXGI_FORMAT_R16G16_SINT:
	case DXGI_FORMAT_R32_UINT:
	case DXGI_FORMAT_R32_SINT:
	case DXGI_FORMAT_R8G8_UINT:
	case DXGI_FORMAT_R8G8_SINT:
	case DXGI_FORMAT_R16_UINT:
	case DXGI_FORMAT_R16_SINT:
	case DXGI_FORMAT_R8_UINT:
	case DXGI_FORMAT_R8_SINT:
		return true;
	default:
		return false;
	}
}

ComPtr<ID3D12PipelineState> RenderManager::CreatePipeline(const char* VertexShaderFile, const char* PixelShaderFile, const DXGI_FORMAT* RTVFormats, unsigned int NumRenderTargets, int DepthBias, float SlopeScaledDepthBias, EBlendStatePreset BlendPreset, ECullModePreset CullPreset, EDepthStatePreset DepthPreset)
{
	D3D12_GRAPHICS_PIPELINE_STATE_DESC pipelineStateDesc{};

	// シェーダバイトコード読み込み (.cso をそのまま読み込む)
	auto loadShader = [](const char* path, std::vector<char>& out, D3D12_SHADER_BYTECODE& bytecode)
		{
			// 欠落 / 空の .cso (シェーダのコンパイル失敗や未再コンパイル)
			// を null バイトコードのまま PSO 生成に渡すと、
			// CreateGraphicsPipelineState 内のアクセス違反になるため
			// ここで即検知する。
			bytecode.pShaderBytecode = nullptr;
			bytecode.BytecodeLength = 0;

			std::ifstream file(path, std::ios_base::in | std::ios_base::binary);
			if (!file)
			{
				char msg[512];
				sprintf_s(msg, "[RenderManager] shader .cso not found: %s\n", path);
				OutputDebugStringA(msg);
				assert(false && "shader .cso not found");
				return;
			}

			file.seekg(0, std::ios_base::end);
			int filesize = (int)file.tellg();
			file.seekg(0, std::ios_base::beg);

			if (filesize <= 0)
			{
				char msg[512];
				sprintf_s(msg, "[RenderManager] shader .cso is empty (compile failed?): %s\n", path);
				OutputDebugStringA(msg);
				assert(false && "shader .cso is empty");
				return;
			}

			out.resize(filesize);
			file.read(out.data(), filesize);
			file.close();

			bytecode.pShaderBytecode = out.data();
			bytecode.BytecodeLength = filesize;
		};

	std::vector<char> vertexShader;
	std::vector<char> pixelShader;
	loadShader(VertexShaderFile, vertexShader, pipelineStateDesc.VS);
	loadShader(PixelShaderFile, pixelShader, pipelineStateDesc.PS);

	// インプットレイアウト
	D3D12_INPUT_ELEMENT_DESC InputElementDesc[] =
	{
		{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT,    0,  0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT,    0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "TANGENT",  0, DXGI_FORMAT_R32G32B32_FLOAT,    0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,       0, 36, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 44, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
	};
	pipelineStateDesc.InputLayout.pInputElementDescs = InputElementDesc;
	pipelineStateDesc.InputLayout.NumElements = _countof(InputElementDesc);

	pipelineStateDesc.SampleDesc.Count = 1;
	pipelineStateDesc.SampleDesc.Quality = 0;
	pipelineStateDesc.SampleMask = UINT_MAX;

	pipelineStateDesc.NumRenderTargets = NumRenderTargets;
	for (unsigned int i = 0; i < NumRenderTargets; i++)
		pipelineStateDesc.RTVFormats[i] = RTVFormats[i];

	pipelineStateDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	pipelineStateDesc.pRootSignature = m_RootSignature.Get();

	// ラスタライザ
	// bTwoSided (ECullModePreset::None) はカリング無効
	pipelineStateDesc.RasterizerState.CullMode =
		(CullPreset == ECullModePreset::None) ? D3D12_CULL_MODE_NONE : D3D12_CULL_MODE_BACK;
	pipelineStateDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
	pipelineStateDesc.RasterizerState.FrontCounterClockwise = FALSE;
	pipelineStateDesc.RasterizerState.DepthBias = DepthBias;
	pipelineStateDesc.RasterizerState.DepthBiasClamp = 0.0f;
	pipelineStateDesc.RasterizerState.SlopeScaledDepthBias = SlopeScaledDepthBias;
	pipelineStateDesc.RasterizerState.DepthClipEnable = FALSE;
	pipelineStateDesc.RasterizerState.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;
	pipelineStateDesc.RasterizerState.AntialiasedLineEnable = FALSE;
	pipelineStateDesc.RasterizerState.MultisampleEnable = FALSE;

	// ブレンド (EBlendMode -> TStaticBlendState 相当のプリセット)
	//   Opaque / Masked : One / Zero 上書き (従来通り)
	//   Translucent     : SrcAlpha / InvSrcAlpha (BLEND_Translucent)
	//   Additive        : SrcAlpha / One (BLEND_Additive)
	// α チャンネルは合成後のシーンカバレッジ規約 (Zero / InvSrcAlpha 系)
	// ※ Substrate バッファ (BasePass RT3/RT4 = R32G32B32A32_UINT) の
	//   ような整数 RT はブレンド不可なので、その RT だけ BlendEnable を
	//   FALSE にする (IndependentBlendEnable が必要)。未使用スロットも
	//   FALSE に落とす。ブレンド係数は無効 RT では無視される。
	for (int i = 0; i < _countof(pipelineStateDesc.BlendState.RenderTarget); ++i)
	{
		auto& rt = pipelineStateDesc.BlendState.RenderTarget[i];

		const bool bUsedTarget = ((unsigned int)i < NumRenderTargets) && (RTVFormats != nullptr);
		const bool bIntegerTarget = bUsedTarget && IsIntegerFormat(RTVFormats[i]);
		rt.BlendEnable = (bUsedTarget && !bIntegerTarget) ? TRUE : FALSE;

		switch (BlendPreset)
		{
		case EBlendStatePreset::Translucent:
			rt.SrcBlend = D3D12_BLEND_SRC_ALPHA;
			rt.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
			rt.SrcBlendAlpha = D3D12_BLEND_ZERO;
			rt.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
			break;

		case EBlendStatePreset::Additive:
			rt.SrcBlend = D3D12_BLEND_SRC_ALPHA;
			rt.DestBlend = D3D12_BLEND_ONE;
			rt.SrcBlendAlpha = D3D12_BLEND_ZERO;
			rt.DestBlendAlpha = D3D12_BLEND_ONE;
			break;

		case EBlendStatePreset::NoColorWrite:
			// 半透明深度プリパス: カラーは一切書かない (深度のみ)
			rt.BlendEnable = FALSE;
			rt.SrcBlend = D3D12_BLEND_ONE;
			rt.DestBlend = D3D12_BLEND_ZERO;
			rt.SrcBlendAlpha = D3D12_BLEND_ONE;
			rt.DestBlendAlpha = D3D12_BLEND_ZERO;
			break;

		case EBlendStatePreset::Opaque:
		default:
			rt.SrcBlend = D3D12_BLEND_ONE;
			rt.DestBlend = D3D12_BLEND_ZERO;
			rt.SrcBlendAlpha = D3D12_BLEND_ONE;
			rt.DestBlendAlpha = D3D12_BLEND_ZERO;
			break;
		}

		rt.BlendOp = D3D12_BLEND_OP_ADD;
		rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
		rt.RenderTargetWriteMask = (BlendPreset == EBlendStatePreset::NoColorWrite)
			? 0u
			: D3D12_COLOR_WRITE_ENABLE_ALL;
		rt.LogicOpEnable = FALSE;
		rt.LogicOp = D3D12_LOGIC_OP_CLEAR;
	}
	pipelineStateDesc.BlendState.AlphaToCoverageEnable = FALSE;
	// RT ごとに BlendEnable が異なる (浮動小数 RT = ON / 整数 RT = OFF)
	// ため独立ブレンドを有効化する
	pipelineStateDesc.BlendState.IndependentBlendEnable = TRUE;

	// デプス・ステンシル
	pipelineStateDesc.DepthStencilState.DepthEnable = TRUE;
	// DepthReadEqual (半透明深度プリパスの着色パス) はプリパスが書いた
	// 最前面深度と一致するフラグメントのみ通す
	pipelineStateDesc.DepthStencilState.DepthFunc =
		(DepthPreset == EDepthStatePreset::DepthReadEqual)
			? D3D12_COMPARISON_FUNC_EQUAL
			: D3D12_COMPARISON_FUNC_LESS_EQUAL;
	// トランスルーセンシー (DepthRead / DepthReadEqual) は深度テストのみ
	// (書き込み無効)
	pipelineStateDesc.DepthStencilState.DepthWriteMask =
		(DepthPreset == EDepthStatePreset::DepthWrite)
			? D3D12_DEPTH_WRITE_MASK_ALL
			: D3D12_DEPTH_WRITE_MASK_ZERO;
	pipelineStateDesc.DepthStencilState.StencilEnable = FALSE;
	pipelineStateDesc.DepthStencilState.StencilReadMask = D3D12_DEFAULT_STENCIL_READ_MASK;
	pipelineStateDesc.DepthStencilState.StencilWriteMask = D3D12_DEFAULT_STENCIL_WRITE_MASK;

	pipelineStateDesc.DepthStencilState.FrontFace.StencilFailOp = D3D12_STENCIL_OP_KEEP;
	pipelineStateDesc.DepthStencilState.FrontFace.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
	pipelineStateDesc.DepthStencilState.FrontFace.StencilPassOp = D3D12_STENCIL_OP_KEEP;
	pipelineStateDesc.DepthStencilState.FrontFace.StencilFunc = D3D12_COMPARISON_FUNC_ALWAYS;

	pipelineStateDesc.DepthStencilState.BackFace.StencilFailOp = D3D12_STENCIL_OP_KEEP;
	pipelineStateDesc.DepthStencilState.BackFace.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
	pipelineStateDesc.DepthStencilState.BackFace.StencilPassOp = D3D12_STENCIL_OP_KEEP;
	pipelineStateDesc.DepthStencilState.BackFace.StencilFunc = D3D12_COMPARISON_FUNC_ALWAYS;

	pipelineStateDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT;

	ComPtr<ID3D12PipelineState> pipelineState;
	HRESULT hr = m_Device->CreateGraphicsPipelineState(&pipelineStateDesc, IID_PPV_ARGS(&pipelineState));
	if (FAILED(hr))
	{
		// PSO 生成失敗を放置すると null PSO のまま描画に進み、
		// D3D12Core 内のアクセス違反という分かりにくい形で落ちる。
		// ここでシェーダ名と HRESULT を出力して即座に特定できるようにする。
		char msg[512];
		sprintf_s(msg, "[RenderManager] CreateGraphicsPipelineState failed (hr=0x%08X): VS=%s PS=%s\n",
			(unsigned int)hr, VertexShaderFile, PixelShaderFile);
		OutputDebugStringA(msg);
		assert(false && "CreateGraphicsPipelineState failed");
	}

	return pipelineState;
}


// ============================================================
//  Descriptor pool helpers
// ============================================================
unsigned int RenderManager::AllocateSRVSlot()
{
	unsigned int index = m_SRVDescriptorPool.front();
	m_SRVDescriptorPool.pop_front();
	return index;
}

unsigned int RenderManager::AllocateRTVSlot()
{
	unsigned int index = m_RTVDescriptorPool.front();
	m_RTVDescriptorPool.pop_front();
	return index;
}

D3D12_CPU_DESCRIPTOR_HANDLE RenderManager::OffsetCPUHandle(D3D12_CPU_DESCRIPTOR_HANDLE base, unsigned int index, D3D12_DESCRIPTOR_HEAP_TYPE type) const
{
	base.ptr += (SIZE_T)m_Device->GetDescriptorHandleIncrementSize(type) * index;
	return base;
}

D3D12_GPU_DESCRIPTOR_HANDLE RenderManager::OffsetGPUHandle(D3D12_GPU_DESCRIPTOR_HANDLE base, unsigned int index, D3D12_DESCRIPTOR_HEAP_TYPE type) const
{
	base.ptr += (UINT64)m_Device->GetDescriptorHandleIncrementSize(type) * index;
	return base;
}


unsigned int RenderManager::CreateShaderResourceView(ID3D12Resource* Resource)
{
	unsigned int index = AllocateSRVSlot();

	D3D12_CPU_DESCRIPTOR_HANDLE handle = OffsetCPUHandle(
		m_SRVDescriptorHeap->GetCPUDescriptorHandleForHeapStart(),
		index, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

	D3D12_RESOURCE_DESC resDesc = Resource->GetDesc();

	D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
	srvDesc.Format = resDesc.Format;
	srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	srvDesc.Texture2D.MipLevels = resDesc.MipLevels;

	m_Device->CreateShaderResourceView(Resource, &srvDesc, handle);

	return index;
}

D3D12_GPU_DESCRIPTOR_HANDLE RenderManager::GetShaderResourceViewHandle(unsigned int SRVIndex)
{
	return OffsetGPUHandle(
		m_SRVDescriptorHeap->GetGPUDescriptorHandleForHeapStart(),
		SRVIndex, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
}

void RenderManager::ReleaseShaderResourceView(unsigned int SRVIndex)
{
	m_SRVDescriptorPool.push_front(SRVIndex);
}


unsigned int RenderManager::CreateRenderTargetView(ID3D12Resource* Resource, unsigned int MipLevel)
{
	unsigned int index = AllocateRTVSlot();

	D3D12_CPU_DESCRIPTOR_HANDLE handle = OffsetCPUHandle(
		m_RTVDescriptorHeap->GetCPUDescriptorHandleForHeapStart(),
		index, D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

	m_Device->CreateRenderTargetView(Resource, nullptr, handle);

	return index;
}

D3D12_CPU_DESCRIPTOR_HANDLE RenderManager::GetRenderTargetViewHandle(unsigned int RTVIndex)
{
	return OffsetCPUHandle(
		m_RTVDescriptorHeap->GetCPUDescriptorHandleForHeapStart(),
		RTVIndex, D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
}

void RenderManager::ReleaseRenderTargetView(unsigned int RTVIndex)
{
	m_RTVDescriptorPool.push_front(RTVIndex);
}


// ============================================================
//  Resource destructors (return descriptor slots to the pool)
// ============================================================
TEXTURE::~TEXTURE()
{
	RenderManager::GetInstance()->ReleaseShaderResourceView(SRVIndex);
}

RENDER_TARGET::~RENDER_TARGET()
{
	RenderManager::GetInstance()->ReleaseShaderResourceView(SRVIndex);
	RenderManager::GetInstance()->ReleaseRenderTargetView(RTVIndex);
}


// ============================================================
//  Accessors used by IBLBaker
// ============================================================
unsigned int RenderManager::AllocateDescriptor()
{
	return AllocateSRVSlot();
}

D3D12_CPU_DESCRIPTOR_HANDLE RenderManager::GetCPUDescriptorHandle(unsigned int Index)
{
	return OffsetCPUHandle(
		m_SRVDescriptorHeap->GetCPUDescriptorHandleForHeapStart(),
		Index, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
}

D3D12_GPU_DESCRIPTOR_HANDLE RenderManager::GetGPUDescriptorHandle(unsigned int Index)
{
	return OffsetGPUHandle(
		m_SRVDescriptorHeap->GetGPUDescriptorHandleForHeapStart(),
		Index, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
}

// コマンドリストを即時実行して完了待ち、次フレーム用に Reset
void RenderManager::FlushAndResetCommandList()
{
	HRESULT hr = m_GraphicsCommandList->Close();
	assert(SUCCEEDED(hr));

	ID3D12CommandList* lists[] = { m_GraphicsCommandList.Get() };
	m_CommandQueue->ExecuteCommandLists(1, lists);

	WaitGPU();

	hr = m_GraphicsCommandAllocator[m_RTIndex]->Reset();
	assert(SUCCEEDED(hr));
	hr = m_GraphicsCommandList->Reset(m_GraphicsCommandAllocator[m_RTIndex].Get(), nullptr);
	assert(SUCCEEDED(hr));
}
