#include "Main.h"
#include "RenderManager.h"
#include "SceneRenderer.h"
#include "Scene.h"
#include "SceneView.h"
#include "PrimitiveSceneProxy.h"
#include "LightSceneProxy.h"
#include "ShadowRendering.h"
#include "LightGridInjection.h"
#include "LumenScene.h"
#include "IBLBaker.h"
#include "AutoExposure.h"
#include "ColorGradingLUTBaker.h"
#include "Time.h"

#include "D3DX12.h"

#include "ImGUI/imgui.h"
#include "ImGUI/imgui_impl_win32.h"
#include "ImGUI/imgui_impl_dx12.h"


// ============================================================
//  Lifetime
// ============================================================
FSceneRenderer::~FSceneRenderer() = default;


FSceneRenderer::FSceneRenderer(RenderManager* RHI)
	: m_RHI(RHI)
{
	m_SceneTextures.Init(m_RHI);

	InitScreenQuad();
	InitIBL();
	InitLightBuffer();
	InitPostProcess();

	// タイルドライトカリング (Injection -> Compact の 2 コンピュートパス)
	m_LightGrid = std::make_unique<FLightGridInjection>(m_RHI);
	m_LightGrid->Init();

	// シャドウマップ描画系 (CSM + ローカルシャドウアトラス)
	m_ShadowRenderer = std::make_unique<FShadowSceneRenderer>(m_RHI);

	// Lumen Surface Cache (カード + アトラス + ライティングコンピュート)
	m_LumenScene = std::make_unique<FLumenSceneData>(m_RHI);
	m_LumenScene->Init();
}


// ============================================================
//  Initialization
// ============================================================
void FSceneRenderer::InitScreenQuad()
{
	m_ScreenQuad = m_RHI->CreateVertexBuffer(sizeof(VERTEX_3D), 4);

	VERTEX_3D* buffer{};
	HRESULT hr = m_ScreenQuad->Resource->Map(0, nullptr, (void**)&buffer);
	assert(SUCCEEDED(hr));

	buffer[0].Position = { -1.0f,  1.0f, 0.0f };
	buffer[1].Position = { 1.0f,  1.0f, 0.0f };
	buffer[2].Position = { -1.0f, -1.0f, 0.0f };
	buffer[3].Position = { 1.0f, -1.0f, 0.0f };

	buffer[0].Color = { 1.0f, 1.0f, 1.0f, 1.0f };
	buffer[1].Color = { 1.0f, 1.0f, 1.0f, 1.0f };
	buffer[2].Color = { 1.0f, 1.0f, 1.0f, 1.0f };
	buffer[3].Color = { 1.0f, 1.0f, 1.0f, 1.0f };

	buffer[0].Normal = { 0.0f, 1.0f, 0.0f };
	buffer[1].Normal = { 0.0f, 1.0f, 0.0f };
	buffer[2].Normal = { 0.0f, 1.0f, 0.0f };
	buffer[3].Normal = { 0.0f, 1.0f, 0.0f };

	buffer[0].TexCoord = { 0.0f, 0.0f };
	buffer[1].TexCoord = { 1.0f, 0.0f };
	buffer[2].TexCoord = { 0.0f, 1.0f };
	buffer[3].TexCoord = { 1.0f, 1.0f };

	m_ScreenQuad->Resource->Unmap(0, nullptr);
}


void FSceneRenderer::InitIBL()
{
	m_EnvironmentTexture = m_RHI->LoadTexture("Asset/Texture/kloppenheim_06_puresky_4k.dds", true);

	// IBL の事前計算は IBLBaker に委譲
	m_IBLBaker = std::make_unique<IBLBaker>(m_RHI);
	m_IBLBaker->Init();
	m_IBLBaker->Bake(m_EnvironmentTexture->Resource.Get(), m_EnvironmentTexture->SRVIndex);
}


void FSceneRenderer::InitPostProcess()
{
	// フル解像度テクセルを既定値としてセット (ボリューム未登録でも動くように)
	SetTexelSize(m_RHI->GetBackBufferWidth(), m_RHI->GetBackBufferHeight());

	// Create + bake the color grading LUT.
	m_ColorGradingLUTBaker = std::make_unique<ColorGradingLUTBaker>(m_RHI);
	m_ColorGradingLUTBaker->Init();

	// Auto exposure (eye adaptation). Independent compute passes run
	// every frame inside RenderPostProcessing; just create + init here.
	m_AutoExposure = std::make_unique<AutoExposure>(m_RHI);
	m_AutoExposure->Init();

	InitBloom();
	InitDOF();
}


void FSceneRenderer::InitBloom()
{
	// Build a half-res bloom mip chain. Each mip is its own RT (RTV+SRV).
	// m_BloomMip[i]  : downsample chain  (mip0 = bright-pass at half res)
	// m_BloomUp[i]   : upsample accumulators (m_BloomUp[0] = full bloom)
	int w = m_RHI->GetBackBufferWidth() / 2;
	int h = m_RHI->GetBackBufferHeight() / 2;

	for (int i = 0; i < BLOOM_MIPS; ++i)
	{
		w = (w < 1) ? 1 : w;
		h = (h < 1) ? 1 : h;
		m_BloomMipW[i] = w;
		m_BloomMipH[i] = h;

		m_BloomMip[i] = m_RHI->CreateRenderTarget(w, h, DXGI_FORMAT_R16G16B16A16_FLOAT);
		m_BloomMip[i]->Resource->SetName(L"BloomMipDown");

		m_BloomUp[i] = m_RHI->CreateRenderTarget(w, h, DXGI_FORMAT_R16G16B16A16_FLOAT);
		m_BloomUp[i]->Resource->SetName(L"BloomMipUp");

		w /= 2;
		h /= 2;
	}
}


void FSceneRenderer::InitDOF()
{
	// ハーフ解像度で CoC + ブラーを行う (Gaussian 相当)。
	m_DOFWidth = (m_RHI->GetBackBufferWidth() + 1) / 2;
	m_DOFHeight = (m_RHI->GetBackBufferHeight() + 1) / 2;

	m_DOFPrep = m_RHI->CreateRenderTarget(m_DOFWidth, m_DOFHeight, DXGI_FORMAT_R16G16B16A16_FLOAT);
	m_DOFPrep->Resource->SetName(L"DOFPrep");
	m_DOFPing = m_RHI->CreateRenderTarget(m_DOFWidth, m_DOFHeight, DXGI_FORMAT_R16G16B16A16_FLOAT);
	m_DOFPing->Resource->SetName(L"DOFPing");
	m_DOFPong = m_RHI->CreateRenderTarget(m_DOFWidth, m_DOFHeight, DXGI_FORMAT_R16G16B16A16_FLOAT);
	m_DOFPong->Resource->SetName(L"DOFPong");
	m_DOFBlur = m_RHI->CreateRenderTarget(m_DOFWidth, m_DOFHeight, DXGI_FORMAT_R16G16B16A16_FLOAT);
	m_DOFBlur->Resource->SetName(L"DOFBlur");

	// フル解像度のシャープコピー (合成時の読み取り元)。
	m_DOFSharp = m_RHI->CreateRenderTarget(m_RHI->GetBackBufferWidth(), m_RHI->GetBackBufferHeight(), DXGI_FORMAT_R16G16B16A16_FLOAT);
	m_DOFSharp->Resource->SetName(L"DOFSharp");
}


void FSceneRenderer::InitLightBuffer()
{
	// ローカルライト (Point/Spot/Rect) 用の StructuredBuffer (t13)。
	// CPU から毎フレーム詰め直すのでアップロードヒープに永続 Map。
	// Present は「前フレームの完了」までしか待たない (2 フレーム
	// インフライト) ため、GPU 読み取り中の上書きを避けるべく
	// ダブルバッファにする — 定数リングと同じ理由。
	ID3D12Device* device = m_RHI->GetDevice();

	const UINT64 bufferSize = sizeof(FLightShaderParameters) * MAX_LOCAL_LIGHTS;

	D3D12_HEAP_PROPERTIES heapProperties{};
	heapProperties.Type = D3D12_HEAP_TYPE_UPLOAD;

	D3D12_RESOURCE_DESC resourceDesc{};
	resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	resourceDesc.Width = bufferSize;
	resourceDesc.Height = 1;
	resourceDesc.DepthOrArraySize = 1;
	resourceDesc.MipLevels = 1;
	resourceDesc.Format = DXGI_FORMAT_UNKNOWN;
	resourceDesc.SampleDesc.Count = 1;
	resourceDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

	for (int i = 0; i < 2; ++i)
	{
		HRESULT hr = device->CreateCommittedResource(
			&heapProperties,
			D3D12_HEAP_FLAG_NONE,
			&resourceDesc,
			D3D12_RESOURCE_STATE_GENERIC_READ,	// アップロードヒープの固定ステート
			nullptr,
			IID_PPV_ARGS(&m_LightBuffer[i]));
		assert(SUCCEEDED(hr));
		m_LightBuffer[i]->SetName(L"SceneLightBuffer");

		hr = m_LightBuffer[i]->Map(0, nullptr, (void**)&m_LightBufferPointer[i]);
		assert(SUCCEEDED(hr));
		memset(m_LightBufferPointer[i], 0, bufferSize);

		// StructuredBuffer SRV (Format = UNKNOWN + StructureByteStride)
		m_LightBufferSRVIndex[i] = m_RHI->AllocateDescriptor();

		D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
		srvDesc.Format = DXGI_FORMAT_UNKNOWN;
		srvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
		srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		srvDesc.Buffer.FirstElement = 0;
		srvDesc.Buffer.NumElements = MAX_LOCAL_LIGHTS;
		srvDesc.Buffer.StructureByteStride = sizeof(FLightShaderParameters);
		srvDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;

		device->CreateShaderResourceView(
			m_LightBuffer[i].Get(), &srvDesc,
			m_RHI->GetCPUDescriptorHandle(m_LightBufferSRVIndex[i]));
	}
}


