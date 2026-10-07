#include "Main.h"
#include "RenderManager.h"

#include "D3DX12.h"
#include "PostProcessUpscale.h"
#include "DDSTextureLoader12.h"

#include "ImGUI/imgui.h"
#include "ImGUI/imgui_impl_win32.h"
#include "ImGUI/imgui_impl_dx12.h"


// ============================================================
//  デバッグ支援トグル
//  必要時に 1 へ変更してリビルドする (既定はすべて無効)。
// ============================================================
#define ENABLE_GPU_BASED_VALIDATION	0	// デバッグレイヤーの GPU ベース検証
#define ENABLE_DRED					0	// デバイス削除拡張データ (DRED)
#define ENABLE_REPORT_LIVE_OBJECTS	0	// 終了時の生存オブジェクト一覧

// ============================================================
//  デバッグ支援の環境変数スイッチ (Debug ビルドのみ。リビルド不要)
//    DX12_DEBUG_GBV=1      : GPU ベース検証を有効化 (ENABLE_GPU_BASED_VALIDATION と OR)
//    DX12_DEBUG_NO_BREAK=1 : ERROR / CORRUPTION でのブレークを無効化
//                            (デバッガ無しの計測実行でメッセージを最後まで収集する)
// ============================================================
#if defined(_DEBUG)
static bool IsDebugEnvironmentSwitchEnabled(const char* Name)
{
	char value[16] = {};
	const DWORD length = GetEnvironmentVariableA(Name, value, (DWORD)sizeof(value));
	return length > 0 && length < sizeof(value) && value[0] == '1';
}
#endif


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
#if defined(_DEBUG) && ENABLE_REPORT_LIVE_OBJECTS
	// 解放漏れ D3D12 オブジェクトを列挙する (リーク解析用)
	{
		ComPtr<ID3D12DebugDevice> debugInterface;
		if (SUCCEEDED(m_Device->QueryInterface(IID_PPV_ARGS(&debugInterface))))
		{
			debugInterface->ReportLiveDeviceObjects(D3D12_RLDO_DETAIL | D3D12_RLDO_IGNORE_INTERNAL);
		}
	}
#endif
}


// ============================================================
//  Initialization (FD3D12DynamicRHI::Init 相当)
//  コンストラクタから 1 度だけ呼ばれる。呼び出し順 = 依存関係順。
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
	// 既定ビューポートはバックバッファ全体から開始する
	SetDefaultViewportSize((unsigned int)m_BackBufferWidth, (unsigned int)m_BackBufferHeight);
}


// 既定ビューポート / シザーを書き換える (記録済みコマンドには影響しない。
// 次の SetDefaultGraphicsState (BeginFrame / FlushAndResetCommandList) か
// RestoreDefaultViewport から適用される)
void RenderManager::SetDefaultViewportSize(unsigned int Width, unsigned int Height)
{
	m_DefaultViewportWidth = Width;
	m_DefaultViewportHeight = Height;

	m_Viewport.TopLeftX = 0.0f;
	m_Viewport.TopLeftY = 0.0f;
	m_Viewport.Width = (FLOAT)Width;
	m_Viewport.Height = (FLOAT)Height;
	m_Viewport.MinDepth = 0.0f;
	m_Viewport.MaxDepth = 1.0f;

	m_ScissorRect.top = 0;
	m_ScissorRect.left = 0;
	m_ScissorRect.right = (LONG)Width;
	m_ScissorRect.bottom = (LONG)Height;
}


// 既定ビューポート / シザーを現在のコマンドリストへ即時記録する
void RenderManager::RestoreDefaultViewport()
{
	m_GraphicsCommandList->RSSetViewports(1, &m_Viewport);
	m_GraphicsCommandList->RSSetScissorRects(1, &m_ScissorRect);
}


void RenderManager::InitDevice()
{
	HRESULT hr;

#if defined(_DEBUG)
	// ---- デバッグレイヤー有効化 ----
	{
		ComPtr<ID3D12Debug1> debugController;
		if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController))))
		{
			debugController->EnableDebugLayer();

			// GPU ベース検証 (リソース状態 / デスクリプタを GPU 実行時に検証)。
			// デバイス生成前に設定する必要がある
			if (ENABLE_GPU_BASED_VALIDATION || IsDebugEnvironmentSwitchEnabled("DX12_DEBUG_GBV"))
			{
				debugController->SetEnableGPUBasedValidation(TRUE);
				OutputDebugStringA("[RenderManager] GPU-based validation: enabled\n");
			}
		}
	}

#if ENABLE_DRED
	// ---- DRED (デバイス削除拡張データ) ----
	{
		ComPtr<ID3D12DeviceRemovedExtendedDataSettings1> d3dDredSettings1;
		if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&d3dDredSettings1))))
		{
			d3dDredSettings1->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
			d3dDredSettings1->SetBreadcrumbContextEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
			d3dDredSettings1->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
		}
	}
#endif
#endif

	UINT flag{};
	hr = CreateDXGIFactory2(flag, IID_PPV_ARGS(&m_Factory));
	assert(SUCCEEDED(hr));

	hr = m_Factory->EnumAdapters(0, (IDXGIAdapter**)m_Adapter.GetAddressOf());
	assert(SUCCEEDED(hr));

	// ---- フィーチャーレベルは 12_1 -> 12_0 -> 11_1 の順で試行 ----
	// (DXR / SM6 系の機能判定のため可能なら 12 系で作る。11_1 は
	//  従来どおりの最終フォールバック)
	{
		const D3D_FEATURE_LEVEL featureLevels[] = {
			D3D_FEATURE_LEVEL_12_1,
			D3D_FEATURE_LEVEL_12_0,
			D3D_FEATURE_LEVEL_11_1,
		};

		hr = E_FAIL;
		for (D3D_FEATURE_LEVEL level : featureLevels)
		{
			hr = D3D12CreateDevice(m_Adapter.Get(), level, IID_PPV_ARGS(&m_Device));
			if (SUCCEEDED(hr))
			{
				break;
			}
		}
		assert(SUCCEEDED(hr));
	}

	// ---- DXR (インラインレイトレーシング) サポート判定 ----
	// ID3D12Device5 + RaytracingTier 1.1 (RayQuery) が揃えば
	// Lumen の HWRT トレースパスが利用可能 (LumenHardwareRayTracing.h)。
	// 非対応環境では SWRT (メッシュ SDF + Global Distance Field) のみ。
	{
		m_bRayTracingSupported = false;

		if (SUCCEEDED(m_Device.As(&m_Device5)) && m_Device5)
		{
			D3D12_FEATURE_DATA_D3D12_OPTIONS5 options5{};
			if (SUCCEEDED(m_Device->CheckFeatureSupport(
				D3D12_FEATURE_D3D12_OPTIONS5, &options5, sizeof(options5))))
			{
				m_bRayTracingSupported =
					(options5.RaytracingTier >= D3D12_RAYTRACING_TIER_1_1);
			}
		}

		OutputDebugStringA(m_bRayTracingSupported
			? "[RenderManager] DXR inline raytracing: supported (Tier 1.1+)\n"
			: "[RenderManager] DXR inline raytracing: not supported (SWRT only)\n");
	}

#if defined(_DEBUG)
	// デバッグレイヤーのエラーメッセージが出た瞬間にブレークさせる。
	// 不正な API 呼び出しは放置すると後段で D3D12Core 内の
	// アクセス違反 (0xC0000005) という分かりにくい形で落ちるため、
	// 原因の呼び出し箇所そのもので停止するようにする。
	// DX12_DEBUG_NO_BREAK=1 のときはブレークせず、メッセージの出力のみ
	// (デバッガ無しの計測実行ではブレーク = 未処理例外で終了してしまうため)。
	if (!IsDebugEnvironmentSwitchEnabled("DX12_DEBUG_NO_BREAK"))
	{
		ComPtr<ID3D12InfoQueue> infoQueue;
		if (SUCCEEDED(m_Device.As(&infoQueue)))
		{
			infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, TRUE);
			infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, TRUE);
		}
	}
	else
	{
		OutputDebugStringA("[RenderManager] DX12_DEBUG_NO_BREAK: break on ERROR/CORRUPTION disabled\n");
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

		// DXR (加速構造ビルド) 用インターフェース。非対応環境では null のまま。
		// ComPtr::As は内部ポインタを参照するため、必ず生成成功の確認後に行う
		m_GraphicsCommandList.As(&m_GraphicsCommandList4);
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

		m_DepthBufferHandle = m_DepthBufferDescriptorHeap->GetCPUDescriptorHandleForHeapStart();
	}

	// デプスバッファ生成 (バックバッファ解像度で開始)
	CreateDepthBuffer((unsigned int)m_BackBufferWidth, (unsigned int)m_BackBufferHeight);
}


// デプスバッファ生成
// R32_TYPELESS で生成し、DSV は D32_FLOAT、SRV は R32_FLOAT として読む。
// DSV は 1 枠固定のヒープ (m_DepthBufferHandle) へ作り直す。DSV デスクリプタは
// OMSetRenderTargets の記録時点で消費されるため、未実行のコマンドが無ければ
// 同じ枠を上書きしてよい (呼び出し側が FlushAndReset + WaitGPU 済みであること)。
void RenderManager::CreateDepthBuffer(unsigned int Width, unsigned int Height)
{
	assert(m_DepthBuffer == nullptr && "CreateDepthBuffer: release the previous depth buffer first");

	D3D12_RESOURCE_DESC resourceDesc{};
	resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	resourceDesc.Width = Width;
	resourceDesc.Height = Height;
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

	HRESULT hr = m_Device->CreateCommittedResource(
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

	m_Device->CreateDepthStencilView(m_DepthBuffer.Get(), &dsvDesc, m_DepthBufferHandle);

	m_DepthBufferWidth = Width;
	m_DepthBufferHeight = Height;
}


// デプスバッファのリソースを遅延削除キューへ移す (DSV 枠は保持)。
// 深度 SRV は FSceneTextures 側の所有なので、そちらで別途解放すること。
void RenderManager::ReleaseDepthBuffer()
{
	if (m_DepthBuffer)
	{
		DeferredRelease(std::move(m_DepthBuffer));
	}
	m_DepthBuffer.Reset();
	m_DepthBufferWidth = 0;
	m_DepthBufferHeight = 0;
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

			D3D12_CPU_DESCRIPTOR_HANDLE handle = GetCPUDescriptorHandle(index);

			m_Device->CreateConstantBufferView(&desc, handle);
			m_ConstantBufferView[i][j] = index;
		}

		m_ConstantBufferIndex[i] = 0;
	}
}