// ============================================================
//  Lights: FScene のライトリスト -> VIEW 定数 (directional) +
//  FORWARD_LIGHT 定数 + ライトバッファ (local, t13)
//  (FSceneRenderer::GatherLightsAndComputeLightGrid の簡易版)
// ============================================================
void FSceneRenderer::SetupLightConstants(FScene* Scene)
{
	// 書き込み先をフリップ (GPU が読んでいる前フレーム分を避ける)
	m_LightBufferFrame ^= 1;

	// ---- 既定値: ディレクショナルライト不在 = 無光  ----
	// 方向は normalize(0) の NaN を避けるため上向きを入れておく
	m_ViewConstant.DirectionalLightDirection = { 0.0f, 1.0f, 0.0f, 0.0f };
	m_ViewConstant.DirectionalLightColor = { 0.0f, 0.0f, 0.0f, 1.0f };

	unsigned int numLocalLights = 0;
	bool foundDirectional = false;

	// シャドウ構築 (RenderShadowDepths) 用のプロキシ列を集め直す。
	// m_FrameLocalLights はライトバッファ (t13) と必ず同順になる。
	m_FrameDirectionalLight = nullptr;
	m_FrameLocalLights.clear();

	if (Scene)
	{
		for (const FLightSceneInfo& info : Scene->GetLights())
		{
			const FLightSceneProxy* proxy = info.Proxy.get();
			if (proxy == nullptr || !proxy->AffectsWorld())
			{
				continue;
			}

			if (proxy->GetLightType() == ELightType::Directional)
			{
				// 最初の 1 灯のみ採用 (太陽ライトと同じ扱い)
				if (!foundDirectional)
				{
					foundDirectional = true;
					const XMFLOAT3& direction = proxy->GetDirection();
					const XMFLOAT3& color = proxy->GetColor();

					// DirectionalLightDirection は「受光面 -> ライト」規約なので
					// プロキシの発光方向を反転して渡す (View と同じ)
					m_ViewConstant.DirectionalLightDirection = { -direction.x, -direction.y, -direction.z, 0.0f };
					m_ViewConstant.DirectionalLightColor = { color.x, color.y, color.z, 1.0f };

					m_FrameDirectionalLight = proxy;	// CSM の対象ライト
				}
			}
			else if (numLocalLights < MAX_LOCAL_LIGHTS)
			{
				proxy->GetLightShaderParameters(
					m_LightBufferPointer[m_LightBufferFrame][numLocalLights]);
				m_FrameLocalLights.push_back(proxy);	// t16 との 1:1 対応用
				numLocalLights++;
			}
		}
	}

	m_ForwardLightConstant.NumLocalLights = numLocalLights;

	// ---- ライトグリッド (タイルドライトカリング) パラメータ ----
	// VIEW 定数は RenderBasePass 先頭で解決済みなので NearFar を参照できる。
	// グリッド本体の構築 (Dispatch) は RenderLighting 先頭で行う。
	if (m_LightGrid)
	{
		m_LightGrid->FillForwardLightData(
			m_ForwardLightConstant,
			m_ViewConstant.NearFar.x, m_ViewConstant.NearFar.y);
	}
}


// ============================================================
//  PostProcess constant (resolved settings -> b4)
// ============================================================
void FSceneRenderer::ResolvePostProcessSettings(const FSceneView& View)
{
	// ボリュームからの解決 (FFinalPostProcessSettings 相当) はゲーム側
	// (UWorld::CalcSceneView) で済んでいる。ここではレンダラ専有コピーへ
	// 受け取るだけ (パス内の一時変更をボリュームへ書き戻さないため)。
	m_FinalSettings = View.FinalPostProcessSettings;

	// テクセルサイズはレンダラ管轄 (フル解像度で開始)
	SetTexelSize(m_RHI->GetBackBufferWidth(), m_RHI->GetBackBufferHeight());
}


void FSceneRenderer::UploadPostProcessConstant()
{
	m_RHI->SetConstant(RenderManager::CONSTANT_TYPE::POST_PROCESS, &m_FinalSettings, sizeof(m_FinalSettings));
}


void FSceneRenderer::SetTexelSize(int Width, int Height)
{
	m_FinalSettings.SceneTexelSizeX = 1.0f / (float)Width;
	m_FinalSettings.SceneTexelSizeY = 1.0f / (float)Height;
}