// ---- 静的サンプラ (s0..s2) の記述を構築する ----
static void BuildStaticSamplerDescs(D3D12_STATIC_SAMPLER_DESC(&OutSamplers)[3])
{
	// s0: 異方性ラップ (アルベドなど通常テクスチャ用)
	OutSamplers[0].Filter = D3D12_FILTER_ANISOTROPIC;
	OutSamplers[0].AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
	OutSamplers[0].AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
	OutSamplers[0].AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
	OutSamplers[0].MipLODBias = 0.0f;
	OutSamplers[0].MaxAnisotropy = 4;
	OutSamplers[0].ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
	OutSamplers[0].BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
	OutSamplers[0].MinLOD = 0.0f;
	OutSamplers[0].MaxLOD = D3D12_FLOAT32_MAX;
	OutSamplers[0].ShaderRegister = 0;
	OutSamplers[0].RegisterSpace = 0;
	OutSamplers[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

	// s1: 線形クランプ (G-Buffer / IBL / LUT などフルスクリーン参照用)
	OutSamplers[1].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
	OutSamplers[1].AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	OutSamplers[1].AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	OutSamplers[1].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	OutSamplers[1].MipLODBias = 0.0f;
	OutSamplers[1].MaxAnisotropy = 16;
	OutSamplers[1].ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
	OutSamplers[1].BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
	OutSamplers[1].MinLOD = 0.0f;
	OutSamplers[1].MaxLOD = D3D12_FLOAT32_MAX;
	OutSamplers[1].ShaderRegister = 1;
	OutSamplers[1].RegisterSpace = 0;
	OutSamplers[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

	// s2: シャドウ比較サンプラ (SampleCmp 用。バイリニア比較 PCF)
	// ボーダー白 = シャドウマップ外は「影なし」扱い
	OutSamplers[2].Filter = D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
	OutSamplers[2].AddressU = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
	OutSamplers[2].AddressV = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
	OutSamplers[2].AddressW = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
	OutSamplers[2].MipLODBias = 0.0f;
	OutSamplers[2].MaxAnisotropy = 1;
	OutSamplers[2].ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
	OutSamplers[2].BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
	OutSamplers[2].MinLOD = 0.0f;
	OutSamplers[2].MaxLOD = D3D12_FLOAT32_MAX;
	OutSamplers[2].ShaderRegister = 2;
	OutSamplers[2].RegisterSpace = 0;
	OutSamplers[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
}


void RenderManager::InitRootSignature()
{
	const unsigned int ROOT_PARAM_COUNT = (unsigned int)TEXTURE_TYPE::COUNT;
	const unsigned int CBV_COUNT = (unsigned int)CONSTANT_TYPE::FOG + 1; // VIEW/PRIMITIVE/MATERIAL/FORWARD_LIGHT/POST_PROCESS/SHADOW/LUMEN/FOG

	// ルートパラメータは全てデスクリプタテーブル (1 DWORD ずつ) なので
	// 上限 64 DWORD に対して b0..b7 + t0..t38 = 47 DWORD
	static_assert((unsigned int)TEXTURE_TYPE::COUNT <= 64,
		"root signature exceeds 64 DWORDs (CONSTANT_TYPE + TEXTURE_TYPE)");

	D3D12_ROOT_PARAMETER  rootParameters[ROOT_PARAM_COUNT]{};
	D3D12_DESCRIPTOR_RANGE range[ROOT_PARAM_COUNT]{};

	// 定数バッファ (b0..b7)
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

	// 静的サンプラ (s0: 異方性ラップ / s1: 線形クランプ / s2: シャドウ比較)
	D3D12_STATIC_SAMPLER_DESC samplerDesc[3]{};
	BuildStaticSamplerDescs(samplerDesc);

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

	m_PipelineState["BasePass"] =
		CreatePipeline("Shader/cso/GeometryVS.cso", "Shader/cso/GeometryPS.cso", gbuffer, _countof(gbuffer));

	// ---- フルスクリーンパス (DSV をバインドしない) ----
	// 以下の 10 PSO (LinearDepth / DeferredLighting / HeightFog / Tonemap /
	// Bloom x3 / DOF x3) は OMSetRenderTargets に DSV を渡さずに描くため、
	// 深度無効 (EDepthStatePreset::None = DepthEnable FALSE + DSVFormat UNKNOWN)
	// で生成する。DSV 無しで DSVFormat = D32 の PSO を使うとデバッグレイヤーの
	// EXECUTION ERROR #615 になる (DSV 未バインド時は深度テストが働かないため
	// 描画結果は従来と同一)。
	m_PipelineState["LinearDepth"] =
		CreatePipeline("Shader/cso/DeferredVS.cso", "Shader/cso/LinearDepthPS.cso", depth, _countof(depth),
			0, 0.0f, EBlendStatePreset::Opaque, ECullModePreset::Back, EDepthStatePreset::None);

	m_PipelineState["DeferredLighting"] =
		CreatePipeline("Shader/cso/DeferredVS.cso", "Shader/cso/DeferredPS.cso", hdr, _countof(hdr),
			0, 0.0f, EBlendStatePreset::Opaque, ECullModePreset::Back, EDepthStatePreset::None);

	// ---- Exponential Height Fog パス (FogRendering.h) ----
	// デファードライティング後の HDR SceneColor に対し、深度から
	// 再構築したワールド座標で高さフォグ (+ Volumetric Fog の積分結果)
	// を合成するフルスクリーンパス。ブレンドは
	//   SceneColor' = Src.rgb + SceneColor * Src.a (RGB のみ書き込み)
	m_PipelineState["HeightFog"] =
		CreatePipeline("Shader/cso/DeferredVS.cso", "Shader/cso/HeightFogPS.cso", hdr, _countof(hdr),
			0, 0.0f, EBlendStatePreset::HeightFog, ECullModePreset::Back, EDepthStatePreset::None);

	// Tonemap pass: HDR SceneColor (+bloom) -> Post chain -> SDR back buffer
	m_PipelineState["PostProcessTonemap"] =
		CreatePipeline("Shader/cso/DeferredVS.cso", "Shader/cso/TonemapPS.cso", ldr, _countof(ldr),
			0, 0.0f, EBlendStatePreset::Opaque, ECullModePreset::Back, EDepthStatePreset::None);

	// ---- Bloom passes (all render to HDR R16G16B16A16 mips) ----
	m_PipelineState["PostProcessBloomThreshold"] =
		CreatePipeline("Shader/cso/DeferredVS.cso", "Shader/cso/BloomThresholdPS.cso", hdr, _countof(hdr),
			0, 0.0f, EBlendStatePreset::Opaque, ECullModePreset::Back, EDepthStatePreset::None);
	m_PipelineState["PostProcessBloomDownsample"] =
		CreatePipeline("Shader/cso/DeferredVS.cso", "Shader/cso/BloomDownsamplePS.cso", hdr, _countof(hdr),
			0, 0.0f, EBlendStatePreset::Opaque, ECullModePreset::Back, EDepthStatePreset::None);
	m_PipelineState["PostProcessBloomUpsample"] =
		CreatePipeline("Shader/cso/DeferredVS.cso", "Shader/cso/BloomUpsamplePS.cso", hdr, _countof(hdr),
			0, 0.0f, EBlendStatePreset::Opaque, ECullModePreset::Back, EDepthStatePreset::None);

	// ---- Depth of Field passes ----
	// CoC/prep -> half-res HDR (RGBA16F). Blur passes ping-pong at half
	// res. Composite writes back to full-res HDR SceneColor.
	m_PipelineState["PostProcessDOFCoC"] =
		CreatePipeline("Shader/cso/DeferredVS.cso", "Shader/cso/DOFCoCPS.cso", hdr, _countof(hdr),
			0, 0.0f, EBlendStatePreset::Opaque, ECullModePreset::Back, EDepthStatePreset::None);
	m_PipelineState["PostProcessDOFBlur"] =
		CreatePipeline("Shader/cso/DeferredVS.cso", "Shader/cso/DOFBlurPS.cso", hdr, _countof(hdr),
			0, 0.0f, EBlendStatePreset::Opaque, ECullModePreset::Back, EDepthStatePreset::None);
	m_PipelineState["PostProcessDOFComposite"] =
		CreatePipeline("Shader/cso/DeferredVS.cso", "Shader/cso/DOFCompositePS.cso", hdr, _countof(hdr),
			0, 0.0f, EBlendStatePreset::Opaque, ECullModePreset::Back, EDepthStatePreset::None);

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
	// (FSceneRenderer::RenderTranslucency)。
	// シェーダは両モード共通 (TranslucentPS)。ブレンドステート:
	//   Translucent = SrcAlpha / InvSrcAlpha, Additive = SrcAlpha / One
	// Additive は順序非依存のため深度テストのみ (DepthRead) の 1 パスで描き、
	// Translucent は下の深度プリパス + Equal PSO の 2 パスで描く。
	// Translucency / TranslucencyTwoSided は ETranslucencyDrawMode::Standard
	// (深度プリパスを使わない 1 パス合成。深度はテストのみ = DepthRead) 用。
	// 現在の RenderTranslucency は DepthPrepass / ColorEqual のみを渡すため未使用。
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
	// Translucent / Additive はシャドウマップに描かない。
	m_PipelineState["ShadowDepthTwoSided"] =
		CreatePipeline("Shader/cso/ShadowDepthVS.cso", "Shader/cso/ShadowDepthPS.cso", nullptr, 0, 1000, 1.5f,
			EBlendStatePreset::Opaque, ECullModePreset::None);
	m_PipelineState["ShadowDepthMasked"] =
		CreatePipeline("Shader/cso/ShadowDepthVS.cso", "Shader/cso/ShadowDepthMaskedPS.cso", nullptr, 0, 1000, 1.5f);
	m_PipelineState["ShadowDepthMaskedTwoSided"] =
		CreatePipeline("Shader/cso/ShadowDepthVS.cso", "Shader/cso/ShadowDepthMaskedPS.cso", nullptr, 0, 1000, 1.5f,
			EBlendStatePreset::Opaque, ECullModePreset::None);

	// ---- Lumen カードキャプチャ (Surface Cache, LumenScene.h) ----
	// メッシュをカードのオルソ投影でアトラスタイルへ焼く MRT パス:
	//   RT0 = Albedo (RGBA8) / RT1 = Normal (RGBA8) /
	//   RT2 = Emissive (R11G11B10F) + 深度アトラス (D32)
	// VS はベースパスと共通 (b1 = 単位行列でローカル空間描画)。
	// 面の欠け防止のため常にカリング無効 (裏面は PS で法線反転)。
	const DXGI_FORMAT lumenCard[] =
	{
		DXGI_FORMAT_R8G8B8A8_UNORM,
		DXGI_FORMAT_R8G8B8A8_UNORM,
		DXGI_FORMAT_R11G11B10_FLOAT,
	};
	m_PipelineState["LumenCardCapture"] =
		CreatePipeline("Shader/cso/GeometryVS.cso", "Shader/cso/LumenCardCapturePS.cso",
			lumenCard, _countof(lumenCard), 0, 0.0f,
			EBlendStatePreset::Opaque, ECullModePreset::None);

	// ---- 一次空間アップスケール ----
	// トーンマップ済み LDR (ポスト解像度 P) -> バックバッファ (出力解像度 O)。
	// DSV を渡さないフルスクリーンパスなので深度無効。名前の添字 = UpscaleQuality。
	// オプション PSO: .cso が欠落していても起動を止めず、登録もしない
	// (FSceneRenderer::SelectPrimaryUpscalePipeline が HasPipelineState で確かめ、
	//  Bilinear (1) -> トーンマップ統合 (bilinear) の順にフォールバックする)
	{
		static const char* const kUpscalePixelShaders[] =
		{
			"Shader/cso/PostProcessUpscale_Nearest_PS.cso",		// 0 Nearest
			"Shader/cso/PostProcessUpscale_Bilinear_PS.cso",	// 1 Bilinear
			"Shader/cso/PostProcessUpscale_Directional_PS.cso",	// 2 Directional blur + unsharp mask
			"Shader/cso/PostProcessUpscale_CatmullRom_PS.cso",	// 3 5 タップ Catmull-Rom (既定)
			"Shader/cso/PostProcessUpscale_Lanczos_PS.cso",		// 4 Lanczos-3 (13 タップ)
			"Shader/cso/PostProcessUpscale_Gaussian_PS.cso",	// 5 Gaussian unsharp
		};
		static_assert(_countof(kUpscalePixelShaders) == (int)EUpscaleMethod::Count, "one .cso per EUpscaleMethod");
		for (int i = 0; i < (int)_countof(kUpscalePixelShaders); ++i)
		{
			ComPtr<ID3D12PipelineState> pso =
				CreatePipeline("Shader/cso/DeferredVS.cso", kUpscalePixelShaders[i], ldr, _countof(ldr),
					0, 0.0f, EBlendStatePreset::Opaque, ECullModePreset::Back, EDepthStatePreset::None,
					true);	// bOptional
			if (pso)
			{
				m_PipelineState[GetPrimaryUpscalePipelineName(i)] = pso;	// "PostProcessUpscale<i>" (PostProcessUpscale.cpp の表)
			}
		}
	}

	// ---- ベロシティパス (FSceneRenderer::RenderVelocities, VelocityRendering.cpp) ----
	// 動いたプリミティブ (Opaque / Masked サブセット) だけを R16G16_UNORM の Velocity へ描く。
	// ベースパス深度を DSV にバインドして LESS_EQUAL テストのみ (書き込み無し)。VS はベースパスと
	// 同じ GetBasePassClipPosition (precise) なので深度はビット一致する。
	// 一致しない環境向けのフォールバックは DepthBias = kVelocityDepthBias (-4, リスク R1)。
	// Masked は BaseColor テクスチャがある時だけ clip 版 (シャドウ深度と同じ規則)
	{
		const DXGI_FORMAT velocity[] = { DXGI_FORMAT_R16G16_UNORM };
		const int kVelocityDepthBias = 0;	// リスク R1 のフォールバックでは -4 (D32 数 ULP 手前へ)
		m_PipelineState["Velocity"] =
			CreatePipeline("Shader/cso/VelocityVS.cso", "Shader/cso/VelocityPS.cso", velocity, _countof(velocity),
				kVelocityDepthBias, 0.0f, EBlendStatePreset::Opaque, ECullModePreset::Back, EDepthStatePreset::DepthRead);
		m_PipelineState["VelocityTwoSided"] =
			CreatePipeline("Shader/cso/VelocityVS.cso", "Shader/cso/VelocityPS.cso", velocity, _countof(velocity),
				kVelocityDepthBias, 0.0f, EBlendStatePreset::Opaque, ECullModePreset::None, EDepthStatePreset::DepthRead);
		m_PipelineState["VelocityMasked"] =
			CreatePipeline("Shader/cso/VelocityVS.cso", "Shader/cso/VelocityMaskedPS.cso", velocity, _countof(velocity),
				kVelocityDepthBias, 0.0f, EBlendStatePreset::Opaque, ECullModePreset::Back, EDepthStatePreset::DepthRead);
		m_PipelineState["VelocityMaskedTwoSided"] =
			CreatePipeline("Shader/cso/VelocityVS.cso", "Shader/cso/VelocityMaskedPS.cso", velocity, _countof(velocity),
				kVelocityDepthBias, 0.0f, EBlendStatePreset::Opaque, ECullModePreset::None, EDepthStatePreset::DepthRead);
	}

	// ---- Responsive AA マスク (FSceneRenderer::RenderResponsiveAAMask) ----
	// bEnableResponsiveAA の Translucent / Additive サブセットを R8_UNORM のマスクへ 1 で描く。
	// RenderTranslucency の最後に半透明深度プリパスの
	// 深度を DSV にバインドしたまま描く: VS は半透明と同じ GeometryVS (GetBasePassClipPosition, precise)
	// なので LESS_EQUAL (書き込み無し) がビット一致で通り、最前面の Translucent 層だけが残る
	{
		const DXGI_FORMAT responsive[] = { DXGI_FORMAT_R8_UNORM };
		m_PipelineState["ResponsiveAA"] =
			CreatePipeline("Shader/cso/GeometryVS.cso", "Shader/cso/ResponsiveAAPS.cso", responsive, _countof(responsive),
				0, 0.0f, EBlendStatePreset::Opaque, ECullModePreset::Back, EDepthStatePreset::DepthRead);
		m_PipelineState["ResponsiveAATwoSided"] =
			CreatePipeline("Shader/cso/GeometryVS.cso", "Shader/cso/ResponsiveAAPS.cso", responsive, _countof(responsive),
				0, 0.0f, EBlendStatePreset::Opaque, ECullModePreset::None, EDepthStatePreset::DepthRead);
	}

	// ---- Temporal AA デバッグ表示 (FSceneRenderer::AddVisualizeTemporalAAPass) ----
	// DeferredVS / VisualizeTemporalAAPS でバックバッファ (出力解像度 O) へ上書きする。
	// DSV を渡さないフルスクリーンパスなので深度無効。オプション PSO (.cso 欠落時は登録しない。
	// 呼び出し側が HasPipelineState で確かめ、無ければログ 1 回で可視化をスキップする)
	{
		ComPtr<ID3D12PipelineState> pso =
			CreatePipeline("Shader/cso/DeferredVS.cso", "Shader/cso/VisualizeTemporalAAPS.cso", ldr, _countof(ldr),
				0, 0.0f, EBlendStatePreset::Opaque, ECullModePreset::Back, EDepthStatePreset::None,
				true);	// bOptional
		if (pso)
		{
			m_PipelineState["VisualizeTemporalAA"] = pso;
		}
	}

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
//  Frame (BeginFrame / Present は FSceneRenderer、WaitGPU は終了時 / FlushAndResetCommandList から呼ばれる)
// ============================================================
void RenderManager::WaitGPU()
{
	m_CommandQueue->Signal(m_Fence.Get(), m_Frame[m_RTIndex]);

	m_Fence->SetEventOnCompletion(m_Frame[m_RTIndex], m_FenceEvent);
	WaitForSingleObjectEx(m_FenceEvent, INFINITE, FALSE);

	// GPU 完全アイドルなので保留中の遅延削除をすべて実解放できる
	FlushDeferredReleases(m_Fence->GetCompletedValue());

	m_Frame[m_RTIndex]++;
}


// シェーダ可視ヒープ / ルートシグネチャ / ビューポート / シザー
// (BeginFrame と Reset 後の復帰で共通)
void RenderManager::SetDefaultGraphicsState()
{
	// シェーダ可視デスクリプタヒープ + ルートシグネチャ
	ID3D12DescriptorHeap* dh[] = { m_SRVDescriptorHeap.Get() };
	m_GraphicsCommandList->SetDescriptorHeaps(_countof(dh), dh);
	m_GraphicsCommandList->SetGraphicsRootSignature(m_RootSignature.Get());

	// ビューポート / シザー
	m_GraphicsCommandList->RSSetViewports(1, &m_Viewport);
	m_GraphicsCommandList->RSSetScissorRects(1, &m_ScissorRect);
}


// フレーム先頭: シェーダ可視ヒープ / ルートシグネチャ / 定数リング /
// ビューポートを設定する。パス列 (G-Buffer -> デファード -> ポスプロ)
// は FSceneRenderer が駆動する。
void RenderManager::BeginFrame()
{
	SetDefaultGraphicsState();

	// 定数バッファのリングインデックス初期化
	m_ConstantBufferIndex[m_RTIndex] = 0;
}


// フレーム末尾: Close -> Execute -> Present -> 前フレーム待ち -> Reset
void RenderManager::Present()
{
	HRESULT hr;

	// ---- コマンド発行 ----
	{
		hr = m_GraphicsCommandList->Close();
		assert(SUCCEEDED(hr));

		ID3D12CommandList* const command_lists[1] = { m_GraphicsCommandList.Get() };
		m_CommandQueue->ExecuteCommandLists(1, command_lists);
		m_CommandQueue->Signal(m_Fence.Get(), m_Frame[m_RTIndex]);
	}

	hr = m_SwapChain->Present(1, 0);
	assert(SUCCEEDED(hr));

	// ---- 前フレーム待ち (フェンス) ----
	{
		UINT64 frame = m_Frame[m_RTIndex];

		m_RTIndex = m_SwapChain->GetCurrentBackBufferIndex();

		if (m_Fence->GetCompletedValue() < m_Frame[m_RTIndex])
		{
			m_Fence->SetEventOnCompletion(m_Frame[m_RTIndex], m_FenceEvent);
			WaitForSingleObjectEx(m_FenceEvent, INFINITE, FALSE);
		}

		// フェンス到達済みの遅延削除エントリを実解放する
		FlushDeferredReleases(m_Fence->GetCompletedValue());

		m_Frame[m_RTIndex] = frame + 1;
	}

	hr = m_GraphicsCommandAllocator[m_RTIndex]->Reset();
	assert(SUCCEEDED(hr));

	hr = m_GraphicsCommandList->Reset(m_GraphicsCommandAllocator[m_RTIndex].Get(), nullptr);
	assert(SUCCEEDED(hr));
}


// ============================================================
//  Resource creation
// ============================================================
// ---- DDS フォーマットごとの bpp / ブロックサイズ ----
// (WriteToSubresource の幅・高さ算出用。LoadTexture から呼ばれる)
static void GetDDSFormatBlockInfo(DXGI_FORMAT Format, unsigned int& OutBpp, unsigned int& OutBlock)
{
	switch (Format)
	{
		// BC1: 4bpp ブロック圧縮 (UNORM / sRGB 同レイアウト)
	case DXGI_FORMAT_BC1_UNORM:
	case DXGI_FORMAT_BC1_UNORM_SRGB:
		OutBpp = 4;  OutBlock = 4;  break;

		// BC2/BC3/BC7: 8bpp ブロック圧縮 (UNORM / sRGB 同レイアウト)
	case DXGI_FORMAT_BC2_UNORM:
	case DXGI_FORMAT_BC2_UNORM_SRGB:
	case DXGI_FORMAT_BC3_UNORM:
	case DXGI_FORMAT_BC3_UNORM_SRGB:
	case DXGI_FORMAT_BC7_UNORM:
	case DXGI_FORMAT_BC7_UNORM_SRGB:
		OutBpp = 8;  OutBlock = 4;  break;

		// BC6H: 8bpp ブロック圧縮 HDR (sRGB バリアントなし)
	case DXGI_FORMAT_BC6H_UF16:
	case DXGI_FORMAT_BC6H_SF16:
		OutBpp = 8;  OutBlock = 4;  break;

		// BC4: 4bpp ブロック圧縮 単チャンネル (ハイトマップ / マスク等)
	case DXGI_FORMAT_BC4_UNORM:
	case DXGI_FORMAT_BC4_SNORM:
		OutBpp = 4;  OutBlock = 4;  break;

		// BC5: 8bpp ブロック圧縮 2チャンネル (法線マップの標準形式)
		// ※ default (32bpp/block1) に落ちると WriteToSubresource の
		//    D3D12_BOX が実際の 1/4 の行数になりテクスチャが破損する
	case DXGI_FORMAT_BC5_UNORM:
	case DXGI_FORMAT_BC5_SNORM:
		OutBpp = 8;  OutBlock = 4;  break;

		// 非圧縮 32bit (R8G8B8A8 / B8G8R8A8, UNORM or sRGB)
	default:
		OutBpp = 32; OutBlock = 1;  break;
	}
}


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


	// フォーマットごとの bpp / ブロックサイズ (WriteToSubresource 用)
	unsigned int bpp, block;
	GetDDSFormatBlockInfo(desc.Format, bpp, block);

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


std::unique_ptr<RENDER_TARGET> RenderManager::CreateRenderTarget(unsigned int Width, unsigned int Height, DXGI_FORMAT Format, unsigned int MipLevels, bool bAllowUnorderedAccess)
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
	if (bAllowUnorderedAccess)
	{
		// コンピュートの書き込み先 (TAA 出力 / 履歴など)
		desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
	}
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
	renderTarget->SRVHandle = GetGPUDescriptorHandle(renderTarget->SRVIndex);
	renderTarget->RTVIndex = CreateRenderTargetView(renderTarget->Resource.Get());
	renderTarget->RTVHandle = GetRenderTargetViewHandle(renderTarget->RTVIndex);

	// UAV (ミップ 0)。SRV と同じシェーダ可視ヒープの別枠に作る
	if (bAllowUnorderedAccess)
	{
		renderTarget->UAVIndex = AllocateSRVSlot();

		D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
		uavDesc.Format = Format;
		uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
		uavDesc.Texture2D.MipSlice = 0;
		uavDesc.Texture2D.PlaneSlice = 0;

		m_Device->CreateUnorderedAccessView(renderTarget->Resource.Get(), nullptr, &uavDesc,
			GetCPUDescriptorHandle(renderTarget->UAVIndex));
		renderTarget->UAVHandle = GetGPUDescriptorHandle(renderTarget->UAVIndex);
	}

	renderTarget->Width = Width;
	renderTarget->Height = Height;
	renderTarget->Format = Format;

	return renderTarget;
}


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


// ============================================================
//  Binding
// ============================================================
void RenderManager::BindRootTableBySRVIndex(unsigned int RootParameter, unsigned int SRVIndex)
{
	D3D12_GPU_DESCRIPTOR_HANDLE handle = GetGPUDescriptorHandle(SRVIndex);

	m_GraphicsCommandList->SetGraphicsRootDescriptorTable(RootParameter, handle);
}


void RenderManager::SetConstant(CONSTANT_TYPE Type, const void* Constant, unsigned int Size)
{
	// 1 スロット (CONSTANT_BUFFER_SIZE = 512B) を超える定数は隣接スロット
	// (= 他の描画の定数) を上書きしてしまう。Release でも検出できるよう
	// ログを出して呼び出しごとスキップする (定数構造体の肥大化の検知)。
	assert(Size <= CONSTANT_BUFFER_SIZE && "SetConstant: constant larger than one ring slot");
	if (Size > CONSTANT_BUFFER_SIZE)
	{
		char msg[160];
		sprintf_s(msg, "[RenderManager] SetConstant: size %u exceeds CONSTANT_BUFFER_SIZE (%u), type %d skipped\n",
			Size, CONSTANT_BUFFER_SIZE, (int)Type);
		OutputDebugStringA(msg);
		return;
	}

	const unsigned int slot = m_ConstantBufferIndex[m_RTIndex];
	assert(slot < CONSTANT_BUFFER_MAX);

	// Release ビルドでは assert が消えるため実行時ガードを併設する。
	// リング溢れ時にそのまま進むと、マップ済みアップロードヒープ外への
	// memcpy (ヒープ破壊) と m_ConstantBufferView の範囲外読みが無警告で
	// 発生する。溢れた描画は直前バインドの定数のまま描かれる (表示は
	// 乱れるがメモリ破壊よりは安全)。
	if (slot >= CONSTANT_BUFFER_MAX)
	{
		OutputDebugStringA("[RenderManager] SetConstant: constant ring overflow (CONSTANT_BUFFER_MAX exceeded)\n");
		return;
	}

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


void RenderManager::SetVertexBuffer(const VERTEX_BUFFER* VertexBuffer)
{
	D3D12_VERTEX_BUFFER_VIEW vertexView{};
	vertexView.BufferLocation = VertexBuffer->Resource->GetGPUVirtualAddress();
	vertexView.StrideInBytes = VertexBuffer->Stride;
	vertexView.SizeInBytes = VertexBuffer->Stride * VertexBuffer->Size;

	m_GraphicsCommandList->IASetVertexBuffers(0, 1, &vertexView);
}


void RenderManager::SetIndexBuffer(const INDEX_BUFFER* IndexBuffer)
{
	D3D12_INDEX_BUFFER_VIEW indexView{};
	indexView.BufferLocation = IndexBuffer->Resource->GetGPUVirtualAddress();
	indexView.SizeInBytes = sizeof(unsigned int) * IndexBuffer->Size;
	indexView.Format = DXGI_FORMAT_R32_UINT;

	m_GraphicsCommandList->IASetIndexBuffer(&indexView);
}


// 登録済みかつ非 null の PSO か (オプション PSO を SetPipelineState する前の確認用。§3.8)
bool RenderManager::HasPipelineState(const char* PipelineName) const
{
	auto it = m_PipelineState.find(PipelineName);
	return (it != m_PipelineState.end()) && (it->second.Get() != nullptr);
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
//  EBlendStatePreset / ECullModePreset / EDepthStatePreset
//  (TStaticBlendState / TStaticRasterizerState /
//   TStaticDepthStencilState 相当) から PSO を構築する。
// ============================================================
// ---- シェーダバイトコード読み込み (.cso をそのまま読み込む) ----
// 戻り値: 読み込めたら true。bAssertOnMissing = false (オプション PSO 用) の時は
// 欠落 / 空でもログ・assert を出さずに false を返す (呼び出し側がまとめて 1 行ログを出す)
static bool LoadShaderBytecode(const char* Path, std::vector<char>& OutData, D3D12_SHADER_BYTECODE& OutBytecode,
	bool bAssertOnMissing = true)
{
	// 欠落 / 空の .cso (シェーダのコンパイル失敗や未再コンパイル)
	// を null バイトコードのまま PSO 生成に渡すと、
	// CreateGraphicsPipelineState 内のアクセス違反になるため
	// ここで即検知する。
	OutBytecode.pShaderBytecode = nullptr;
	OutBytecode.BytecodeLength = 0;

	std::ifstream file(Path, std::ios_base::in | std::ios_base::binary);
	if (!file)
	{
		if (bAssertOnMissing)
		{
			char msg[512];
			sprintf_s(msg, "[RenderManager] shader .cso not found: %s\n", Path);
			OutputDebugStringA(msg);
			assert(false && "shader .cso not found");
		}
		return false;
	}

	file.seekg(0, std::ios_base::end);
	int filesize = (int)file.tellg();
	file.seekg(0, std::ios_base::beg);

	if (filesize <= 0)
	{
		if (bAssertOnMissing)
		{
			char msg[512];
			sprintf_s(msg, "[RenderManager] shader .cso is empty (compile failed?): %s\n", Path);
			OutputDebugStringA(msg);
			assert(false && "shader .cso is empty");
		}
		return false;
	}

	OutData.resize(filesize);
	file.read(OutData.data(), filesize);
	file.close();

	OutBytecode.pShaderBytecode = OutData.data();
	OutBytecode.BytecodeLength = filesize;
	return true;
}


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


// ラスタライザ
// bTwoSided (ECullModePreset::None) はカリング無効
static D3D12_RASTERIZER_DESC BuildRasterizerStateDesc(ECullModePreset CullPreset, int DepthBias, float SlopeScaledDepthBias)
{
	D3D12_RASTERIZER_DESC Desc{};
	Desc.CullMode =
		(CullPreset == ECullModePreset::None) ? D3D12_CULL_MODE_NONE : D3D12_CULL_MODE_BACK;
	Desc.FillMode = D3D12_FILL_MODE_SOLID;
	Desc.FrontCounterClockwise = FALSE;
	Desc.DepthBias = DepthBias;
	Desc.DepthBiasClamp = 0.0f;
	Desc.SlopeScaledDepthBias = SlopeScaledDepthBias;
	Desc.DepthClipEnable = FALSE;
	Desc.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;
	Desc.AntialiasedLineEnable = FALSE;
	Desc.MultisampleEnable = FALSE;

	return Desc;
}


// ブレンド (EBlendMode -> TStaticBlendState 相当のプリセット)
//   Opaque / Masked : One / Zero 上書き (従来通り)
//   Translucent     : SrcAlpha / InvSrcAlpha (BLEND_Translucent)
//   Additive        : SrcAlpha / One (BLEND_Additive)
//   HeightFog       : One / SrcAlpha, RGB のみ書き込み (フォグパス。
//                     Dst x 透過率 + インスキャッタ)
// α チャンネルは合成後のシーンカバレッジ規約 (Zero / InvSrcAlpha 系)
// ※ Substrate バッファ (BasePass RT3/RT4 = R32G32B32A32_UINT) の
//   ような整数 RT はブレンド不可なので、その RT だけ BlendEnable を
//   FALSE にする (IndependentBlendEnable が必要)。未使用スロットも
//   FALSE に落とす。ブレンド係数は無効 RT では無視される。
static D3D12_BLEND_DESC BuildBlendStateDesc(EBlendStatePreset BlendPreset, const DXGI_FORMAT* RTVFormats, unsigned int NumRenderTargets)
{
	D3D12_BLEND_DESC Desc{};
	for (int i = 0; i < _countof(Desc.RenderTarget); ++i)
	{
		auto& rt = Desc.RenderTarget[i];

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

		case EBlendStatePreset::HeightFog:
			// フォグパス: Dst * Src.a (透過率) + Src.rgb (インスキャッタ)。
			// α は書かない (SceneColor の α は未使用のまま保持)
			rt.SrcBlend = D3D12_BLEND_ONE;
			rt.DestBlend = D3D12_BLEND_SRC_ALPHA;
			rt.SrcBlendAlpha = D3D12_BLEND_ZERO;
			rt.DestBlendAlpha = D3D12_BLEND_ONE;
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
		if (BlendPreset == EBlendStatePreset::NoColorWrite)
		{
			rt.RenderTargetWriteMask = 0u;
		}
		else if (BlendPreset == EBlendStatePreset::HeightFog)
		{
			// CW_RGB 相当
			rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_RED
				| D3D12_COLOR_WRITE_ENABLE_GREEN | D3D12_COLOR_WRITE_ENABLE_BLUE;
		}
		else
		{
			rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
		}
		rt.LogicOpEnable = FALSE;
		rt.LogicOp = D3D12_LOGIC_OP_CLEAR;
	}
	Desc.AlphaToCoverageEnable = FALSE;
	// RT ごとに BlendEnable が異なる (浮動小数 RT = ON / 整数 RT = OFF)
	// ため独立ブレンドを有効化する
	Desc.IndependentBlendEnable = TRUE;

	return Desc;
}


// デプス・ステンシル (EDepthStatePreset -> TStaticDepthStencilState 相当)
static D3D12_DEPTH_STENCIL_DESC BuildDepthStencilStateDesc(EDepthStatePreset DepthPreset)
{
	D3D12_DEPTH_STENCIL_DESC Desc{};
	// None (DSV をバインドしないフルスクリーンパス) は深度テスト自体を無効化
	Desc.DepthEnable = (DepthPreset == EDepthStatePreset::None) ? FALSE : TRUE;
	// DepthReadEqual (半透明深度プリパスの着色パス) はプリパスが書いた
	// 最前面深度と一致するフラグメントのみ通す
	if (DepthPreset == EDepthStatePreset::None)
	{
		Desc.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
	}
	else
	{
		Desc.DepthFunc =
			(DepthPreset == EDepthStatePreset::DepthReadEqual)
			? D3D12_COMPARISON_FUNC_EQUAL
			: D3D12_COMPARISON_FUNC_LESS_EQUAL;
	}
	// トランスルーセンシー (DepthRead / DepthReadEqual) は深度テストのみ
	// (書き込み無効)
	Desc.DepthWriteMask =
		(DepthPreset == EDepthStatePreset::DepthWrite)
		? D3D12_DEPTH_WRITE_MASK_ALL
		: D3D12_DEPTH_WRITE_MASK_ZERO;
	Desc.StencilEnable = FALSE;
	Desc.StencilReadMask = D3D12_DEFAULT_STENCIL_READ_MASK;
	Desc.StencilWriteMask = D3D12_DEFAULT_STENCIL_WRITE_MASK;

	Desc.FrontFace.StencilFailOp = D3D12_STENCIL_OP_KEEP;
	Desc.FrontFace.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
	Desc.FrontFace.StencilPassOp = D3D12_STENCIL_OP_KEEP;
	Desc.FrontFace.StencilFunc = D3D12_COMPARISON_FUNC_ALWAYS;

	Desc.BackFace.StencilFailOp = D3D12_STENCIL_OP_KEEP;
	Desc.BackFace.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
	Desc.BackFace.StencilPassOp = D3D12_STENCIL_OP_KEEP;
	Desc.BackFace.StencilFunc = D3D12_COMPARISON_FUNC_ALWAYS;

	return Desc;
}


ComPtr<ID3D12PipelineState> RenderManager::CreatePipeline(const char* VertexShaderFile, const char* PixelShaderFile, const DXGI_FORMAT* RTVFormats, unsigned int NumRenderTargets, int DepthBias, float SlopeScaledDepthBias, EBlendStatePreset BlendPreset, ECullModePreset CullPreset, EDepthStatePreset DepthPreset, bool bOptional)
{
	D3D12_GRAPHICS_PIPELINE_STATE_DESC pipelineStateDesc{};

	// ---- シェーダバイトコード ----
	std::vector<char> vertexShader;
	std::vector<char> pixelShader;
	const bool bHasVS = LoadShaderBytecode(VertexShaderFile, vertexShader, pipelineStateDesc.VS, !bOptional);
	const bool bHasPS = LoadShaderBytecode(PixelShaderFile, pixelShader, pipelineStateDesc.PS, !bOptional);

	// オプション PSO: .cso が欠落 / 空なら PSO を作らず nullptr を返す (assert しない)。
	// PS 無しでも CreateGraphicsPipelineState は成功して「何も描かない非 null の PSO」が
	// できてしまい null 判定で検出できないため、ここで生成自体を止める。
	// 呼び出し側は HasPipelineState で存在を確かめてから SetPipelineState する (§3.8)
	if (bOptional && (!bHasVS || !bHasPS))
	{
		char msg[512];
		sprintf_s(msg, "[RenderManager] optional pipeline skipped (.cso missing or empty): VS=%s%s PS=%s%s\n",
			VertexShaderFile, bHasVS ? "" : " (missing)", PixelShaderFile, bHasPS ? "" : " (missing)");
		OutputDebugStringA(msg);
		return nullptr;
	}

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

	// ---- ステートプリセット -> 各ステート記述 ----
	pipelineStateDesc.RasterizerState = BuildRasterizerStateDesc(CullPreset, DepthBias, SlopeScaledDepthBias);
	pipelineStateDesc.BlendState = BuildBlendStateDesc(BlendPreset, RTVFormats, NumRenderTargets);
	pipelineStateDesc.DepthStencilState = BuildDepthStencilStateDesc(DepthPreset);
	// 深度無効プリセットは DSV 無しで描くため DSV フォーマットも UNKNOWN にする
	// (DSV 未バインド + DSVFormat = D32 はデバッグレイヤー #615)
	pipelineStateDesc.DSVFormat =
		(DepthPreset == EDepthStatePreset::None) ? DXGI_FORMAT_UNKNOWN : DXGI_FORMAT_D32_FLOAT;

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
		// オプション PSO はログのみ (null を返し、呼び出し側が登録しない)
		assert(bOptional && "CreateGraphicsPipelineState failed");
		pipelineState.Reset();
	}

	return pipelineState;
}


// ============================================================
//  Descriptor management
//  SRV / RTV ヒープのフリーリスト割当と各ビュー生成。
// ============================================================
unsigned int RenderManager::AllocateSRVSlot()
{
	// 枯渇時の front() は未定義動作 (空 list の参照) になるため明示的に止める。
	// Lumen 追加でデスクリプタ消費が増えたので、上限超過を静かに壊れる形では
	// なくここで検出できるようにする。
	if (m_SRVDescriptorPool.empty())
	{
		OutputDebugStringA("[RenderManager] SRV descriptor pool exhausted (SRV_DESCRIPTOR_MAX)\n");
		assert(false && "SRV descriptor pool exhausted");
		return 0;
	}

	unsigned int index = m_SRVDescriptorPool.front();
	m_SRVDescriptorPool.pop_front();
	return index;
}


unsigned int RenderManager::AllocateRTVSlot()
{
	// SRV と同様、枯渇時の front() (空 list の参照) を明示的に止める
	if (m_RTVDescriptorPool.empty())
	{
		OutputDebugStringA("[RenderManager] RTV descriptor pool exhausted (RTV_DESCRIPTOR_MAX)\n");
		assert(false && "RTV descriptor pool exhausted");
		return 0;
	}

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

	D3D12_CPU_DESCRIPTOR_HANDLE handle = GetCPUDescriptorHandle(index);

	D3D12_RESOURCE_DESC resDesc = Resource->GetDesc();

	D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
	srvDesc.Format = resDesc.Format;
	srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	srvDesc.Texture2D.MipLevels = resDesc.MipLevels;

	m_Device->CreateShaderResourceView(Resource, &srvDesc, handle);

	return index;
}


void RenderManager::ReleaseShaderResourceView(unsigned int SRVIndex)
{
	// 即時返却すると次の Allocate が同じ枠を掴み、in-flight の
	// コマンドリストが読んでいる shader-visible デスクリプタを
	// 上書きしてしまうため、遅延削除キュー経由で返却する。
	DeferredRelease(nullptr, (int)SRVIndex, -1);
}


// ※ MipLevel は現状未反映 (desc = nullptr のため常にミップ 0 の RTV になる)。
//    ミップ別 RTV が必要になったら D3D12_RENDER_TARGET_VIEW_DESC の MipSlice に渡すこと。
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
	// SRV 同様、遅延削除キュー経由で返却する。
	DeferredRelease(nullptr, -1, (int)RTVIndex);
}


// ============================================================
//  Deferred Deletion (遅延削除)
// ============================================================
void RenderManager::DeferredRelease(ComPtr<ID3D12Resource> Resource,
	int SRVIndex, int RTVIndex)
{
	DEFERRED_RELEASE_ENTRY entry;

	// m_Frame[m_RTIndex] は「現在記録中のフレームが Present 時に
	// Signal する値」。この値の完了を待てば、1つ前の in-flight
	// フレーム (より小さい値) の完了も保証される。
	entry.FenceValue = m_Frame[m_RTIndex];
	entry.Resource = std::move(Resource);
	entry.SRVIndex = SRVIndex;
	entry.RTVIndex = RTVIndex;

	m_DeferredReleaseQueue.push_back(std::move(entry));
}


void RenderManager::FlushDeferredReleases(UINT64 CompletedFenceValue)
{
	// FenceValue は enqueue 順に単調非減少なので先頭から見るだけでよい
	while (!m_DeferredReleaseQueue.empty() &&
		m_DeferredReleaseQueue.front().FenceValue <= CompletedFenceValue)
	{
		DEFERRED_RELEASE_ENTRY& entry = m_DeferredReleaseQueue.front();

		if (entry.SRVIndex >= 0)
		{
			m_SRVDescriptorPool.push_front((unsigned int)entry.SRVIndex);
		}
		if (entry.RTVIndex >= 0)
		{
			m_RTVDescriptorPool.push_front((unsigned int)entry.RTVIndex);
		}

		// pop で ComPtr が解放される (リソース本体の実解放)
		m_DeferredReleaseQueue.pop_front();
	}
}


// ============================================================
//  Resource destructors (リソース本体 + デスクリプタ枠を遅延解放)
// ============================================================
TEXTURE::~TEXTURE()
{
	// Resource の所有権を遅延削除キューへ移し、GPU が現在記録中の
	// フレームを完了するまで生存させる (即時解放だと in-flight の
	// 描画が解放済みリソースを参照して DEVICE_REMOVED になる)。
	RenderManager::GetInstance()->DeferredRelease(
		std::move(Resource), (int)SRVIndex, -1);
}


RENDER_TARGET::~RENDER_TARGET()
{
	RenderManager* rhi = RenderManager::GetInstance();

	// UAV 枠 (bAllowUnorderedAccess で生成した場合のみ) も遅延削除キュー経由で返却
	if (UAVIndex != UINT_MAX)
	{
		rhi->ReleaseShaderResourceView(UAVIndex);
	}

	rhi->DeferredRelease(
		std::move(Resource), (int)SRVIndex, (int)RTVIndex);
}


// ============================================================
//  Accessors used by baker / compute systems
//  (IBLBaker / AutoExposure / ColorGradingLUTBaker / LightGrid / DFAtlas)
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


// ローカル (ビデオ) メモリの現在使用量 [byte]。解像度変更のリーク / VRAM 検査用。
// m_Adapter は EnumAdapters (IDXGIAdapter) の結果を格納しているため、
// QueryInterface (As) で正しく IDXGIAdapter3 を取得してから問い合わせる。
UINT64 RenderManager::QueryLocalVideoMemoryUsage()
{
	ComPtr<IDXGIAdapter3> adapter3;
	if (!m_Adapter || FAILED(m_Adapter.As(&adapter3)) || !adapter3)
	{
		return 0;
	}

	DXGI_QUERY_VIDEO_MEMORY_INFO info{};
	if (FAILED(adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info)))
	{
		return 0;
	}
	return info.CurrentUsage;
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

	// ---- Reset で失われる状態を復帰させる ----
	// ID3D12GraphicsCommandList::Reset はデスクリプタヒープ / ルート
	// シグネチャ / ビューポートをクリアする。BeginFrame 前の初期化時に
	// しか呼ばれない想定だが、実行時のアセットロード (FBXModel::Load の
	// SDF ベイク / BLAS ビルド) から呼ばれるとフレーム途中で状態が
	// 失われ、以降の SetConstant / SetTexture が
	// 「デスクリプタヒープが設定されていない」エラーになる。
	SetDefaultGraphicsState();
}