// ============================================================
//  Frame begin: RHI prep -> G-Buffer open (base pass setup)
// ============================================================
void FSceneRenderer::BeginFrame()
{
	// ヒープ / ルートシグネチャ / 定数リング / ビューポート
	m_RHI->BeginFrame();

	ID3D12GraphicsCommandList* cl = m_RHI->GetGraphicsCommandList();

	// G-Buffer: SRV -> RENDER_TARGET
	for (auto* buf : m_SceneTextures.GBuffers)
	{
		cl->ResourceBarrier(1,
			&CD3DX12_RESOURCE_BARRIER::Transition(
				buf->Resource.Get(),
				D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
				D3D12_RESOURCE_STATE_RENDER_TARGET));
	}

	// G-Buffer をレンダーターゲットに設定
	assert(!m_SceneTextures.GBuffers.empty() && "GBuffers is empty");
	std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> renderTargets;
	renderTargets.reserve(m_SceneTextures.GBuffers.size());
	for (const auto* buf : m_SceneTextures.GBuffers)
	{
		assert(buf != nullptr);
		renderTargets.push_back(buf->RTVHandle);
	}

	D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle = m_RHI->GetDepthStencilViewHandle();

	cl->OMSetRenderTargets(
		static_cast<UINT>(renderTargets.size()),
		renderTargets.data(),
		FALSE,
		&dsvHandle);

	// クリア
	const FLOAT clearColor[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
	for (auto* buf : m_SceneTextures.GBuffers)
		cl->ClearRenderTargetView(buf->RTVHandle, clearColor, 0, nullptr);

	cl->ClearDepthStencilView(dsvHandle, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

	// ImGui フレーム開始
	{
		RECT rect;
		GetClientRect(GetWindow(), &rect);

		ImGui_ImplDX12_NewFrame();
		ImGui_ImplWin32_NewFrame(
			(float)(rect.right - rect.left) / m_RHI->GetBackBufferWidth(),
			(float)(rect.bottom - rect.top) / m_RHI->GetBackBufferHeight());
		ImGui::NewFrame();
	}

	m_RHI->SetPipelineState("BasePass");
}


// ============================================================
//  View visibility: フラスタム + 距離カリング
//  (FSceneRenderer::ComputeViewVisibility / SceneVisibility.cpp の
//   FrustumCull 相当)
//  カメラの ViewProjection から FConvexVolume を構築し、FScene の
//  プロキシ列を 距離 (Min/MaxDrawDistance) -> 球 -> ボックス の順で
//  判定して m_PrimitiveVisibilityMap (登録順 1:1) に解決する。
//  結果はベースパスの描画ループが使う。シャドウ深度パスは
//  各シャドウビューのフラスタムで独立にカリングする。
// ============================================================
void FSceneRenderer::ComputeViewVisibility(FScene* Scene)
{
	// ---- フラスタム構築 (GetViewFrustumBounds) ----
	// VIEW 定数は転置済みで保持しているため転置を戻してから合成する
	XMMATRIX view = XMMatrixTranspose(XMLoadFloat4x4(&m_ViewConstant.View));
	XMMATRIX projection = XMMatrixTranspose(XMLoadFloat4x4(&m_ViewConstant.Projection));
	GetViewFrustumBounds(m_ViewFrustum, view * projection, true, true);

	XMFLOAT3 viewOrigin = {
		m_ViewConstant.WorldCameraOrigin.x,
		m_ViewConstant.WorldCameraOrigin.y,
		m_ViewConstant.WorldCameraOrigin.z };

	// ---- r.FreezeRendering 相当: フラスタム凍結 ----
	// チェック ON の瞬間のフラスタム / 視点を保持し続けることで、
	// カメラを動かしてカリング済みプリミティブの消え方を観察できる
	if (m_CullingParams.bFreezeFrustum)
	{
		if (!m_bHasFrozenView)
		{
			m_FrozenViewFrustum = m_ViewFrustum;
			m_FrozenViewOrigin = viewOrigin;
			m_bHasFrozenView = true;
		}
	}
	else
	{
		m_bHasFrozenView = false;
	}

	const FConvexVolume& frustum = m_bHasFrozenView ? m_FrozenViewFrustum : m_ViewFrustum;
	const XMFLOAT3& cullOrigin = m_bHasFrozenView ? m_FrozenViewOrigin : viewOrigin;

	// ---- プリミティブ巡回 (PrimitiveCull) ----
	const std::vector<FPrimitiveSceneInfo>& primitives = Scene->GetPrimitives();
	m_PrimitiveVisibilityMap.assign(primitives.size(), 0);

	m_CullingStats = FCullingStats{};

	for (size_t i = 0; i < primitives.size(); ++i)
	{
		const FPrimitiveSceneProxy* proxy = primitives[i].Proxy.get();
		if (proxy == nullptr)
		{
			continue;
		}

		m_CullingStats.NumProcessed++;

		// ゲーム側の可視フラグ (SetVisibility)
		if (!proxy->IsVisible())
		{
			continue;
		}

		if (m_CullingParams.bEnableFrustumCulling)
		{
			const FBoxSphereBounds& bounds = proxy->GetBounds();

			// ---- 距離カリング (Min/MaxDrawDistance。0 = 無制限) ----
			// 境界中心とカメラ位置の距離で判定する
			const float dx = bounds.Origin.x - cullOrigin.x;
			const float dy = bounds.Origin.y - cullOrigin.y;
			const float dz = bounds.Origin.z - cullOrigin.z;
			const float distanceSq = dx * dx + dy * dy + dz * dz;

			const float maxDistance = proxy->GetMaxDrawDistance();
			const float minDistance = proxy->GetMinDrawDistance();

			if ((maxDistance > 0.0f && distanceSq > maxDistance * maxDistance) ||
				(minDistance > 0.0f && distanceSq < minDistance * minDistance))
			{
				m_CullingStats.NumDistanceCulled++;
				continue;
			}

			// ---- フラスタムカリング (球 -> ボックスの 2 段判定) ----
			if (!frustum.IntersectBounds(bounds))
			{
				m_CullingStats.NumFrustumCulled++;
				continue;
			}
		}

		m_PrimitiveVisibilityMap[i] = 1;
		m_CullingStats.NumVisible++;
	}
}


// ============================================================
//  Base pass: view/env constants + scene primitives -> G-Buffer
// ============================================================
void FSceneRenderer::RenderBasePass(FScene* Scene, const FSceneView& View)
{
	if (Scene == nullptr) return;

	// ---- VIEW 定数 (b0: カメラ + 代表ディレクショナルライト) ----
	// FViewUniformShaderParameters と同様、FSceneView (ゲーム側で
	// スナップショット済みのカメラ情報) とシーンの太陽ライトを
	// 1 つの View ユニフォームに解決する。カメラ不在
	// (View.bValid = false) のフレームは前回の VIEW 定数を保持する
	// (従来のカメラ不在時挙動と同じ)。
	if (View.bValid)
	{
		XMMATRIX view = XMLoadFloat4x4(&View.ViewMatrix);
		XMMATRIX projection = XMLoadFloat4x4(&View.ProjectionMatrix);

		XMMATRIX viewProjection = view * projection;
		XMMATRIX invViewProjection = XMMatrixInverse(nullptr, viewProjection);

		XMStoreFloat4x4(&m_ViewConstant.View, XMMatrixTranspose(view));
		XMStoreFloat4x4(&m_ViewConstant.Projection, XMMatrixTranspose(projection));
		XMStoreFloat4x4(&m_ViewConstant.InvViewProjection, XMMatrixTranspose(invViewProjection));

		m_ViewConstant.WorldCameraOrigin = { View.ViewOrigin.x, View.ViewOrigin.y, View.ViewOrigin.z, 1.0f };
		m_ViewConstant.NearFar = { View.NearClip, View.FarClip, 0.0f, 0.0f };
	}

	// FScene のライトリストを VIEW 定数 (directional) /
	// FORWARD_LIGHT 定数 / ライトバッファ (local, t13) へ解決する
	SetupLightConstants(Scene);

	m_RHI->SetConstant(RenderManager::CONSTANT_TYPE::VIEW, &m_ViewConstant, sizeof(m_ViewConstant));
	m_RHI->SetConstant(RenderManager::CONSTANT_TYPE::FORWARD_LIGHT, &m_ForwardLightConstant, sizeof(m_ForwardLightConstant));

	// ---- POST_PROCESS 定数 (b4: ゲーム側で解決済みの設定) ----
	ResolvePostProcessSettings(View);
	UploadPostProcessConstant();

	// ---- ビュー可視性の解決 (ComputeViewVisibility) ----
	// フラスタム + 距離カリングの結果を m_PrimitiveVisibilityMap に詰める
	ComputeViewVisibility(Scene);

	// ---- 可視プリミティブ描画 (登録順 = スポーン順) ----
	// レンダラはプロキシ (レンダー側スナップショット) だけを読む。
	// ゲーム側の UPrimitiveComponent にはここからは一切触れない。
	// m_PrimitiveVisibilityMap は登録順 1:1 (可視フラグ + カリング済み)。
	// FPrimitiveViewRelevance で Opaque / Masked のみベースパスへ流す
	// (Translucent / Additive は RenderTranslucency が後→前ソートで描く)。
	const std::vector<FPrimitiveSceneInfo>& primitives = Scene->GetPrimitives();
	for (size_t i = 0; i < primitives.size(); ++i)
	{
		if (m_PrimitiveVisibilityMap[i] == 0)
			continue;

		const FPrimitiveViewRelevance relevance = primitives[i].Proxy->GetViewRelevance();
		if (!relevance.HasOpaqueRelevance())
			continue;

		// プロキシ側でサブセットごとに PSO が差し替わる可能性がある
		// (BasePassTwoSided 等) ため、プリミティブごとに既定へ戻す
		m_RHI->SetPipelineState("BasePass");

		primitives[i].Proxy->DrawPrimitive(m_RHI);
	}
}


// ============================================================
//  Shadow depths: CSM + ローカルシャドウ -> シャドウマップ
//  (FSceneRenderer::InitDynamicShadows + RenderShadowDepthMaps)
//  RenderBasePass の SetupLightConstants が集めたプロキシ列を使う。
//  ※ 各シャドウビューが VIEW 定数 (b0) を上書きするため、
//    RenderLighting 先頭でカメラの VIEW 定数を積み直す。
// ============================================================
void FSceneRenderer::RenderShadowDepths(FScene* Scene, const FSceneView& View)
{
	if (Scene == nullptr || m_ShadowRenderer == nullptr) return;

	// カリング制御をシャドウ側へ伝搬 (デバッグ時に一括で無効化できる)
	m_ShadowRenderer->SetFrustumCullingEnabled(m_CullingParams.bEnableFrustumCulling);

	// シャドウビュー構築 (CSM カスケード + ローカルスライス割当) と
	// GPU パラメータ (b5 / t16) の更新。CSM のフィッティングは
	// FSceneView のカメラスナップショットを使う (View.bValid = false
	// のフレームは CSM をスキップ = 従来のカメラ不在時挙動)。
	m_ShadowRenderer->InitDynamicShadows(
		m_FrameDirectionalLight, m_FrameLocalLights, View);

	// Distance Field オブジェクトバッファ (t18) を今フレームの
	// プロキシ列から詰め直す (DistanceFieldObjectBuffers 更新相当)
	m_ShadowRenderer->UpdateDistanceFieldObjects(Scene);

	// シャドウ深度パス (FScene のプリミティブプロキシ列を巡回)
	m_ShadowRenderer->RenderShadowDepthMaps(Scene);
}


// ============================================================
//  Lumen: シーン更新 -> カードキャプチャ -> Surface Cache ライティング
//  (UpdateLumenScene / RenderLumenSceneLighting 相当)。
//  キャプチャが b0 (カードビュー) / b1 (単位行列) を上書きするため
//  RenderShadowDepths の後 / RenderLighting の前に呼ぶこと。
//  Surface Cache の FinalLighting には Emissive が合成され、
//  デファードパスのスクリーン GI が「発光面の光」として採光する。
// ============================================================
void FSceneRenderer::RenderLumenScene(FScene* Scene)
{
	if (m_LumenScene == nullptr)
	{
		return;
	}

	// プロキシ列 -> スロット同期 + オブジェクト / カードバッファ更新
	// (HWRT 有効時は TLAS 再構築も記録される)
	m_LumenScene->UpdateLumenScene(Scene);

	// カードキャプチャ (フレーム予算制)
	m_LumenScene->RenderCardCaptures();

	// Global SDF 再構築 -> Direct -> Radiosity -> Combine -> Radiance
	// Cache (Emissive は Combine で光源化される)。ライト情報は
	// SetupLightConstants が解決済みの VIEW 定数の値をそのまま使う。
	m_LumenScene->RenderLumenSceneLighting(MakeLumenFrameInputs());
}


// ============================================================
//  MakeLumenFrameInputs
//  今フレームの解決済みビュー / ライト / シーンテクスチャを
//  Lumen へ渡す入力へまとめる (Lumen はゲーム側に触れない)。
// ============================================================
FLumenFrameInputs FSceneRenderer::MakeLumenFrameInputs() const
{
	FLumenFrameInputs inputs;

	inputs.DirectionalLightDirection = m_ViewConstant.DirectionalLightDirection;
	inputs.DirectionalLightColor = m_ViewConstant.DirectionalLightColor;
	inputs.LightBufferSRVIndex = m_LightBufferSRVIndex[m_LightBufferFrame];
	inputs.NumLocalLights = m_ForwardLightConstant.NumLocalLights;

	if (m_IBLBaker)
	{
		inputs.IrradianceSRVIndex = m_IBLBaker->GetIrradianceSRVIndex();
		inputs.PrefilterSRVIndex = m_IBLBaker->GetPrefilterSRVIndex();
	}

	inputs.CameraOrigin = m_ViewConstant.WorldCameraOrigin;

	// (V x P)^T = P^T x V^T (格納は転置済み)
	const XMMATRIX viewT = XMLoadFloat4x4(&m_ViewConstant.View);
	const XMMATRIX projT = XMLoadFloat4x4(&m_ViewConstant.Projection);
	XMStoreFloat4x4(&inputs.ViewProjectionT, XMMatrixMultiply(projT, viewT));
	inputs.InvViewProjectionT = m_ViewConstant.InvViewProjection;

	inputs.ScreenWidth = (unsigned int)m_RHI->GetBackBufferWidth();
	inputs.ScreenHeight = (unsigned int)m_RHI->GetBackBufferHeight();

	inputs.PrevViewProjectionT = m_PrevViewProjectionT;
	inputs.PrevInvViewProjectionT = m_PrevInvViewProjectionT;
	inputs.PrevCameraOrigin = m_PrevViewOrigin;
	inputs.bHistoryValid = m_bHistoryValid;

	inputs.SceneDepthSRVIndex = m_SceneTextures.DepthSRVIndex;
	if (m_SceneTextures.LinearDepth) { inputs.LinearDepthSRVIndex = m_SceneTextures.LinearDepth->SRVIndex; }
	if (m_SceneTextures.GBufferA) { inputs.GBufferNormalSRVIndex = m_SceneTextures.GBufferA->SRVIndex; }
	if (m_SceneTextures.GBufferB) { inputs.GBufferBSRVIndex = m_SceneTextures.GBufferB->SRVIndex; }
	if (m_SceneTextures.PrevSceneColor) { inputs.PrevSceneColorSRVIndex = m_SceneTextures.PrevSceneColor->SRVIndex; }
	if (m_SceneTextures.PrevLinearDepth) { inputs.PrevLinearDepthSRVIndex = m_SceneTextures.PrevLinearDepth->SRVIndex; }

	return inputs;
}


// ============================================================
//  CopySceneColorHistory
//  ライティング + 半透明合成後の線形 HDR SceneColor を
//  PrevSceneColor へ確定し、そのフレームのビュー行列を保存する。
//  RenderPostProcessing 先頭 (SceneColor が加工される前) に呼ぶ。
//  Lumen のスクリーンスペーストレース (スクリーンプローブ / 反射) が
//  次フレームにリプロジェクションで採光する。
// ============================================================
void FSceneRenderer::CopySceneColorHistory()
{
	if (m_SceneTextures.PrevSceneColor == nullptr)
	{
		return;
	}

	ID3D12GraphicsCommandList* cl = m_RHI->GetGraphicsCommandList();

	const D3D12_RESOURCE_STATES readState =
		D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

	// LinearDepth も同時に履歴化する (スクリーントレースの採光点が前フレームで
	// 可視だったかを検証するため。常在状態は (PIXEL | NON_PIXEL))
	const bool bCopyDepth =
		(m_SceneTextures.LinearDepth != nullptr) && (m_SceneTextures.PrevLinearDepth != nullptr);

	std::vector<D3D12_RESOURCE_BARRIER> toCopy =
	{
		CD3DX12_RESOURCE_BARRIER::Transition(
			m_SceneTextures.SceneColor->Resource.Get(),
			D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
			D3D12_RESOURCE_STATE_COPY_SOURCE),
		CD3DX12_RESOURCE_BARRIER::Transition(
			m_SceneTextures.PrevSceneColor->Resource.Get(),
			readState,
			D3D12_RESOURCE_STATE_COPY_DEST),
	};
	if (bCopyDepth)
	{
		toCopy.push_back(CD3DX12_RESOURCE_BARRIER::Transition(
			m_SceneTextures.LinearDepth->Resource.Get(),
			readState,
			D3D12_RESOURCE_STATE_COPY_SOURCE));
		toCopy.push_back(CD3DX12_RESOURCE_BARRIER::Transition(
			m_SceneTextures.PrevLinearDepth->Resource.Get(),
			readState,
			D3D12_RESOURCE_STATE_COPY_DEST));
	}
	cl->ResourceBarrier((UINT)toCopy.size(), toCopy.data());

	cl->CopyResource(
		m_SceneTextures.PrevSceneColor->Resource.Get(),
		m_SceneTextures.SceneColor->Resource.Get());
	if (bCopyDepth)
	{
		cl->CopyResource(
			m_SceneTextures.PrevLinearDepth->Resource.Get(),
			m_SceneTextures.LinearDepth->Resource.Get());
	}

	std::vector<D3D12_RESOURCE_BARRIER> fromCopy =
	{
		CD3DX12_RESOURCE_BARRIER::Transition(
			m_SceneTextures.SceneColor->Resource.Get(),
			D3D12_RESOURCE_STATE_COPY_SOURCE,
			D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
		CD3DX12_RESOURCE_BARRIER::Transition(
			m_SceneTextures.PrevSceneColor->Resource.Get(),
			D3D12_RESOURCE_STATE_COPY_DEST,
			readState),
	};
	if (bCopyDepth)
	{
		fromCopy.push_back(CD3DX12_RESOURCE_BARRIER::Transition(
			m_SceneTextures.LinearDepth->Resource.Get(),
			D3D12_RESOURCE_STATE_COPY_SOURCE,
			readState));
		fromCopy.push_back(CD3DX12_RESOURCE_BARRIER::Transition(
			m_SceneTextures.PrevLinearDepth->Resource.Get(),
			D3D12_RESOURCE_STATE_COPY_DEST,
			readState));
	}
	cl->ResourceBarrier((UINT)fromCopy.size(), fromCopy.data());

	// ---- 前フレーム行列 / カメラ位置の確定 ----
	// 格納は転置済み: (V x P)^T = P^T x V^T (XMMatrixMultiply(A, B) = A x B)
	const XMMATRIX viewT = XMLoadFloat4x4(&m_ViewConstant.View);
	const XMMATRIX projT = XMLoadFloat4x4(&m_ViewConstant.Projection);
	XMStoreFloat4x4(&m_PrevViewProjectionT, XMMatrixMultiply(projT, viewT));
	m_PrevInvViewProjectionT = m_ViewConstant.InvViewProjection;
	m_PrevViewOrigin = m_ViewConstant.WorldCameraOrigin;
	m_bHistoryValid = true;
}


// ============================================================
//  Lighting: LinearDepth -> Deferred lighting -> HDR SceneColor
// ============================================================
void FSceneRenderer::RenderLighting()
{
	ID3D12GraphicsCommandList* cl = m_RHI->GetGraphicsCommandList();

	// シャドウ深度パスが VIEW 定数 (b0) をライトビューで上書きして
	// いるため、カメラの VIEW 定数を積み直す
	// (LinearDepth の NearFar / デファードの行列参照)
	m_RHI->SetConstant(RenderManager::CONSTANT_TYPE::VIEW, &m_ViewConstant, sizeof(m_ViewConstant));

	//======================================================
	// Light Grid : Injection -> Compact (タイルドライトカリング)
	//  SetupLightConstants が今フレーム分を詰めたライトバッファを
	//  セルごとにカリングする。デファードパスが t19/t20 で読む。
	//  (コンピュート専用の独立ルートシグネチャを使うため、
	//   グラフィックス側のバインドには影響しない)
	//======================================================
	if (m_LightGrid)
	{
		m_LightGrid->Dispatch(
			m_ViewConstant,
			m_LightBufferSRVIndex[m_LightBufferFrame],
			m_ForwardLightConstant.NumLocalLights);
	}

	// G-Buffer: RENDER_TARGET -> 読み取り (PIXEL | NON_PIXEL)。
	// デファードパス (ピクセル) に加え、Lumen のスクリーンプローブ /
	// 反射コンピュートパスも法線 / ラフネスを読むため複合状態にする。
	for (auto* buf : m_SceneTextures.GBuffers)
	{
		cl->ResourceBarrier(1,
			&CD3DX12_RESOURCE_BARRIER::Transition(
				buf->Resource.Get(),
				D3D12_RESOURCE_STATE_RENDER_TARGET,
				D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
	}

	// 深度バッファ: DEPTH_WRITE -> 読み取り (PIXEL | NON_PIXEL)
	cl->ResourceBarrier(1,
		&CD3DX12_RESOURCE_BARRIER::Transition(
			m_RHI->GetDepthBufferResource(),
			D3D12_RESOURCE_STATE_DEPTH_WRITE,
			D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));

	//======================================================
	// LinearDepth Pass : 深度 SRV を読んで線形深度バッファに書き出す
	//======================================================
	{
		cl->ResourceBarrier(1,
			&CD3DX12_RESOURCE_BARRIER::Transition(
				m_SceneTextures.LinearDepth->Resource.Get(),
				D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
				D3D12_RESOURCE_STATE_RENDER_TARGET));

		cl->OMSetRenderTargets(1, &m_SceneTextures.LinearDepth->RTVHandle, TRUE, nullptr);

		const FLOAT linearClear[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
		cl->ClearRenderTargetView(m_SceneTextures.LinearDepth->RTVHandle, linearClear, 0, nullptr);

		m_RHI->SetPipelineState("LinearDepth");
		m_RHI->BindRootTableBySRVIndex(
			(unsigned int)RenderManager::TEXTURE_TYPE::DEPTH, m_SceneTextures.DepthSRVIndex); // t3

		DrawScreenPass();

		// RENDER_TARGET -> 読み取り (PIXEL | NON_PIXEL)。
		// 以降のパス / ImGui に加え、Lumen スクリーンプローブの
		// スクリーンスペーストレース (コンピュート) も読む。
		cl->ResourceBarrier(1,
			&CD3DX12_RESOURCE_BARRIER::Transition(
				m_SceneTextures.LinearDepth->Resource.Get(),
				D3D12_RESOURCE_STATE_RENDER_TARGET,
				D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
	}

	//======================================================
	// Lumen スクリーン GI (Screen Probe Gather + Reflections)
	//  G-Buffer / 深度 / LinearDepth が読み取り状態になり、
	//  Surface Cache の FinalLighting が確定したこの時点で記録する。
	//  結果はデファードパスが t28 (DiffuseIndirect) / t29
	//  (Reflections) で読む。
	//======================================================
	if (m_LumenScene)
	{
		m_LumenScene->RenderLumenScreenGI(MakeLumenFrameInputs());
	}

	//======================================================
	// Deferred Lighting Pass -> HDR SceneColor (linear, unclamped)
	//======================================================
	cl->ResourceBarrier(1,
		&CD3DX12_RESOURCE_BARRIER::Transition(
			m_SceneTextures.SceneColor->Resource.Get(),
			D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
			D3D12_RESOURCE_STATE_RENDER_TARGET));

	cl->OMSetRenderTargets(1, &m_SceneTextures.SceneColor->RTVHandle, TRUE, nullptr);

	const FLOAT sceneClear[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
	cl->ClearRenderTargetView(m_SceneTextures.SceneColor->RTVHandle, sceneClear, 0, nullptr);

	{
		m_RHI->SetPipelineState("DeferredLighting");
		// G-Buffer (t0/t1/t2)。ワールド座標は深度から再構築するため
		// 専用バッファのバインドは無い。
		m_RHI->SetTexture(RenderManager::TEXTURE_TYPE::BASE_COLOR, m_SceneTextures.GBufferC.get());
		m_RHI->SetTexture(RenderManager::TEXTURE_TYPE::NORMAL, m_SceneTextures.GBufferA.get());
		m_RHI->SetTexture(RenderManager::TEXTURE_TYPE::MSRA, m_SceneTextures.GBufferB.get());

		// Substrate Slab パックデータ (t22/t23)。GBuffers vector 経由で
		// SRV 遷移済み。ヘッダ (x) = 0 のピクセルはレガシー経路へ分岐する
		// (DeferredPS.hlsl)。
		m_RHI->SetTexture(RenderManager::TEXTURE_TYPE::SUBSTRATE_MATERIAL0, m_SceneTextures.SubstrateMaterial0.get());
		m_RHI->SetTexture(RenderManager::TEXTURE_TYPE::SUBSTRATE_MATERIAL1, m_SceneTextures.SubstrateMaterial1.get());

		m_RHI->BindRootTableBySRVIndex(
			(unsigned int)RenderManager::TEXTURE_TYPE::DEPTH, m_SceneTextures.DepthSRVIndex);

		m_RHI->SetTexture(RenderManager::TEXTURE_TYPE::LINEAR_DEPTH, m_SceneTextures.LinearDepth.get());
		m_RHI->SetTexture(RenderManager::TEXTURE_TYPE::ENVIRONMENT, m_EnvironmentTexture.get());

		// IBL precomputed (t6 irradiance / t7 prefilter / t8 brdfLUT)
		if (m_IBLBaker)
		{
			m_IBLBaker->BindTextures();
		}

		// ローカルライト (t13): 今フレーム分の StructuredBuffer
		m_RHI->BindRootTableBySRVIndex(
			(unsigned int)RenderManager::TEXTURE_TYPE::LIGHTS,
			m_LightBufferSRVIndex[m_LightBufferFrame]);

		// ライトグリッド (t19: セルヘッダ / t20: ライトインデックス列)
		if (m_LightGrid)
		{
			m_RHI->BindRootTableBySRVIndex(
				(unsigned int)RenderManager::TEXTURE_TYPE::NUM_CULLED_LIGHTS_GRID,
				m_LightGrid->GetNumCulledLightsGridSRVIndex());
			m_RHI->BindRootTableBySRVIndex(
				(unsigned int)RenderManager::TEXTURE_TYPE::CULLED_LIGHT_DATA_GRID,
				m_LightGrid->GetCulledLightDataGridSRVIndex());
		}

		// シャドウ (b5: CSM 定数 / t14: CSM / t15: ローカルアトラス /
		// t16: ローカルシャドウパラメータ)
		if (m_ShadowRenderer)
		{
			m_ShadowRenderer->BindShadowResources();
		}

		// Lumen スクリーン GI (b6: LUMEN 定数 / t24: オブジェクト /
		// t25: カード / t26: FinalLighting / t27: 深度アトラス)。
		// SDF アトラス (t17) はシャドウ側 BindShadowResources が
		// バインド済みのものを共用する。
		if (m_LumenScene)
		{
			LUMEN_CONSTANT lumenConstant{};
			m_LumenScene->FillLumenConstant(lumenConstant);
			m_RHI->SetConstant(RenderManager::CONSTANT_TYPE::LUMEN,
				&lumenConstant, sizeof(lumenConstant));

			m_LumenScene->BindLumenResources();
		}

		DrawScreenPass();
	}

	//======================================================
	// SceneColor: RENDER_TARGET -> SRV
	//======================================================
	cl->ResourceBarrier(1,
		&CD3DX12_RESOURCE_BARRIER::Transition(
			m_SceneTextures.SceneColor->Resource.Get(),
			D3D12_RESOURCE_STATE_RENDER_TARGET,
			D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE));
}



// ============================================================
//  RenderTranslucency
//  FDeferredShadingSceneRenderer::RenderTranslucency 相当。
//  デファードライティング済みの SceneColor に対し、Translucent /
//  Additive マテリアルのプリミティブをフォワードシェーディング
//  (TranslucentPS: デファードと同一のライト / シャドウ / IBL 入力)
//  で合成する。
//    - 深度は不透明結果に対するテストのみ (PSO: DepthRead、書き込みなし)
//    - 描画順は TranslucentSortPolicy::SortByDistance
//      (境界原点のカメラ距離で後→前)
//    - ブレンドは PSO 側: Translucent = SrcAlpha/InvSrcAlpha,
//      Additive = SrcAlpha/One (Two Sided はカリング無効バリアント)
//  トランスルーセントプリミティブが 1 つも無いフレームは
//  バリアも発行せず早期リターンする。
// ============================================================
void FSceneRenderer::RenderTranslucency(FScene* Scene)
{
	if (Scene == nullptr) return;

	// ---- 可視トランスルーセントプリミティブの収集 ----
	// ベースパスの ComputeViewVisibility の結果 (フラスタム + 距離
	// カリング済み) をそのまま使う。
	const std::vector<FPrimitiveSceneInfo>& primitives = Scene->GetPrimitives();

	struct FTranslucentSortEntry
	{
		size_t Index;
		int    SortPriority;	// TranslucencySortPriority (低い = 先に描く = 奥)
		float  SortKey;			// ポリシー別の奥行きキー (大きい = 先に描く = 奥)
	};
	std::vector<FTranslucentSortEntry> sortedPrims;
	sortedPrims.reserve(primitives.size());

	const XMFLOAT4& cameraOrigin = m_ViewConstant.WorldCameraOrigin;
	const FTranslucencyParams& transParams = m_TranslucencyParams;

	// SortByProjectedZ 用のビュー行列 (格納は転置なので戻して使う)
	const XMMATRIX viewMatrix = XMMatrixTranspose(XMLoadFloat4x4(&m_ViewConstant.View));

	for (size_t i = 0; i < primitives.size(); ++i)
	{
		if (i < m_PrimitiveVisibilityMap.size() && m_PrimitiveVisibilityMap[i] == 0)
			continue;

		const FPrimitiveSceneProxy* proxy = primitives[i].Proxy.get();
		if (proxy == nullptr)
			continue;

		if (!proxy->GetViewRelevance().HasTranslucency())
			continue;

		const XMFLOAT3& origin = proxy->GetBounds().Origin;

		// ---- ポリシー別の奥行きキー (ETranslucentSortPolicy) ----
		float sortKey = 0.0f;
		switch (transParams.SortPolicy)
		{
		case ETranslucentSortPolicy::SortByProjectedZ:
			// ビュー空間 Z: 視線方向の奥行き。地面のような大きな面と
			// 小物の前後が原点「距離」で逆転するケースに強い
			sortKey = XMVectorGetZ(XMVector3TransformCoord(XMLoadFloat3(&origin), viewMatrix));
			break;

		case ETranslucentSortPolicy::SortAlongAxis:
			// 固定軸への射影 (2D / 見下ろし向け)
			sortKey = origin.x * transParams.SortAxis.x
				+ origin.y * transParams.SortAxis.y
				+ origin.z * transParams.SortAxis.z;
			break;

		case ETranslucentSortPolicy::SortByDistance:
		default:
		{
			const float dx = origin.x - cameraOrigin.x;
			const float dy = origin.y - cameraOrigin.y;
			const float dz = origin.z - cameraOrigin.z;
			sortKey = dx * dx + dy * dy + dz * dz;
			break;
		}
		}

		sortedPrims.push_back({ i, proxy->GetTranslucencySortPriority(), sortKey });
	}

	if (sortedPrims.empty())
		return;

	// ---- ソート: 優先度昇順 -> 奥行きキー降順 (後→前) ----
	// UPrimitiveComponent::TranslucencySortPriority 仕様:
	// 「低い優先度は高い優先度の後ろに描かれ、同値内はバウンズ原点
	//  基準の後→前」。低優先度を先に描く = 奥に置く。
	std::sort(sortedPrims.begin(), sortedPrims.end(),
		[](const FTranslucentSortEntry& A, const FTranslucentSortEntry& B)
		{
			if (A.SortPriority != B.SortPriority)
				return A.SortPriority < B.SortPriority;
			return A.SortKey > B.SortKey;
		});

	ID3D12GraphicsCommandList* cl = m_RHI->GetGraphicsCommandList();

	// ---- 屈折用シーンカラーコピー (t21) ----
	// 確定済み SceneColor をコピーしてから RENDER_TARGET に遷移する。
	// 半透明 PS (RefractionCommon.hlsl) が屈折オフセット付きで読む。
	// トランスルーセントが無いフレームは早期リターン済みなので
	// コピーは半透明ありのフレームだけ発生する。
	{
		D3D12_RESOURCE_BARRIER toCopy[2] =
		{
			CD3DX12_RESOURCE_BARRIER::Transition(
				m_SceneTextures.SceneColor->Resource.Get(),
				D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
				D3D12_RESOURCE_STATE_COPY_SOURCE),
			CD3DX12_RESOURCE_BARRIER::Transition(
				m_SceneTextures.SceneColorCopy->Resource.Get(),
				D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
				D3D12_RESOURCE_STATE_COPY_DEST),
		};
		cl->ResourceBarrier(_countof(toCopy), toCopy);

		cl->CopyResource(
			m_SceneTextures.SceneColorCopy->Resource.Get(),
			m_SceneTextures.SceneColor->Resource.Get());

		// SceneColor は SRV 経由ではなく COPY_SOURCE から直接
		// RENDER_TARGET へ (デファード結果の上に合成する)
		D3D12_RESOURCE_BARRIER fromCopy[2] =
		{
			CD3DX12_RESOURCE_BARRIER::Transition(
				m_SceneTextures.SceneColor->Resource.Get(),
				D3D12_RESOURCE_STATE_COPY_SOURCE,
				D3D12_RESOURCE_STATE_RENDER_TARGET),
			CD3DX12_RESOURCE_BARRIER::Transition(
				m_SceneTextures.SceneColorCopy->Resource.Get(),
				D3D12_RESOURCE_STATE_COPY_DEST,
				D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
		};
		cl->ResourceBarrier(_countof(fromCopy), fromCopy);
	}

	// ---- 深度: SRV -> DEPTH_WRITE (DSV バインドのため) ----
	// 不透明の深度に対するテストのみ。PSO (DepthRead) 側で書き込みが
	// 無効化されているため深度値は変化しない。
	// ※ この間、TranslucentPS は深度 SRV (t3) を参照しないこと。
	//   LinearDepth (t4) は深度バッファとは別リソースで SRV のまま
	//   なので参照可 (屈折の深度棄却が使用する)。
	cl->ResourceBarrier(1,
		&CD3DX12_RESOURCE_BARRIER::Transition(
			m_RHI->GetDepthBufferResource(),
			D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
			D3D12_RESOURCE_STATE_DEPTH_WRITE));

	D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle = m_RHI->GetDepthStencilViewHandle();
	cl->OMSetRenderTargets(1, &m_SceneTextures.SceneColor->RTVHandle, TRUE, &dsvHandle);

	// ---- 定数 (b0 カメラ / b3 フォワードライト) を積み直す ----
	m_RHI->SetConstant(RenderManager::CONSTANT_TYPE::VIEW, &m_ViewConstant, sizeof(m_ViewConstant));
	m_RHI->SetConstant(RenderManager::CONSTANT_TYPE::FORWARD_LIGHT, &m_ForwardLightConstant, sizeof(m_ForwardLightConstant));

	// ---- Lumen (b6 + t30-t32): 半透明の Radiance Cache GI ----
	if (m_LumenScene)
	{
		LUMEN_CONSTANT lumenConstant{};
		m_LumenScene->FillLumenConstant(lumenConstant);
		m_RHI->SetConstant(RenderManager::CONSTANT_TYPE::LUMEN,
			&lumenConstant, sizeof(lumenConstant));

		m_LumenScene->BindTranslucencyResources();
	}

	// ---- b4 (PostProcess): 屈折 UV 計算が SceneTexelSize を参照 ----
	// フル解像度のテクセルサイズを持つ通常の内容をそのまま積む。
	UploadPostProcessConstant();

	// ---- 屈折入力: シーンカラーコピー (t21) + 線形深度 (t4) ----
	// t4 は深度棄却 (ResolveRefractedSceneUV) が読む。LinearDepth は
	// 深度バッファ (DSV) とは別リソースなので SRV のまま参照できる。
	m_RHI->SetTexture(RenderManager::TEXTURE_TYPE::SCENE_COLOR_COPY, m_SceneTextures.SceneColorCopy.get());
	m_RHI->SetTexture(RenderManager::TEXTURE_TYPE::LINEAR_DEPTH, m_SceneTextures.LinearDepth.get());

	// ---- フォワードライティング入力 (デファードパスと同一セット) ----
	// IBL precomputed (t6 irradiance / t7 prefilter / t8 brdfLUT)
	if (m_IBLBaker)
	{
		m_IBLBaker->BindTextures();
	}

	// ローカルライト (t13): 今フレーム分の StructuredBuffer
	m_RHI->BindRootTableBySRVIndex(
		(unsigned int)RenderManager::TEXTURE_TYPE::LIGHTS,
		m_LightBufferSRVIndex[m_LightBufferFrame]);

	// ライトグリッド (t19: セルヘッダ / t20: ライトインデックス列)
	if (m_LightGrid)
	{
		m_RHI->BindRootTableBySRVIndex(
			(unsigned int)RenderManager::TEXTURE_TYPE::NUM_CULLED_LIGHTS_GRID,
			m_LightGrid->GetNumCulledLightsGridSRVIndex());
		m_RHI->BindRootTableBySRVIndex(
			(unsigned int)RenderManager::TEXTURE_TYPE::CULLED_LIGHT_DATA_GRID,
			m_LightGrid->GetCulledLightDataGridSRVIndex());
	}

	// シャドウ (b5: CSM 定数 / t14: CSM / t15: ローカルアトラス /
	// t16: ローカルシャドウパラメータ / t17-t18: Distance Field)
	if (m_ShadowRenderer)
	{
		m_ShadowRenderer->BindShadowResources();
	}

	// ---- 描画 (後→前) ----
	// ---- 半透明深度プリパス (単層トランスルーセンシー) ----
	// プリミティブごとに「(1) カラー書き込み無効 + 深度書き込みで
	// 最前面深度を焼く -> (2) EQUAL 比較で最前面のみ着色」を
	// 後→前順にインターリーブする。プリミティブ内部の三角形順に
	// 依存した自己前後逆転が構造的に消え、プリミティブ間の合成は
	// 従来どおり後→前ブレンドで保たれる。PSO はプロキシ側が
	// マテリアル (Blend Mode / Two Sided / 描画モード) から選ぶ。
	// ※ 全プリミティブの深度を先に焼く方式は不可 (手前の
	//   プリミティブの深度が奥の EQUAL 着色を落とすため)。
	for (const FTranslucentSortEntry& entry : sortedPrims)
	{
		const FPrimitiveSceneProxy* proxy = primitives[entry.Index].Proxy.get();

		proxy->DrawTranslucency(m_RHI, FPrimitiveSceneProxy::ETranslucencyDrawMode::DepthPrepass);
		proxy->DrawTranslucency(m_RHI, FPrimitiveSceneProxy::ETranslucencyDrawMode::ColorEqual);
	}

	// ---- SceneColor: RENDER_TARGET -> SRV (ポストプロセスが読む) ----
	cl->ResourceBarrier(1,
		&CD3DX12_RESOURCE_BARRIER::Transition(
			m_SceneTextures.SceneColor->Resource.Get(),
			D3D12_RESOURCE_STATE_RENDER_TARGET,
			D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE));

	// ---- 深度: DEPTH_WRITE -> 読み取り (PIXEL | NON_PIXEL) に戻す ----
	// (RenderPostProcessing 末尾の 読み取り -> DEPTH_WRITE 遷移と対にする)
	cl->ResourceBarrier(1,
		&CD3DX12_RESOURCE_BARRIER::Transition(
			m_RHI->GetDepthBufferResource(),
			D3D12_RESOURCE_STATE_DEPTH_WRITE,
			D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
}


// ============================================================
//  Post processing: DOF -> AutoExposure -> Bloom -> LUT -> Tonemap
// ============================================================
void FSceneRenderer::RenderPostProcessing()
{
	ID3D12GraphicsCommandList* cl = m_RHI->GetGraphicsCommandList();

	//======================================================
	// Lumen 用シーンカラー履歴の確定 (SceneColor が加工される前)
	//======================================================
	CopySceneColorHistory();

	//======================================================
	// Depth of Field (Gaussian). SceneColor in/out SRV.
	//  Bloom の入力にもなるよう、AutoExposure/Bloom より前で合成する。
	//======================================================
	if (m_FinalSettings.Flags & PP_FLAG_DOF)
	{
		RenderDOF();
	}

	//======================================================
	// Auto Exposure: histogram + adapt.
	//======================================================
	if (m_AutoExposure)
	{
		m_AutoExposure->Dispatch(
			m_SceneTextures.SceneColor->Resource.Get(),
			m_SceneTextures.SceneColor->SRVIndex,
			(unsigned int)m_RHI->GetBackBufferWidth(),
			(unsigned int)m_RHI->GetBackBufferHeight(),
			Time::GetDeltaTime());
	}

	//======================================================
	// Bloom: bright-pass -> down/up chain (-> m_BloomUp[0])
	//======================================================
	if (m_FinalSettings.Flags & PP_FLAG_BLOOM)
	{
		RenderBloom();
	}

	// Re-bake the color grading LUT if any grading setting changed this
	// frame (compute dispatch; cheap no-op when unchanged).
	if (m_ColorGradingLUTBaker)
	{
		m_ColorGradingLUTBaker->UpdateIfDirty(m_FinalSettings);
	}

	// Restore the full ENV constant (full-res texel size) for the tonemap pass.
	UploadPostProcessConstant();

	//======================================================
	// Tonemap Pass -> SDR back buffer (post chain: CA, bloom,
	// exposure, white balance, ACES, grading, vignette, grain, sRGB)
	//======================================================

	// バックバッファ: PRESENT -> RENDER_TARGET
	// (旧 DrawEnd はここを逆方向 (RT->PRESENT) にしていた「要見直し」箇所。
	//  スワップチェーンバッファは PRESENT(COMMON) 開始なので、書き込み前に
	//  RENDER_TARGET へ遷移し、Present 直前に戻すのが正しい)
	cl->ResourceBarrier(1,
		&CD3DX12_RESOURCE_BARRIER::Transition(
			m_RHI->GetCurrentBackBufferResource(),
			D3D12_RESOURCE_STATE_PRESENT,
			D3D12_RESOURCE_STATE_RENDER_TARGET));

	D3D12_CPU_DESCRIPTOR_HANDLE backBufferRTV = m_RHI->GetCurrentBackBufferRTV();

	cl->OMSetRenderTargets(1, &backBufferRTV, TRUE, nullptr);

	const FLOAT clearColor[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
	cl->ClearRenderTargetView(backBufferRTV, clearColor, 0, nullptr);

	{
		m_RHI->SetPipelineState("PostProcessTonemap");
		m_RHI->SetTexture(RenderManager::TEXTURE_TYPE::BASE_COLOR, m_SceneTextures.SceneColor.get()); // t0
		// Bloom result on t9 (full bloom-res). When bloom is off the
		// shader ignores it via PP_FLAG_BLOOM, but bind a valid SRV anyway.
		m_RHI->SetTexture(RenderManager::TEXTURE_TYPE::BLOOM, m_BloomUp[0].get());           // t9

		// Baked color grading LUT on t10 (Texture3D).
		if (m_ColorGradingLUTBaker)
		{
			m_RHI->BindRootTableBySRVIndex((unsigned int)RenderManager::TEXTURE_TYPE::COLOR_GRADING_LUT, m_ColorGradingLUTBaker->GetLUTSRVIndex());// t10
		}

		// Auto-exposure scale buffer on t11. Always bound (valid SRV) so
		// the tonemap PS can read it; the shader only applies it when
		// PP_FLAG_AUTO_EXPOSURE is set, otherwise it uses manual EV.
		if (m_AutoExposure)
		{
			m_RHI->BindRootTableBySRVIndex((unsigned int)RenderManager::TEXTURE_TYPE::AUTO_EXPOSURE, m_AutoExposure->GetExposureSRVIndex());// t11
		}

		DrawScreenPass();
	}

	//======================================================
	// 深度バッファ: SRV -> DEPTH_WRITE に戻す
	//======================================================
	cl->ResourceBarrier(1,
		&CD3DX12_RESOURCE_BARRIER::Transition(
			m_RHI->GetDepthBufferResource(),
			D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
			D3D12_RESOURCE_STATE_DEPTH_WRITE));
}


// ============================================================
//  Frame end: ImGui render -> back buffer to PRESENT -> Present
// ============================================================
void FSceneRenderer::EndFrame()
{
	ID3D12GraphicsCommandList* cl = m_RHI->GetGraphicsCommandList();

	// ImGui (バックバッファは RenderPostProcessing で RENDER_TARGET 済み)
	{
		ImGui::Render();
		ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), cl);
	}

	// バックバッファ: RENDER_TARGET -> PRESENT
	cl->ResourceBarrier(1,
		&CD3DX12_RESOURCE_BARRIER::Transition(
			m_RHI->GetCurrentBackBufferResource(),
			D3D12_RESOURCE_STATE_RENDER_TARGET,
			D3D12_RESOURCE_STATE_PRESENT));

	// コマンド発行 + Present + 前フレーム待ち + Reset
	m_RHI->Present();
}


void FSceneRenderer::DrawScreenPass()
{
	m_RHI->SetVertexBuffer(m_ScreenQuad.get());
	m_RHI->GetGraphicsCommandList()->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
	m_RHI->GetGraphicsCommandList()->DrawInstanced(4, 1, 0, 0);
}


// ============================================================
//  Gaussian Depth of Field.
//   1. CoC/prep : SceneColor -> half-res (RGB=premul color, A=CoC)
//   2. Blur H   : DOFPrep -> DOFPing  (separable, radius = CoC)
//   3. Blur V   : DOFPing -> DOFBlur  (separable, radius = CoC)
//   4. Composite: sharp SceneColor + DOFBlur -> SceneColor
//  SceneColor must be in PIXEL_SHADER_RESOURCE on entry and is left
//  in PIXEL_SHADER_RESOURCE on exit.
// ============================================================
void FSceneRenderer::RenderDOF()
{
	ID3D12GraphicsCommandList* cl = m_RHI->GetGraphicsCommandList();

	auto toRT = [&](RENDER_TARGET* rt)
		{
			cl->ResourceBarrier(1,
				&CD3DX12_RESOURCE_BARRIER::Transition(rt->Resource.Get(),
					D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
					D3D12_RESOURCE_STATE_RENDER_TARGET));
		};
	auto toSRV = [&](RENDER_TARGET* rt)
		{
			cl->ResourceBarrier(1,
				&CD3DX12_RESOURCE_BARRIER::Transition(rt->Resource.Get(),
					D3D12_RESOURCE_STATE_RENDER_TARGET,
					D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE));
		};
	auto setVP = [&](int w, int h)
		{
			D3D12_VIEWPORT vp{ 0.0f, 0.0f, (FLOAT)w, (FLOAT)h, 0.0f, 1.0f };
			D3D12_RECT     sc{ 0, 0, (LONG)w, (LONG)h };
			cl->RSSetViewports(1, &vp);
			cl->RSSetScissorRects(1, &sc);
		};

	const FLOAT clr[4] = { 0, 0, 0, 0 };

	// 線形深度は RenderLighting 側で既に PIXEL_SHADER_RESOURCE。CoC 計算で t5 を読む。
	// SceneColor も入口で PIXEL_SHADER_RESOURCE。

	// ---- 1. CoC / prep : SceneColor(t0) + LinearDepth(t5) -> DOFPrep ----
	// ハーフ解像度テクセルサイズを渡す (ブラー内で使用)。
	SetTexelSize(m_DOFWidth, m_DOFHeight);
	UploadPostProcessConstant();

	toRT(m_DOFPrep.get());
	cl->OMSetRenderTargets(1, &m_DOFPrep->RTVHandle, TRUE, nullptr);
	cl->ClearRenderTargetView(m_DOFPrep->RTVHandle, clr, 0, nullptr);
	setVP(m_DOFWidth, m_DOFHeight);
	m_RHI->SetPipelineState("PostProcessDOFCoC");
	m_RHI->SetTexture(RenderManager::TEXTURE_TYPE::BASE_COLOR, m_SceneTextures.SceneColor.get()); // t0
	m_RHI->SetTexture(RenderManager::TEXTURE_TYPE::LINEAR_DEPTH, m_SceneTextures.LinearDepth.get()); // t5
	DrawScreenPass();
	toSRV(m_DOFPrep.get());

	// ---- 2. Horizontal blur : DOFPrep -> DOFPing (DofPad=0 => 水平) ----
	m_FinalSettings.DofPad = 0.0f;
	UploadPostProcessConstant();
	toRT(m_DOFPing.get());
	cl->OMSetRenderTargets(1, &m_DOFPing->RTVHandle, TRUE, nullptr);
	cl->ClearRenderTargetView(m_DOFPing->RTVHandle, clr, 0, nullptr);
	setVP(m_DOFWidth, m_DOFHeight);
	m_RHI->SetPipelineState("PostProcessDOFBlur");
	m_RHI->SetTexture(RenderManager::TEXTURE_TYPE::BASE_COLOR, m_DOFPrep.get()); // t0
	DrawScreenPass();
	toSRV(m_DOFPing.get());

	// ---- 3. Vertical blur : DOFPing -> DOFBlur (DofPad=1 => 垂直) ----
	m_FinalSettings.DofPad = 1.0f;
	UploadPostProcessConstant();
	toRT(m_DOFBlur.get());
	cl->OMSetRenderTargets(1, &m_DOFBlur->RTVHandle, TRUE, nullptr);
	cl->ClearRenderTargetView(m_DOFBlur->RTVHandle, clr, 0, nullptr);
	setVP(m_DOFWidth, m_DOFHeight);
	m_RHI->SetPipelineState("PostProcessDOFBlur");
	m_RHI->SetTexture(RenderManager::TEXTURE_TYPE::BASE_COLOR, m_DOFPing.get()); // t0
	DrawScreenPass();
	toSRV(m_DOFBlur.get());
	m_FinalSettings.DofPad = 0.0f;
	UploadPostProcessConstant();

	// ---- 4. Composite : sharp(t0) + DOFBlur(t12) -> DOFSharp(full-res) ----
	//  SceneColor 自身を read/write 同時参照できないため、合成結果は一旦
	//  full-res の DOFSharp に書き、その後 CopyResource で SceneColor に戻す。
	toRT(m_DOFSharp.get());
	cl->OMSetRenderTargets(1, &m_DOFSharp->RTVHandle, TRUE, nullptr);
	cl->ClearRenderTargetView(m_DOFSharp->RTVHandle, clr, 0, nullptr);
	setVP(m_RHI->GetBackBufferWidth(), m_RHI->GetBackBufferHeight());
	m_RHI->SetPipelineState("PostProcessDOFComposite");
	m_RHI->SetTexture(RenderManager::TEXTURE_TYPE::BASE_COLOR, m_SceneTextures.SceneColor.get()); // t0 sharp
	m_RHI->SetTexture(RenderManager::TEXTURE_TYPE::LINEAR_DEPTH, m_SceneTextures.LinearDepth.get()); // t5
	m_RHI->BindRootTableBySRVIndex((unsigned int)RenderManager::TEXTURE_TYPE::DOF, m_DOFBlur->SRVIndex); // t12
	DrawScreenPass();
	toSRV(m_DOFSharp.get());

	// 合成結果 (DOFSharp) を SceneColor へコピーし直す。
	//  SceneColor: SRV -> COPY_DEST, DOFSharp: SRV -> COPY_SOURCE
	cl->ResourceBarrier(1,
		&CD3DX12_RESOURCE_BARRIER::Transition(m_SceneTextures.SceneColor->Resource.Get(),
			D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST));
	cl->ResourceBarrier(1,
		&CD3DX12_RESOURCE_BARRIER::Transition(m_DOFSharp->Resource.Get(),
			D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE));
	cl->CopyResource(m_SceneTextures.SceneColor->Resource.Get(), m_DOFSharp->Resource.Get());
	cl->ResourceBarrier(1,
		&CD3DX12_RESOURCE_BARRIER::Transition(m_SceneTextures.SceneColor->Resource.Get(),
			D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE));
	cl->ResourceBarrier(1,
		&CD3DX12_RESOURCE_BARRIER::Transition(m_DOFSharp->Resource.Get(),
			D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE));

	// フル解像度ビューポート / テクセルサイズを復元。
	setVP(m_RHI->GetBackBufferWidth(), m_RHI->GetBackBufferHeight());
	SetTexelSize(m_RHI->GetBackBufferWidth(), m_RHI->GetBackBufferHeight());
	UploadPostProcessConstant();
}


// ============================================================
//  Bloom: bright-pass -> downsample chain -> upsample chain.
//  Reads SceneColor (must already be in PIXEL_SHADER_RESOURCE).
//  Result ends in m_BloomUp[0] (full bloom-res), left in SRV state.
// ============================================================
void FSceneRenderer::RenderBloom()
{
	ID3D12GraphicsCommandList* cl = m_RHI->GetGraphicsCommandList();

	auto toRT = [&](RENDER_TARGET* rt)
		{
			cl->ResourceBarrier(1,
				&CD3DX12_RESOURCE_BARRIER::Transition(rt->Resource.Get(),
					D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
					D3D12_RESOURCE_STATE_RENDER_TARGET));
		};
	auto toSRV = [&](RENDER_TARGET* rt)
		{
			cl->ResourceBarrier(1,
				&CD3DX12_RESOURCE_BARRIER::Transition(rt->Resource.Get(),
					D3D12_RESOURCE_STATE_RENDER_TARGET,
					D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE));
		};

	// The full-screen quad spans NDC -1..1, so the rasterised area is defined
	// purely by the viewport/scissor. They are left at back-buffer size by
	// BeginFrame, so each (smaller) bloom mip MUST set its own viewport or it
	// would only be drawn into the top-left corner.
	auto setVP = [&](int w, int h)
		{
			D3D12_VIEWPORT vp{ 0.0f, 0.0f, (FLOAT)w, (FLOAT)h, 0.0f, 1.0f };
			D3D12_RECT     sc{ 0, 0, (LONG)w, (LONG)h };
			cl->RSSetViewports(1, &vp);
			cl->RSSetScissorRects(1, &sc);
		};

	const FLOAT clr[4] = { 0,0,0,1 };

	// ---- Bright-pass: SceneColor -> m_BloomMip[0] ----
	toRT(m_BloomMip[0].get());
	cl->OMSetRenderTargets(1, &m_BloomMip[0]->RTVHandle, TRUE, nullptr);
	cl->ClearRenderTargetView(m_BloomMip[0]->RTVHandle, clr, 0, nullptr);
	setVP(m_BloomMipW[0], m_BloomMipH[0]);
	m_RHI->SetPipelineState("PostProcessBloomThreshold");
	m_RHI->SetTexture(RenderManager::TEXTURE_TYPE::BASE_COLOR, m_SceneTextures.SceneColor.get()); // t0
	DrawScreenPass();
	toSRV(m_BloomMip[0].get());

	// ---- Downsample chain: mip[i-1] -> mip[i] ----
	for (int i = 1; i < BLOOM_MIPS; ++i)
	{
		SetTexelSize(m_BloomMipW[i - 1], m_BloomMipH[i - 1]);
		UploadPostProcessConstant();

		toRT(m_BloomMip[i].get());
		cl->OMSetRenderTargets(1, &m_BloomMip[i]->RTVHandle, TRUE, nullptr);
		cl->ClearRenderTargetView(m_BloomMip[i]->RTVHandle, clr, 0, nullptr);
		setVP(m_BloomMipW[i], m_BloomMipH[i]);          // dest mip size
		m_RHI->SetPipelineState("PostProcessBloomDownsample");
		m_RHI->SetTexture(RenderManager::TEXTURE_TYPE::BASE_COLOR, m_BloomMip[i - 1].get()); // t0 = source
		DrawScreenPass();
		toSRV(m_BloomMip[i].get());
	}

	// ---- Upsample chain (deepest down mip -> up[0]) ----
	for (int i = BLOOM_MIPS - 2; i >= 0; --i)
	{
		RENDER_TARGET* lowSrc = (i == BLOOM_MIPS - 2)
			? m_BloomMip[BLOOM_MIPS - 1].get()
			: m_BloomUp[i + 1].get();

		SetTexelSize(m_BloomMipW[i + 1], m_BloomMipH[i + 1]);
		UploadPostProcessConstant();

		toRT(m_BloomUp[i].get());
		cl->OMSetRenderTargets(1, &m_BloomUp[i]->RTVHandle, TRUE, nullptr);
		cl->ClearRenderTargetView(m_BloomUp[i]->RTVHandle, clr, 0, nullptr);
		setVP(m_BloomMipW[i], m_BloomMipH[i]);          // dest (up) mip size
		m_RHI->SetPipelineState("PostProcessBloomUpsample");
		m_RHI->SetTexture(RenderManager::TEXTURE_TYPE::BASE_COLOR, lowSrc);              // t0 low-res
		m_RHI->SetTexture(RenderManager::TEXTURE_TYPE::BLOOM, m_BloomMip[i].get()); // t9 hi-res same-res mip
		DrawScreenPass();
		toSRV(m_BloomUp[i].get());
	}

	// restore full-res viewport + texel size for the tonemap pass.
	setVP(m_RHI->GetBackBufferWidth(), m_RHI->GetBackBufferHeight());
	SetTexelSize(m_RHI->GetBackBufferWidth(), m_RHI->GetBackBufferHeight());
	UploadPostProcessConstant();
}
