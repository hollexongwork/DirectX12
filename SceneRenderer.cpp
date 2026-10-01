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
#include "FogRendering.h"
#include "IBLBaker.h"
#include "AutoExposure.h"
#include "ColorGradingLUTBaker.h"
#include "ScreenshotCapture.h"
#include "TemporalAASelfTest.h"
#include "PostProcessUpscale.h"
#include "TemporalAA.h"
#include "Time.h"

#include <ctime>

#include "D3DX12.h"

// スクリーンパス用ヘルパー (RenderDOF / RenderBloom 共用。他の翻訳単位とも共有)
#include "SceneRenderingUtils.h"

#include "ImGUI/imgui.h"
#include "ImGUI/imgui_impl_win32.h"
#include "ImGUI/imgui_impl_dx12.h"

#include <cmath>


// ============================================================
//  Lifetime
// ============================================================
FSceneRenderer::~FSceneRenderer() = default;


FSceneRenderer::FSceneRenderer(RenderManager* RHI)
	: m_RHI(RHI)
{
	// 生成時は レンダー解像度 R = ポスト解像度 P = 出力解像度 O (バックバッファ) で確保する。
	// INI の適用 (SettingsManager::Initialize) はこの後なので、最初の BeginFrame で
	// 今フレームのビューファミリと食い違えば ResizeRenderTargets が作り直す (§5.1)
	const unsigned int bbW = (unsigned int)m_RHI->GetBackBufferWidth();
	const unsigned int bbH = (unsigned int)m_RHI->GetBackBufferHeight();

	// (1) レンダー解像度のシーンテクスチャ (深度 SRV は RenderManager の深度バッファ = BB へ張る)
	m_SceneTextures.Init(m_RHI, bbW, bbH);

	// (2) 解像度非依存の初期化 + DOF (R) / Bloom (O/2 固定。出力解像度 O = バックバッファは再確保されない)
	InitScreenQuad();
	InitIBL();
	InitLightBuffer();
	InitPostProcess();
	InitDOF(bbW, bbH);
	InitBloom(bbW, bbH);

	// (3) タイルドライトカリング (Injection -> Compact の 2 コンピュートパス)。
	//     バッファは容量 (2 x 出力解像度 = スクリーンパーセンテージ上限 200 %) で一度だけ確保し、
	//     今フレームの次元は SetViewSize で決める (再確保の経路は無い。§5.3)
	m_LightGrid = std::make_unique<FLightGridInjection>(m_RHI);
	m_LightGrid->Init(2u * bbW, 2u * bbH);
	m_LightGrid->SetViewSize(bbW, bbH);

	// シャドウマップ描画系 (CSM + ローカルシャドウアトラス)
	m_ShadowRenderer = std::make_unique<FShadowSceneRenderer>(m_RHI);

	// Lumen Surface Cache (カード + アトラス + ライティングコンピュート)
	m_LumenScene = std::make_unique<FLumenSceneData>(m_RHI);
	m_LumenScene->Init();

	// Exponential Height Fog + Volumetric Fog (froxel ボリューム構築)。
	// Inscattering Color Cubemap には IBL の prefilter キューブ
	// (ミップ = ぼかし段階) を割り当てる (InitIBL 済み)。
	m_FogRenderer = std::make_unique<FFogSceneRenderer>(m_RHI);
	m_FogRenderer->Init();
	if (m_IBLBaker)
	{
		m_FogRenderer->SetInscatteringColorCubemap(
			m_IBLBaker->GetPrefilterSRVIndex(), IBLBaker::PREFILTER_MIP_COUNT);
	}

	// Temporal AA / TAAU (コンピュートルートシグネチャ / PSO / 定数リング / ダミー / 自己テストバッファ)。
	// .cso が欠落した順列は PSO が null のまま残り、PrepareViewRectsForRendering が AA 無しへフォールバックする
	m_TemporalUpscaler = std::make_unique<FDefaultTemporalUpscaler>(m_RHI);
	m_TemporalUpscaler->Init();

	// スクリーンショット (READBACK バッファは初回キャプチャ時に確保)
	m_Screenshot = std::make_unique<FScreenshotCapture>(m_RHI);

	// 確保済みの解像度 (PrepareViewRectsForRendering が今フレームの R / P と比べる)
	m_AllocatedRenderExtent = { bbW, bbH };
	m_AllocatedPostExtent = { bbW, bbH };

#ifdef _DEBUG
	// Debug ビルドは最初の BeginFrame で TAA 自己テスト (CPU + GPU パリティ) を走らせる (§9.2)
	m_TAADebug.bRequestSelfTest = true;
#endif
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
	// equirect 環境マップは IBL ベイクの入力としてのみ使う。Bake は末尾で
	// FlushAndResetCommandList により GPU 完了まで待つため、ベイク後は
	// ローカルのまま破棄してよい (TEXTURE のデストラクタが DeferredRelease
	// 経由でリソース + SRV 枠を遅延解放する)。
	std::unique_ptr<TEXTURE> environmentTexture = m_RHI->LoadTexture("Asset/Texture/kloppenheim_06_puresky_4k.dds", true);

	// IBL の事前計算は IBLBaker に委譲
	m_IBLBaker = std::make_unique<IBLBaker>(m_RHI);
	m_IBLBaker->Init();
	m_IBLBaker->Bake(environmentTexture->Resource.Get(), environmentTexture->SRVIndex);
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

	// Bloom / DOF は解像度付きの InitBloom(O) / InitDOF(R) で別に作る
	// (Bloom はコンストラクタのみ、DOF はコンストラクタと ResizeRenderTargets)
}


void FSceneRenderer::InitBloom(unsigned int OutputWidth, unsigned int OutputHeight)
{
	// Build a half-res bloom mip chain. Each mip is its own RT (RTV+SRV).
	// m_BloomMip[i]  : downsample chain  (mip0 = bright-pass at half res)
	// m_BloomUp[i]   : upsample accumulators (m_BloomUp[0] = full bloom)
	// mip0 = 出力解像度 O の半分 (切り捨て, >= 1)、以降も半分ずつ。
	// ポスト解像度 P ではなく O に固定する: ブルームのカーネルは各 mip のテクセル単位なので、
	// P に比例させると出力画面上のハローの大きさがスクリーンパーセンテージで変わる (DOF と同じ SP 不変)。
	// P != 2·mip0 の入力はしきい値パスの 4 タップボックスが前置フィルタする
	int w = (int)(OutputWidth / 2u);
	int h = (int)(OutputHeight / 2u);

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


void FSceneRenderer::InitDOF(unsigned int Width, unsigned int Height)
{
	// ハーフ解像度で CoC + ブラーを行う (Gaussian 相当)。基準はレンダー解像度 R
	m_DOFWidth = (int)((Width + 1u) / 2u);
	m_DOFHeight = (int)((Height + 1u) / 2u);

	m_DOFPrep = m_RHI->CreateRenderTarget(m_DOFWidth, m_DOFHeight, DXGI_FORMAT_R16G16B16A16_FLOAT);
	m_DOFPrep->Resource->SetName(L"DOFPrep");
	m_DOFPing = m_RHI->CreateRenderTarget(m_DOFWidth, m_DOFHeight, DXGI_FORMAT_R16G16B16A16_FLOAT);
	m_DOFPing->Resource->SetName(L"DOFPing");
	m_DOFBlur = m_RHI->CreateRenderTarget(m_DOFWidth, m_DOFHeight, DXGI_FORMAT_R16G16B16A16_FLOAT);
	m_DOFBlur->Resource->SetName(L"DOFBlur");

	// フル解像度 (R) のシャープコピー (合成時の読み取り元)。
	m_DOFSharp = m_RHI->CreateRenderTarget(Width, Height, DXGI_FORMAT_R16G16B16A16_FLOAT);
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

	// テクセルサイズはレンダラ管轄 (レンダー解像度 R で開始。ベースパス〜半透明の屈折が読む。
	// DOF / Bloom / トーンマップ / アップスケールはパス内で切り替える: §4.1 テクセル規則)
	SetTexelSize((int)m_ViewFamily.RenderExtent.x, (int)m_ViewFamily.RenderExtent.y);
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
//  View family / view state (UE PrepareViewRectsForRendering /
//  PrepareViewStateForVisibility / FSceneViewState の確定)
// ============================================================
namespace
{
	// 転置して格納 (C++ は転置前で保持し、HLSL へは転置して渡す規約)
	void StoreT(XMFLOAT4X4& Dst, const XMFLOAT4X4& Src)
	{
		XMStoreFloat4x4(&Dst, XMMatrixTranspose(XMLoadFloat4x4(&Src)));
	}
}


// BeginFrame 先頭 (m_RHI->BeginFrame の前): 今フレームのビューファミリを決め、
// レンダー / ポスト解像度のターゲットを必要なら作り直す (§4.3)
void FSceneRenderer::PrepareViewRectsForRendering()
{
	const XMUINT2 O = { (unsigned)m_RHI->GetBackBufferWidth(), (unsigned)m_RHI->GetBackBufferHeight() };
	FViewFamilyInfo F = ComputeViewFamilyInfo(m_AAParams, O, m_TemporalUpscaler->IsR11G11B10HistorySupported());

	// TAA の PSO (.cso) が揃っていなければ AA 無しの構成へフォールバック (クラッシュさせない)。
	// AA 手法が変わるので次のビュー状態はカメラカット扱いになる (§5.4)
	m_TAAStats.bFallbackMissingPSO = false;
	if (F.bTemporalAA && !m_TemporalUpscaler->IsReady(F))
	{
		FAntiAliasingParams q = m_AAParams;
		q.AntiAliasingMethod = 0;
		F = ComputeViewFamilyInfo(q, O, false);
		m_TAAStats.bFallbackMissingPSO = true;		// IsReady 内で組合せごとに初回のみログ出力
	}
	m_ViewFamily = F;

	// ---- 解像度変更 (§5.1): 確保済みの R / P と比べる遅延再確保 ----
	// INI 適用直後の最初のフレームも、ImGui / テストドライバによる変更もここで拾う
	// (ImGui / SettingsManager 側にフックは要らない)。bRequestReallocate は同一サイズでも再確保する
	if (m_TAADebug.bRequestReallocate ||
		F.RenderExtent.x != m_AllocatedRenderExtent.x || F.RenderExtent.y != m_AllocatedRenderExtent.y ||
		F.PostProcessExtent.x != m_AllocatedPostExtent.x || F.PostProcessExtent.y != m_AllocatedPostExtent.y)
	{
		ResizeRenderTargets(F);		// §5.2 (GPU 同期を含む。稀なイベント)
	}

	// 容量確保済みライトグリッドの今フレームの次元 (FillForwardLightData / Dispatch より前)
	m_LightGrid->SetViewSize(F.RenderExtent.x, F.RenderExtent.y);

	// RHI の既定ビューポート = R。直後の m_RHI->BeginFrame / FlushAndResetCommandList /
	// シャドウ・Lumen カードキャプチャ後の RestoreDefaultViewport はすべて R へ戻る。
	// ビューポートを設定しないパス (ベース / LinearDepth / デファード / 高さフォグ / 半透明) は
	// これで R に描く。TAA 以降のポストパスは各自でビューポートを設定する (§4.1 ビューポート規則)
	m_RHI->SetDefaultViewportSize(F.RenderExtent.x, F.RenderExtent.y);
}


// 既定ビューポート (= R) を現在のコマンドリストへ即時適用する
void FSceneRenderer::ApplyRenderViewport()
{
	m_RHI->RestoreDefaultViewport();
}


// ============================================================
//  ResizeRenderTargets (§5.2。順序が規範)
//  レンダー解像度 R / ポスト解像度 P のターゲットを作り直す。
//  (0) FlushAndReset -> (1) 旧リソースを遅延削除キューへ -> (2) WaitGPU で実解放 -> (3) 生成
//  ・(0) で記録済み未実行のコマンド (初回フレームならコンストラクタの初期バリア) を実行し、
//    GPU をアイドルにする。以降、実行中のコマンドが解放済みリソース / 書き換えたデスクリプタを
//    参照することは無い
//  ・(1) の遅延削除エントリは (0) で進んだフェンス値を持ち、(2) の WaitGPU がちょうどその値を
//    Signal + 待機 + Flush するので、旧メモリは新規確保の前に返る (VRAM ピーク = 旧 or 新のみ)
//  ・深度 DSV は RenderManager の 1 枠ヒープへ同じ CPU ハンドルで作り直す (記録済みコマンドは無い)
//  ・新しい SRV / UAV / RTV は全てフリーリストから確保し直す
// ============================================================
void FSceneRenderer::ResizeRenderTargets(const FViewFamilyInfo& F)
{
	const XMUINT2 R = F.RenderExtent;
	const XMUINT2 P = F.PostProcessExtent;
	const bool bForce = m_TAADebug.bRequestReallocate;		// ワンショット (同一サイズでも再確保。リーク検査用)
	m_TAADebug.bRequestReallocate = false;
	const bool bR = bForce || (R.x != m_AllocatedRenderExtent.x || R.y != m_AllocatedRenderExtent.y);
	const bool bP = bForce || (P.x != m_AllocatedPostExtent.x || P.y != m_AllocatedPostExtent.y);
	if (!bR && !bP)
	{
		return;
	}

	FVolumetricFog* volumetricFog = m_FogRenderer ? m_FogRenderer->GetVolumetricFog() : nullptr;

	// (0) 記録済みで未実行のコマンド (コンストラクタの初期バリア等) を実行し GPU をアイドルにする。
	//     リストはこの時点で空か、初回フレームならコンストラクタの記録のみ
	m_RHI->FlushAndResetCommandList();

	// (1) 旧リソースを遅延削除キューへ (フェンス値 = 現在値)
	if (bR)
	{
		m_SceneTextures.Release(m_RHI);						// 12 RT + DepthSRV + LinearDepthDisplaySRV
		m_RHI->ReleaseDepthBuffer();						// リソースのみ遅延解放 (DSV 枠は保持)
		m_DOFPrep.reset();
		m_DOFPing.reset();
		m_DOFBlur.reset();
		m_DOFSharp.reset();
		if (m_LumenScene)
		{
			m_LumenScene->ReleaseScreenTextures();			// Probe*, ProbeSH[2], DiffuseIndirect[2], Reflection (GlobalSDF / RCSH / アトラスは保持)
		}
		if (volumetricFog)
		{
			volumetricFog->ReleaseVolumes();
		}
	}
	if (bP)
	{
		m_TonemapOutput.reset();							// 実際のポスト入力サイズで EnsureTonemapOutput が遅延再作成
	}

	// (2) 待機 + 遅延削除の実解放 (新規確保の前に旧メモリを返す = VRAM ピーク抑制)
	m_RHI->WaitGPU();

	// (3) 再確保 (SRV / UAV / RTV は全てフリーリストから取り直す。DSV だけは同一 CPU 枠へ再作成)
	if (bR)
	{
		m_RHI->CreateDepthBuffer(R.x, R.y);					// DEPTH_WRITE 開始 (BeginFrame のクリアと整合)
		m_SceneTextures.Init(m_RHI, R.x, R.y);				// 読み取り状態への初期バリアを記録 (コンストラクタと同じ)
		InitDOF(R.x, R.y);
		if (m_LumenScene)
		{
			m_LumenScene->CreateScreenTextures(R.x, R.y);	// ジッタ位相 (m_ProbeJitterIndex) は保持。履歴は下の ViewRectSize 規則で無効化
		}
		if (volumetricFog)
		{
			volumetricFog->CreateVolumes(R.x, R.y);			// m_bHistoryValid = false のみ, 状態 UAV (ジッタ位相 m_FrameNumber は保持)
		}
		// PrevSceneColor / PrevLinearDepth は未定義 -> Lumen / Fog 履歴を 1 フレーム無効化する
		// (同一サイズの再確保でも。IsScreenHistoryValid が PrevViewInfo.ViewRectSize == R を要求する)
		m_ViewState.PrevFrameViewInfo.ViewRectSize = { 0u, 0u };
		m_AllocatedRenderExtent = R;
	}
	if (bP)
	{
		m_AllocatedPostExtent = P;							// Bloom チェーンは O/2 固定 (再確保しない)
	}

	++m_TAAStats.NumResizes;

	char msg[160];
	sprintf_s(msg, "[ScreenPercentage] resize R=%ux%u P=%ux%u freeSRV=%zu\n",
		R.x, R.y, P.x, P.y, m_RHI->GetNumFreeSRVDescriptors());
	OutputDebugStringA(msg);
}


// RenderBasePass 先頭: FViewInfo を再構築して b0 (448 B) を書く (§4.4)。
// ジッタは bTemporalAA か bForceJitterWithoutTAA の時だけ
void FSceneRenderer::PrepareViewStateForVisibility(const FSceneView& View)
{
	FViewInfo& V = m_ViewInfo;
	FSceneViewState& S = m_ViewState;
	const FViewFamilyInfo& F = m_ViewFamily;
	const FAntiAliasingParams& p = m_AAParams;
	const XMUINT2 R = F.RenderExtent;

	V.ViewRectSize = R;
	V.UnscaledViewRectSize = F.OutputExtent;
	V.AntiAliasingMethod = F.AntiAliasingMethod;
	V.PrimaryScreenPercentageMethod = F.PrimaryScreenPercentageMethod;

	if (!View.bValid)
	{
		// 従来挙動: b0 は据え置き (前回値)。次の有効フレームは強制カメラカット
		V.bValid = false;
		V.bPrevViewInfoValid = false;
		V.bPrevTransformsReset = false;
		S.bForceCameraCut = true;
		return;
	}
	V.bValid = true;
	V.NearClip = View.NearClip;
	V.FarClip = View.FarClip;
	V.ViewMatrices.Init(View.ViewMatrix, View.ProjectionMatrix, View.ViewOrigin);   // NoAA (ジッタ 0)

	// ---- カメラカット (UE FSceneView::bCameraCut + レンダラ側の規則; §4.4 表) ----
	V.bCameraCut = View.bCameraCut || S.bForceCameraCut || !S.bPrevFrameViewInfoValid
		|| (S.PrevAntiAliasingMethod != F.AntiAliasingMethod) || m_TAADebug.bRequestHistoryReset;
	S.bForceCameraCut = false;
	m_TAADebug.bRequestHistoryReset = false;

	// ---- テンポラルジッタ (UE 4.26 PreVisibilityFrameSetup / 5.x PrepareViewStateForVisibility) ----
	V.TemporalJitterPixels = { 0.0f, 0.0f };
	V.TemporalJitterIndex = 0;
	V.TemporalJitterSequenceLength = 1;
	const bool bJitter = (F.bTemporalAA || m_TAADebug.bForceJitterWithoutTAA) && !m_TAADebug.bDisableJitter;
	if (bJitter)
	{
		const bool bTAAU = (F.PrimaryScreenPercentageMethod == EPrimaryScreenPercentageMethod::TemporalUpscale);
		const int  CVar  = p.TemporalAASamples;
		// TAAU: N = int(CVar * max(1, 1/f^2)) (切り捨て)、それ以外: CVar (5 -> 4)。[1, 255]
		const int  N     = ComputeTemporalAASampleCount(bTAAU, CVar, F.EffectivePrimaryResolutionFraction);

		int Index = (int)S.TemporalAASampleIndex + 1;
		if (Index >= N || V.bCameraCut) Index = 0;                                  // [M] UE 4.26/5.x はカットで 0 へ戻す
		if (m_TAADebug.OverrideTemporalIndex >= 0)
			Index = m_TAADebug.OverrideTemporalIndex % N;                           // r.TemporalAA.Debug.OverrideTemporalIndex (凍結: 状態は進めない)
		else
			S.TemporalAASampleIndex = (uint32_t)Index;

		const XMFLOAT2 s = ComputeTemporalAASample(bTAAU, CVar, N, Index, p.TemporalAAFilterSize);
		V.TemporalJitterPixels = s;
		V.TemporalJitterIndex = Index;
		V.TemporalJitterSequenceLength = N;
		// レンダー (入力) 解像度でクリップ空間へ (UE: SampleX * 2 / ViewRect.W, SampleY * -2 / ViewRect.H)
		V.ViewMatrices.HackAddTemporalAAProjectionJitter({ s.x * 2.0f / (float)R.x, s.y * -2.0f / (float)R.y });
	}

	// ---- 前フレーム情報 (フレーム中は不変のスナップショット) ----
	V.PrevViewInfo = S.PrevFrameViewInfo;
	V.bPrevViewInfoValid = S.bPrevFrameViewInfoValid && !V.bCameraCut;
	V.bPrevTransformsReset = false;
	if (V.bCameraCut)
	{
		V.PrevViewInfo.ViewMatrices = V.ViewMatrices;          // 静止画素のカメラモーション = 0 (ジッタ込みの今フレーム)
		V.PrevViewInfo.TemporalAAHistory.SafeRelease();         // TAA 履歴を読まない
	}
	else if (IsLargeCameraMovement(V.ViewMatrices, V.PrevViewInfo.ViewMatrices, p.CameraRotationThreshold, p.CameraTranslationThreshold))
	{
		V.PrevViewInfo.ViewMatrices = V.ViewMatrices;          // UE bPrevTransformsReset: 履歴は保持しクランプに任せる [M]
		V.bPrevTransformsReset = true;
	}

	// ---- ClipToPrevClip (NoAA x NoAA, row-vector: PrevClip = ThisClip * C2P) ----
	// ワールド絶対座標の VP を float で逆行列 x 積にすると、静止カメラでも |カメラ位置| に比例した
	// 再投影誤差が残る。UE と同じくカメラ相対 (Translated) で double 合成する (ComputeClipToPrevClip)。
	// カット / 大移動では Prev = Cur なので厳密に単位行列
	V.ClipToPrevClip = ComputeClipToPrevClip(V.ViewMatrices, V.PrevViewInfo.ViewMatrices);

	// ---- Automatic View Mip Bias (TemporalUpscale 時のみ; UE 4.26 は TAAU 分岐内で計算) ----
	const float bias = ComputeViewTextureMipBias(F, p);
	V.MaterialTextureMipBias = bias;
	V.StateFrameIndex = S.FrameIndex;

	// ---- b0 (§3.1)。光源 2 フィールドは直後の SetupLightConstants が書く ----
	// 先頭 256 B は従来と同じ値 / 同じ計算順 (ジッタ 0 なら基準とビット一致)
	VIEW_CONSTANT& c = m_ViewConstant;
	StoreT(c.View, V.ViewMatrices.ViewMatrix);
	StoreT(c.Projection, V.ViewMatrices.ProjectionMatrix);                  // ジッタ込み
	StoreT(c.InvViewProjection, V.ViewMatrices.InvViewProjectionMatrix);    // ジッタ込み
	c.WorldCameraOrigin = { View.ViewOrigin.x, View.ViewOrigin.y, View.ViewOrigin.z, 1.0f };
	c.NearFar = { View.NearClip, View.FarClip, 0.0f, 0.0f };
	StoreT(c.PrevViewProjection, V.PrevViewInfo.ViewMatrices.ViewProjectionMatrix);   // 前フレームのジッタ込み
	StoreT(c.ClipToPrevClip, V.ClipToPrevClip);                                       // NoAA x NoAA
	const XMFLOAT2 jc = V.ViewMatrices.TemporalAAProjectionJitter;
	const XMFLOAT2 jp = V.PrevViewInfo.ViewMatrices.TemporalAAProjectionJitter;
	c.TemporalAAJitter = { jc.x, jc.y, jp.x, jp.y };
	c.TemporalAAParams = { (float)V.TemporalJitterIndex, (float)V.TemporalJitterSequenceLength, V.TemporalJitterPixels.x, V.TemporalJitterPixels.y };
	c.ViewSizeAndInvSize = { (float)R.x, (float)R.y, 1.0f / (float)R.x, 1.0f / (float)R.y };
	c.MaterialTextureMipBias = bias;
	c.MaterialTextureDerivativeMultiply = std::exp2(bias);
	c.StateFrameIndexMod8 = F.bTemporalAA ? S.GetFrameIndexMod8() : 0u;       // [PORT] AA 無効時は 0 (基準画像を保つ)
	c.StateFrameIndex = S.FrameIndex;
}


// RenderPostProcessing の最後: 今フレームのビュー情報を前フレーム情報として確定する (§4.9)
void FSceneRenderer::CommitViewState(const FTemporalAAHistory& OutputHistory)
{
	if (!m_ViewInfo.bValid) return;                         // カメラ不在フレームは確定しない (次の有効フレームはカット)

	FPreviousViewInfo& P = m_ViewState.PrevFrameViewInfo;
	P.ViewMatrices          = m_ViewInfo.ViewMatrices;      // ジッタ込み + NoAA + ジッタ値
	P.TemporalAAHistory     = OutputHistory;                // TAA 未実行フレームは無効 -> 次の TAA は bCameraCut 扱い
	P.ViewRectSize          = m_ViewInfo.ViewRectSize;
	P.SceneColorPreExposure = 1.0f;
	m_ViewState.bPrevFrameViewInfoValid = true;
	m_ViewState.PrevAntiAliasingMethod  = m_ViewInfo.AntiAliasingMethod;
	m_ViewState.FrameIndex++;
}


// Lumen / Volumetric Fog のスクリーン履歴を今フレーム使えるか (§4.9)。
// カット / 初回 (bPrevViewInfoValid = false)、大移動 (前フレーム行列が今フレームで
// 置き換えられている)、レンダー解像度の変更 (PrevSceneColor / PrevLinearDepth が未定義)
// のいずれでも 1 フレームだけ無効になる
bool FSceneRenderer::IsScreenHistoryValid() const
{
	const FViewInfo& V = m_ViewInfo;
	const XMUINT2& prevSize = V.PrevViewInfo.ViewRectSize;
	return V.bPrevViewInfoValid && !V.bPrevTransformsReset
		&& prevSize.x == V.ViewRectSize.x && prevSize.y == V.ViewRectSize.y;
}


// Lumen / LightGrid のデバッグ表示は SceneColor を TAA より前で置き換える。
// その間は TAA 履歴をバイパスする (CB bCameraCut = 1 のみ。ビュー状態 / 他の履歴には影響しない)
bool FSceneRenderer::IsPreTAADebugViewActive() const
{
	const bool bLumenDebug = m_LumenScene && m_LumenScene->GetParams().DebugMode != 0;
	const bool bLightGridDebug = m_LightGrid && m_LightGrid->GetParams().DebugMode != 0;
	return bLumenDebug || bLightGridDebug;
}


// ============================================================
//  Frame begin: RHI prep -> G-Buffer open (base pass setup)
// ============================================================
void FSceneRenderer::BeginFrame()
{
	// (0) フレーム毎の有効フラグのリセット。早期リターンし得るどのパスよりも前に行う
	//     (RenderTranslucency は半透明が無いと RenderResponsiveAAMask を呼ばずに戻る。
	//      再確保されたテクスチャはクリアされていない)。true にするのは今フレームに
	//     クリア + 書き込みをしたパス (RenderVelocities / RenderResponsiveAAMask) だけ
	m_bVelocityValid = false;
	m_bResponsiveMaskValid = false;

	// (a) 前フレームで記録したスクリーンショットのコピーがあれば WaitGPU して BMP へ書き出す
	// (コピーを含むコマンドリストは前フレームの Present で実行済み)
	m_Screenshot->ResolvePending();

	// (b) 自己テスト要求 (ImGui "Run Self Test" / -taaselftest / Debug ビルドの初回)。
	//     ImGui の構築中ではなく必ずここ (フレーム先頭) で実行する
	if (m_TAADebug.bRequestSelfTest)
	{
		m_TAADebug.bRequestSelfTest = false;
		// CPU テスト + GPU パリティ (TemporalAASelfTest_CS。FlushAndResetCommandList で GPU を待つ)
		m_TAAStats.LastSelfTestFailures = RunTemporalAASelfTests(*this, true);
	}

	// (c) 今フレームのビューファミリ (解像度 / AA 構成)
	PrepareViewRectsForRendering();

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
	// UE の ViewFrustum と同じくジッタ前 (NoAA) の ViewProjection から作る
	// (ジッタでカリング結果がフレーム毎に揺れないように)。行列は転置前で保持している。
	// カメラ不在 (bValid = false) のフレームは前回の有効な平面を保持する
	// (従来の「b0 据え置き」と同じ挙動。最初の有効フレーム前は平面無し = 全て可視)
	if (m_ViewInfo.bValid)
	{
		GetViewFrustumBounds(m_ViewFrustum, XMLoadFloat4x4(&m_ViewInfo.ViewMatrices.ViewProjectionNoAAMatrix), true, true);
	}

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

	// ---- ビュー状態 + VIEW 定数 (b0: カメラ) ----
	// FViewUniformShaderParameters と同様、FSceneView (ゲーム側で
	// スナップショット済みのカメラ情報) を FViewInfo (カメラカット /
	// 大移動判定 / 前フレームのスナップショット) と b0 の行列 / カメラ原点 /
	// NearFar へ解決する。カメラ不在 (View.bValid = false) のフレームは
	// 前回の VIEW 定数を保持し (従来挙動)、次の有効フレームを強制カットにする。
	// 代表ディレクショナルライトは直後の SetupLightConstants が書く。
	PrepareViewStateForVisibility(View);

	// FScene のライトリストを VIEW 定数 (directional) /
	// FORWARD_LIGHT 定数 / ライトバッファ (local, t13) へ解決する
	SetupLightConstants(Scene);

	// ---- FOG 定数 (b7: InitFogConstants) ----
	// FScene の ExponentialFogs[0] を、カメラ高さ / 太陽ライトと合わせて
	// ビュー毎のフォグパラメータへ解決する (フォグ不在なら恒等値)。
	// Volumetric Fog のコンピュートとフォグパスは RenderLighting 側。
	if (m_FogRenderer)
	{
		m_FogRenderer->InitFogConstants(Scene, m_ViewConstant, m_FrameDirectionalLight);
	}

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
	const FLumenFrameInputs inputs = MakeLumenFrameInputs();
	m_TAAStats.bLumenHistoryValid = inputs.bHistoryValid;	// 統計 / テストドライバのログ用
	m_LumenScene->RenderLumenSceneLighting(inputs);
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

	// レンダー解像度 R (スクリーンテクスチャ / プローブグリッドの寸法)
	inputs.ScreenWidth = m_ViewFamily.RenderExtent.x;
	inputs.ScreenHeight = m_ViewFamily.RenderExtent.y;

	// ---- 前フレーム (m_ViewInfo.PrevViewInfo: フレーム先頭のスナップショット) ----
	// PrevSceneColor / PrevLinearDepth は前フレームのジッタ込みラスタなので、
	// リプロジェクションもジッタ込みの前フレーム行列を使う (§4.9)
	const FViewMatrices& prev = m_ViewInfo.PrevViewInfo.ViewMatrices;
	XMStoreFloat4x4(&inputs.PrevViewProjectionT, XMMatrixTranspose(XMLoadFloat4x4(&prev.ViewProjectionMatrix)));
	XMStoreFloat4x4(&inputs.PrevInvViewProjectionT, XMMatrixTranspose(XMLoadFloat4x4(&prev.InvViewProjectionMatrix)));
	inputs.PrevCameraOrigin = { prev.ViewOrigin.x, prev.ViewOrigin.y, prev.ViewOrigin.z, 1.0f };
	inputs.bHistoryValid = IsScreenHistoryValid();

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
//  PrevSceneColor へ (LinearDepth を PrevLinearDepth へ) コピーする。
//  RenderPostProcessing 先頭 (SceneColor が加工される前) に呼ぶ。
//  Lumen のスクリーンスペーストレース (スクリーンプローブ / 反射) が
//  次フレームにリプロジェクションで採光する。
//  テクスチャのコピーのみ。そのフレームのビュー行列の確定は
//  RenderPostProcessing の最後の CommitViewState が行う。
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
}


// ============================================================
//  Lighting: Light Grid -> Volumetric Fog -> LinearDepth ->
//            Deferred lighting -> Height Fog -> HDR SceneColor
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

	//======================================================
	// Volumetric Fog : froxel ボリューム構築 (ComputeVolumetricFog)
	//  シャドウマップ (RenderShadowDepths 済み) とライトグリッド
	//  (直前の Dispatch) を読んで、カメラから各 froxel までの
	//  累積インスキャッタ / 透過率 (t34) を作る。シーン深度には
	//  依存しない。フォグパス (デファード直後) と半透明が参照する。
	//  bEnableVolumetricFog = false / フォグ不在なら何もしない。
	//======================================================
	if (m_FogRenderer)
	{
		FFogSceneRenderer::FComputeInputs fogInputs;
		fogInputs.View = &m_ViewConstant;
		fogInputs.ForwardLightData = &m_ForwardLightConstant;
		// froxel グリッドはジッタ無し (_11/_22 のみ) なので前フレームの NoAA の VP を使う
		// (UE UnjitteredPrevWorldToClip)。有効判定は Lumen と同じ規則 (§4.9)。
		// フォグ自身の m_bHistoryValid (ボリューム再作成でクリア) とはフォグ内部で AND される
		XMStoreFloat4x4(&fogInputs.PrevViewProjectionT,
			XMMatrixTranspose(XMLoadFloat4x4(&m_ViewInfo.PrevViewInfo.ViewMatrices.ViewProjectionNoAAMatrix)));
		fogInputs.bHistoryValid = IsScreenHistoryValid();
		m_TAAStats.bFogHistoryValid = fogInputs.bHistoryValid;	// 統計 / テストドライバのログ用
		fogInputs.LightBufferSRVIndex = m_LightBufferSRVIndex[m_LightBufferFrame];
		fogInputs.LightGrid = m_LightGrid.get();
		fogInputs.ShadowRenderer = m_ShadowRenderer.get();
		fogInputs.SkyIrradianceSRVIndex = m_IBLBaker ? m_IBLBaker->GetIrradianceSRVIndex() : 0;
		fogInputs.DirectionalLight = m_FrameDirectionalLight;
		m_FogRenderer->ComputeVolumetricFog(fogInputs);
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

		// フォワードライティング入力 (IBL t6-t8 / ローカルライト t13 /
		// ライトグリッド t19-t20 / シャドウ b5 + t14-t18)
		BindForwardLightingResources();

		// Lumen スクリーン GI (b6: LUMEN 定数 / t24: オブジェクト /
		// t25: カード / t26: FinalLighting / t27: 深度アトラス)。
		// SDF アトラス (t17) はシャドウ側 BindShadowResources が
		// バインド済みのものを共用する。
		if (m_LumenScene)
		{
			UploadLumenConstant();

			m_LumenScene->BindLumenResources();
		}

		DrawScreenPass();
	}

	//======================================================
	// Exponential Height Fog Pass (RenderFog)
	//  SceneColor が RENDER_TARGET のまま、深度から再構築した
	//  ワールド座標で高さフォグ (+ Volumetric Fog の積分結果) を
	//  One / SrcAlpha ブレンドで合成する。b0 はデファードパスの
	//  バインドをそのまま使う。フォグ不在なら何もしない。
	//======================================================
	if (m_FogRenderer)
	{
		m_FogRenderer->RenderFog(m_ScreenQuad.get(), m_SceneTextures.DepthSRVIndex);
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
//    - 深度: Translucent はプリミティブごとに深度プリパス (DepthWrite,
//      カラー無効) -> EQUAL 着色 (DepthReadEqual)。Additive は DepthRead の 1 パス
//    - 描画順は TranslucencySortPriority 昇順 -> m_TranslucencyParams.SortPolicy
//      (ETranslucentSortPolicy) の奥行きキーで後→前
//    - ブレンドは PSO 側: Translucent = SrcAlpha/InvSrcAlpha,
//      Additive = SrcAlpha/One (Two Sided はカリング無効バリアント)
//  トランスルーセントプリミティブが 1 つも無いフレームは
//  バリアも発行せず早期リターンする。
// ============================================================
void FSceneRenderer::RenderTranslucency(FScene* Scene)
{
	if (Scene == nullptr) return;

	// レンダー解像度 R のビューポート (直前のパスが何を設定していても R で描く)
	ApplyRenderViewport();

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
	// 深度プリパス (DepthWrite) が Translucent の最前面深度を深度バッファへ
	// 書き込むため、このパス以降の深度バッファは不透明のみの深度ではなくなる
	// (後段が t3 を読めば半透明の最前面深度が見える。LinearDepth (t4) は
	// 不透明のみのまま)。
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
		UploadLumenConstant();

		m_LumenScene->BindTranslucencyResources();
	}

	// ---- b4 (PostProcess): 屈折 UV 計算が SceneTexelSize を参照 ----
	// レンダー解像度 R のテクセルサイズ (ResolvePostProcessSettings が設定) を持つ通常の内容をそのまま積む。
	UploadPostProcessConstant();

	// ---- 屈折入力: シーンカラーコピー (t21) + 線形深度 (t4) ----
	// t4 は深度棄却 (ResolveRefractedSceneUV) が読む。LinearDepth は
	// 深度バッファ (DSV) とは別リソースなので SRV のまま参照できる。
	m_RHI->SetTexture(RenderManager::TEXTURE_TYPE::SCENE_COLOR_COPY, m_SceneTextures.SceneColorCopy.get());
	m_RHI->SetTexture(RenderManager::TEXTURE_TYPE::LINEAR_DEPTH, m_SceneTextures.LinearDepth.get());

	// ---- フォワードライティング入力 (デファードパスと同一セット) ----
	BindForwardLightingResources();

	// ---- フォグ (b7: FOG 定数 / t33: キューブマップ / t34: Volumetric Fog) ----
	// TranslucentPS がサーフェス位置で高さフォグ + Volumetric Fog を
	// 直接評価して合成する (BasePassPixelShader の Fogging 相当)
	if (m_FogRenderer)
	{
		m_FogRenderer->BindFogResources();
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

	// ---- Responsive AA マスク (§4.6) ----
	// 半透明深度プリパスの深度 (DEPTH_WRITE) を DSV にバインドしたまま、同じ後→前順で描く。
	// 終わると OM を SceneColor + DSV に戻す (以降の最終バリアはそのまま)
	{
		std::vector<const FPrimitiveSceneProxy*> sortedProxies;
		sortedProxies.reserve(sortedPrims.size());
		for (const FTranslucentSortEntry& entry : sortedPrims)
		{
			sortedProxies.push_back(primitives[entry.Index].Proxy.get());
		}
		RenderResponsiveAAMask(sortedProxies);
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
//  RenderResponsiveAAMask (§4.6)
//  UE は bEnableResponsiveAA のマテリアルの半透明描画でステンシル bit 3
//  (STENCIL_TEMPORAL_RESPONSIVE_AA) を立て、TAA の Responsive パスがその画素の
//  現フレーム重みを上げる。本エンジンの深度バッファは D32_FLOAT (ステンシル無し) なので、
//  レンダー解像度の R8_UNORM マスクへ描き、TAA (t5) が入力画素 K で読む [PORT]。
//    - RenderTranslucency の最後 (半透明深度プリパスの深度を DSV にバインドしたまま、
//      SceneColor / 深度の最終バリアの前) に呼ばれる
//    - VS / b0 / b1 は半透明の描画と同じなので LESS_EQUAL がビット一致で通り、
//      最前面の Translucent 層 (+ その手前の Additive) がマスクされる (UE のステンシルと同じ被覆)
//    - m_bResponsiveMaskValid の正規のリセットは BeginFrame 先頭 (RenderTranslucency は
//      半透明が無いフレームにここを呼ばずに戻る。再確保直後のマスクは未クリア)
// ============================================================
void FSceneRenderer::RenderResponsiveAAMask(const std::vector<const FPrimitiveSceneProxy*>& SortedTranslucent)
{
	m_bResponsiveMaskValid = false;		// 念のため (正規のリセットは BeginFrame 先頭)
	if (!m_ViewFamily.bTemporalAA)
		return;

	// Responsive の半透明サブセットが 1 つも無ければマスクに触れない (TAA はダミーを読みフラグを立てない)
	const bool bForce = m_TAADebug.bForceResponsiveAA;
	bool bAny = false;
	for (const FPrimitiveSceneProxy* proxy : SortedTranslucent)
	{
		bAny |= (proxy != nullptr) && proxy->HasResponsiveAATranslucency(bForce);
	}
	if (!bAny)
		return;

	ID3D12GraphicsCommandList* cl = m_RHI->GetGraphicsCommandList();
	RENDER_TARGET* mask = m_SceneTextures.ResponsiveAAMask.get();

	// 読み取り (PIXEL | NON_PIXEL) -> RENDER_TARGET、0 でクリア (最適化クリア値 (0,0,0,1) と一致 = #820 無し)
	TransitionReadToRenderTarget(cl, mask);
	const D3D12_CPU_DESCRIPTOR_HANDLE dsv = m_RHI->GetDepthStencilViewHandle();	// 半透明プリパス深度を含む DEPTH_WRITE (テストのみ)
	cl->OMSetRenderTargets(1, &mask->RTVHandle, FALSE, &dsv);
	const FLOAT clear[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
	cl->ClearRenderTargetView(mask->RTVHandle, clear, 0, nullptr);

	// 後→前順 (被覆は深度テストで決まるので順序は結果に影響しない)。PSO は ResponsiveAA[TwoSided]
	for (const FPrimitiveSceneProxy* proxy : SortedTranslucent)
	{
		if (proxy != nullptr)
		{
			proxy->DrawResponsiveAA(m_RHI, bForce);
		}
	}

	// RENDER_TARGET -> 読み取り (TAA t5 / ImGui サムネイル)。OM は RenderTranslucency の続きが期待する形へ戻す
	TransitionRenderTargetToRead(cl, mask);
	cl->OMSetRenderTargets(1, &m_SceneTextures.SceneColor->RTVHandle, TRUE, &dsv);
	m_bResponsiveMaskValid = true;
}


// ============================================================
//  Post processing (§4.7):
//    SceneColor 履歴 (Lumen) -> DOF (R) -> Temporal AA (R -> H) -> AutoExposure -> Bloom -> LUT
//    -> Tonemap (ポスト入力サイズ) [-> 一次空間アップスケール -> O、またはトーンマップ統合]
//    [-> 入出力分割表示 / デバッグ表示] -> スクリーンショット -> 深度を DEPTH_WRITE へ -> CommitViewState
//  後段のサイズは計画値 (m_ViewFamily.PostProcessExtent) ではなく「次のパスが実際に
//  読むテクスチャ」の実寸から取る (§0.1 decision 12)。計画値は Bloom チェーンの確保にだけ使う
// ============================================================
void FSceneRenderer::RenderPostProcessing()
{
	ID3D12GraphicsCommandList* cl = m_RHI->GetGraphicsCommandList();
	const XMUINT2 O = m_ViewFamily.OutputExtent;
	const XMUINT2 R = m_ViewFamily.RenderExtent;

	// AutoExposure 結果バッファ: COMMON (コマンドリスト実行ごとのバッファ減衰) ->
	// 読み取り (PIXEL | NON_PIXEL)。前フレームの露出を読む全パスより前に行う
	if (m_AutoExposure) { m_AutoExposure->PrepareResultForRead(); }

	//======================================================
	// Lumen 用シーンカラー履歴の確定 (SceneColor が加工される前)
	//======================================================
	CopySceneColorHistory();

	//======================================================
	// Depth of Field (Gaussian, レンダー解像度 R). SceneColor in/out SRV.
	//  Bloom の入力にもなるよう、AutoExposure/Bloom より前で合成する。
	//======================================================
	if (m_FinalSettings.Flags & PP_FLAG_DOF)
	{
		RenderDOF();
	}

	//======================================================
	// Temporal upscaler (UE: DOF の後, 目の順応 / Bloom の前。§4.7 / §4.8)
	//  AA 無し / TAA 不実行 (カメラ不在, PSO 欠落) では SceneColor (R) をそのまま後段へ渡す。
	//  後段のサイズは TAA が実際に出力したテクスチャから取る
	//======================================================
	RENDER_TARGET* postInput = m_SceneTextures.SceneColor.get();
	XMUINT2 postExtent = R;
	RENDER_TARGET* halfRes = nullptr;
	XMUINT2 halfExtent{};
	FTemporalAAHistory outHistory;
	bool bTAARan = false;
	bool bHistoryValid = false;

	if (m_ViewFamily.bTemporalAA && m_ViewInfo.bValid && m_TemporalUpscaler)
	{
		ITemporalUpscaler::FPassInputs in;
		in.bAllowDownsampleSceneColor = m_ViewFamily.bTAADownsample;
		in.SceneColorTexture = m_SceneTextures.SceneColor.get();
		in.SceneDepthSRVIndex = m_SceneTextures.LinearDepth->SRVIndex;			// 不透明のみ (§0.1 decision 9)
		in.SceneVelocitySRVIndex = m_bVelocityValid ? m_SceneTextures.Velocity->SRVIndex : m_TemporalUpscaler->GetDummySRVIndex();
		in.bResponsiveMaskValid = m_bResponsiveMaskValid;
		in.ResponsiveMaskSRVIndex = m_bResponsiveMaskValid ? m_SceneTextures.ResponsiveAAMask->SRVIndex : m_TemporalUpscaler->GetDummySRVIndex();
		in.bUseEyeAdaptationBuffer = ((m_FinalSettings.Flags & PP_FLAG_AUTO_EXPOSURE) != 0) && m_AutoExposure && m_AutoExposure->IsResultValid();
		in.EyeAdaptationSRVIndex = in.bUseEyeAdaptationBuffer ? m_AutoExposure->GetExposureSRVIndex() : m_TemporalUpscaler->GetDummyEyeAdaptationSRVIndex();
		in.ManualExposure = m_FinalSettings.Exposure;								// = 2^EV (手動露出)
		in.bForceHistoryBypass = IsPreTAADebugViewActive();						// Lumen / LightGrid DebugMode 中は履歴を使わない

		const ITemporalUpscaler::FPassOutputs out =
			m_TemporalUpscaler->AddPasses(m_ViewInfo, m_ViewFamily, m_ViewState, m_AAParams, m_TAADebug, in);
		if (out.SceneColor)
		{
			postInput = out.SceneColor;
			postExtent = out.SceneColorExtent;
			halfRes = out.HalfResSceneColor;
			halfExtent = out.HalfResExtent;
			outHistory = out.NewHistory;
			bHistoryValid = out.bHistoryValid;
			bTAARan = true;
		}
	}

	// AA 無し (または PSO 欠落のフォールバック) の間は TAA の履歴ピンポンと補助出力を解放する
	// (~RENDER_TARGET の遅延解放。フレーム途中でも安全)。カメラ不在のフレーム (TAA 有効のまま
	// 走らなかった) は保持して再確保を避ける。履歴は CommitViewState が無効を確定するので参照は残らない
	if (!m_ViewFamily.bTemporalAA && m_TemporalUpscaler)
	{
		m_ViewState.TemporalAAHistoryPool[0].Release();
		m_ViewState.TemporalAAHistoryPool[1].Release();
		m_TemporalUpscaler->ReleaseAuxiliaryTargets();
	}

	m_TAAStats.bTAARanThisFrame = bTAARan;
	m_TAAStats.bHistoryValidThisFrame = bHistoryValid;
	m_TAAStats.HistoryExtent = bTAARan ? outHistory.ReferenceBufferSize : XMUINT2{ 0u, 0u };
	m_TAAStats.HistoryFormat = bTAARan ? outHistory.Format : DXGI_FORMAT_UNKNOWN;
	m_TAAStats.PostExtent = postExtent;

	//======================================================
	// Auto Exposure: histogram + adapt (入力 = TAA 出力 or ハーフ解像度。入口 PSR 契約)
	//======================================================
	if (m_AutoExposure)
	{
		RENDER_TARGET* aeInput = halfRes ? halfRes : postInput;
		const XMUINT2 aeExtent = halfRes ? halfExtent : postExtent;
		m_AutoExposure->Dispatch(
			aeInput->Resource.Get(),
			aeInput->SRVIndex,
			aeExtent.x,
			aeExtent.y,
			Time::GetDeltaTime());
	}

	//======================================================
	// Bloom: bright-pass -> down/up chain (-> m_BloomUp[0])
	//  チェーンは O/2 固定。入力は UV で読み、しきい値パスの 4 タップボックスが前置フィルタする
	//======================================================
	if (m_FinalSettings.Flags & PP_FLAG_BLOOM)
	{
		RenderBloom(halfRes ? halfRes : postInput, postExtent);
	}

	// Re-bake the color grading LUT if any grading setting changed this
	// frame (compute dispatch; cheap no-op when unchanged).
	if (m_ColorGradingLUTBaker)
	{
		m_ColorGradingLUTBaker->UpdateIfDirty(m_FinalSettings);
	}

	//======================================================
	// Tonemap Pass (+ 一次空間アップスケール, §4.7 / §6.7)
	//  post chain: CA, bloom, exposure, white balance, ACES, grading, vignette, grain, sRGB
	//  ・ポスト入力 = O            : バックバッファへ直接トーンマップ (従来どおり)
	//  ・ポスト入力 != O かつ統合   : バックバッファへ直接トーンマップ。t0 の UV サンプリング
	//                                (s1 バイリニア) が拡大を兼ねる (UE: 統合時は常にバイリニア)
	//  ・ポスト入力 != O かつ非統合 : ポスト入力サイズの m_TonemapOutput (RGBA8) へトーンマップ ->
	//                                AddPrimaryUpscalePass (r.Upscale.Quality) でバックバッファへ
	//  アップスケール PSO が欠落していれば Bilinear、それも無ければ統合経路 (黒画面にしない)
	//======================================================
	const bool bNeedsUpscale = (postExtent.x != O.x || postExtent.y != O.y);
	const char* upscalePipeline = bNeedsUpscale ? SelectPrimaryUpscalePipeline() : nullptr;
	const bool bMerge = bNeedsUpscale &&
		(ShouldMergeTonemapWithUpscale(m_AAParams, postExtent, O) || upscalePipeline == nullptr);

	// b4: テクセルサイズ = 1 / ポスト入力サイズ (TonemapPS 自体は読まない。§4.1 テクセル規則)
	SetTexelSize((int)postExtent.x, (int)postExtent.y);
	UploadPostProcessConstant();

	// バックバッファ: PRESENT -> RENDER_TARGET
	// (スワップチェーンバッファは PRESENT(COMMON) 開始なので、書き込み前に
	//  RENDER_TARGET へ遷移し、EndFrame の Present 直前に戻す)
	TransitionBackBuffer(cl, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);

	const D3D12_CPU_DESCRIPTOR_HANDLE backBufferRTV = m_RHI->GetCurrentBackBufferRTV();
	const FLOAT clearColor[4] = { 0.0f, 0.0f, 0.0f, 1.0f };

	if (!bNeedsUpscale || bMerge)
	{
		cl->OMSetRenderTargets(1, &backBufferRTV, TRUE, nullptr);
		cl->ClearRenderTargetView(backBufferRTV, clearColor, 0, nullptr);
		SetViewportAndScissor(cl, (int)O.x, (int)O.y);
		DrawTonemap(postInput);
	}
	else
	{
		// ポスト入力サイズの LDR へトーンマップ (RGBA8, 実サイズ不一致時のみ再確保。常駐 PSR)
		EnsureTonemapOutput(postExtent);
		TransitionToRenderTarget(cl, m_TonemapOutput.get());
		cl->OMSetRenderTargets(1, &m_TonemapOutput->RTVHandle, TRUE, nullptr);
		cl->ClearRenderTargetView(m_TonemapOutput->RTVHandle, clearColor, 0, nullptr);
		SetViewportAndScissor(cl, (int)postExtent.x, (int)postExtent.y);
		DrawTonemap(postInput);
		TransitionToShaderResource(cl, m_TonemapOutput.get());

		// 一次空間アップスケール: m_TonemapOutput -> バックバッファ (ビューポート O)
		cl->OMSetRenderTargets(1, &backBufferRTV, TRUE, nullptr);
		cl->ClearRenderTargetView(backBufferRTV, clearColor, 0, nullptr);
		AddPrimaryUpscalePass(m_TonemapOutput.get(), postExtent, O, upscalePipeline);
	}
	m_TAAStats.bUpscaleMerged = bMerge;
	m_TAAStats.UpscalePipeline = (bNeedsUpscale && !bMerge) ? upscalePipeline : nullptr;

	//======================================================
	// 入出力分割表示 (ETemporalAADebugView::InputOutputSplit)
	//  左半分に TAA 入力 (ジッタ込み SceneColor, レンダー解像度) を同じトーンマップで描き直す
	//  (Bloom / LUT / 露出は共通 = 厳密比較)。右半分は TAA 出力のまま。分割線は可視化パスが描く
	//======================================================
	if (m_TAADebug.DebugView == ETemporalAADebugView::InputOutputSplit && bTAARan)
	{
		const D3D12_RECT left = { 0, 0, (LONG)(O.x / 2u), (LONG)O.y };
		SetViewportAndScissor(cl, (int)O.x, (int)O.y);
		cl->RSSetScissorRects(1, &left);
		DrawTonemap(m_SceneTextures.SceneColor.get());
		SetViewportAndScissor(cl, (int)O.x, (int)O.y);
	}

	//======================================================
	// Temporal AA デバッグ表示 (§6.8。バックバッファ, ビューポート O。UI 描画前なのでスクリーンショットに写る)
	//======================================================
	if (m_TAADebug.DebugView != ETemporalAADebugView::Off)
	{
		AddVisualizeTemporalAAPass(postInput, bTAARan);
	}

	//======================================================
	// スクリーンショット (UI 描画前のバックバッファ)
	//  RT -> COPY_SOURCE -> READBACK コピー -> RT。書き出しは次フレームの
	//  BeginFrame 先頭 (ResolvePending) か FlushScreenshots で行う。
	//======================================================
	if (m_Screenshot->IsRequested())
	{
		m_Screenshot->RecordCopy(cl, m_RHI->GetCurrentBackBufferResource(), O);

		// バックバッファ RTV を再バインド (EndFrame の ImGui 描画先)
		cl->OMSetRenderTargets(1, &backBufferRTV, TRUE, nullptr);
	}

	//======================================================
	// 深度バッファ: SRV -> DEPTH_WRITE に戻す
	//======================================================
	cl->ResourceBarrier(1,
		&CD3DX12_RESOURCE_BARRIER::Transition(
			m_RHI->GetDepthBufferResource(),
			D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
			D3D12_RESOURCE_STATE_DEPTH_WRITE));

	//======================================================
	// 前フレーム情報の確定 (フレームの最後。§4.9)
	//  フレーム中の全コンシューマは m_ViewInfo.PrevViewInfo (先頭の
	//  スナップショット) を読み終えている。TAA 未実行のフレームは
	//  無効な履歴を確定する (次の TAA はカット扱いになる)
	//======================================================
	CommitViewState(bTAARan ? outHistory : FTemporalAAHistory{});
}


// ============================================================
//  Tonemap / 一次空間アップスケールの補助 (§4.7)
// ============================================================

// 既存のトーンマップ描画。t0 = Input (HDR)。RTV / ビューポートは呼び出し側が設定する
void FSceneRenderer::DrawTonemap(RENDER_TARGET* Input)
{
	m_RHI->SetPipelineState("PostProcessTonemap");
	m_RHI->SetTexture(RenderManager::TEXTURE_TYPE::BASE_COLOR, Input); // t0
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


// アップスケール入力 (トーンマップ済み LDR) を Extent で用意する。
// null かサイズ違いの時だけ作り直す (旧ターゲットは ~RENDER_TARGET の遅延解放)。初期状態 PSR
void FSceneRenderer::EnsureTonemapOutput(XMUINT2 Extent)
{
	if (m_TonemapOutput && m_TonemapOutput->Width == Extent.x && m_TonemapOutput->Height == Extent.y)
	{
		return;
	}
	m_TonemapOutput = m_RHI->CreateRenderTarget(Extent.x, Extent.y, DXGI_FORMAT_R8G8B8A8_UNORM);
	m_TonemapOutput->Resource->SetName(L"TonemapOutput");
}


void FSceneRenderer::TransitionBackBuffer(ID3D12GraphicsCommandList* CommandList, D3D12_RESOURCE_STATES Before, D3D12_RESOURCE_STATES After)
{
	CommandList->ResourceBarrier(1,
		&CD3DX12_RESOURCE_BARRIER::Transition(m_RHI->GetCurrentBackBufferResource(), Before, After));
}


// ============================================================
//  Temporal AA デバッグ表示 (§6.8, UE VisualizeMotionVectors / VisualizeTemporalUpscaler)
// ============================================================

void FSceneRenderer::AddVisualizeTemporalAAPass(RENDER_TARGET* PostInput, bool bTAARan)
{
	const ETemporalAADebugView view = m_TAADebug.DebugView;
	if (view <= ETemporalAADebugView::Off || view >= ETemporalAADebugView::Count)
	{
		return;		// 範囲外 (呼び出し側は Off を除外済み)
	}

	// CS のデバッグ出力 (5..13) は TAA がこのフレームに書いた時だけ表示する
	// (AA 無し / カメラ不在 / PSO 欠落のフレームは通常画像のまま。古い内容を見せない)
	FTAATexture* debugOutput = nullptr;
	if (IsTemporalAADebugViewFromCS(view))
	{
		debugOutput = (bTAARan && m_TemporalUpscaler) ? m_TemporalUpscaler->GetDebugOutput() : nullptr;
		if (debugOutput == nullptr)
		{
			return;
		}
	}

	if (!m_RHI->HasPipelineState("VisualizeTemporalAA"))
	{
		if (!m_bLoggedMissingVisualizePSO)
		{
			m_bLoggedMissingVisualizePSO = true;
			OutputDebugStringA("[TemporalAA] missing PSO VisualizeTemporalAA (VisualizeTemporalAAPS.cso) -> debug view skipped\n");
		}
		return;
	}

	ID3D12GraphicsCommandList* cl = m_RHI->GetGraphicsCommandList();
	const XMUINT2 O = m_ViewFamily.OutputExtent;
	const unsigned int dummySRV = m_TemporalUpscaler->GetDummySRVIndex();			// 1x1 (0,0,0,1), RD 常駐

	// b0: カメラ (ClipToPrevClip / NearFar / ViewSizeAndInvSize)。ポスト中に上書きされていても戻す
	m_RHI->SetConstant(RenderManager::CONSTANT_TYPE::VIEW, &m_ViewConstant, sizeof(m_ViewConstant));

	// b4: モード / 増幅 / テクセルサイズ = 1/O (§3.3。レンダラ専有フィールド)
	m_FinalSettings.VisualizeMode = (unsigned int)view;
	m_FinalSettings.VisualizeScale = m_TAADebug.VisualizeScale;
	SetTexelSize((int)O.x, (int)O.y);
	UploadPostProcessConstant();

	m_RHI->SetPipelineState("VisualizeTemporalAA");
	m_RHI->SetTexture(RenderManager::TEXTURE_TYPE::BASE_COLOR, m_SceneTextures.SceneColor.get());		// t0: レンダー解像度 SceneColor (PSR)
	m_RHI->SetTexture(RenderManager::TEXTURE_TYPE::LINEAR_DEPTH, m_SceneTextures.LinearDepth.get());	// t4: LinearDepth (RD)
	if (m_AutoExposure)
	{
		m_RHI->BindRootTableBySRVIndex((unsigned int)RenderManager::TEXTURE_TYPE::AUTO_EXPOSURE, m_AutoExposure->GetExposureSRVIndex());	// t11
	}
	// t35: 今フレーム描いたベロシティ、描いていなければダミー (RG = 0 = 未書き込み -> カメラモーション)
	m_RHI->BindRootTableBySRVIndex((unsigned int)RenderManager::TEXTURE_TYPE::VELOCITY,
		m_bVelocityValid ? m_SceneTextures.Velocity->SRVIndex : dummySRV);
	// t36: TAA DebugOutput (モード 5..13。RD) / 後段の入力 = TAA 出力 (モード 4 TemporalUpscalerIO。
	//      Main / MainUpsampling は履歴 H、MainSuperSampling は Mitchell-Netravali 出力 S。いずれも PSR。
	//      TAA が走らなかったフレームは SceneColor (R, PSR) = 入力と同じ) / それ以外はダミー
	unsigned int t36 = dummySRV;
	if (debugOutput)
	{
		t36 = debugOutput->RT->SRVIndex;
	}
	else if (view == ETemporalAADebugView::TemporalUpscalerIO && PostInput)
	{
		t36 = PostInput->SRVIndex;
	}
	m_RHI->BindRootTableBySRVIndex((unsigned int)RenderManager::TEXTURE_TYPE::TEMPORAL_AA_DEBUG, t36);

	SetViewportAndScissor(cl, (int)O.x, (int)O.y);
	DrawScreenPass();
}


// ============================================================
//  Frame end: ImGui render -> back buffer to PRESENT -> Present
// ============================================================
void FSceneRenderer::EndFrame()
{
	ID3D12GraphicsCommandList* cl = m_RHI->GetGraphicsCommandList();

	// ---- ImGui の描画先を再バインド ----
	// フレーム途中の FlushAndResetCommandList (ImGui からのアセットロード /
	// 自己テスト等) はコマンドリストの Reset で RTV のバインドを失う
	// (ヒープ / ビューポートは既定値 = レンダー解像度に戻るだけ)。また
	// ポストプロセス後のビューポートはポスト解像度のままの場合がある。
	// ImGui はバックバッファ全体 (出力解像度 O) へ描くため、ここで
	// SRV ヒープ / バックバッファ RTV / ビューポート O を明示的に設定し直す。
	{
		ID3D12DescriptorHeap* heaps[] = { m_RHI->GetSRVDescriptorHeap() };
		cl->SetDescriptorHeaps(_countof(heaps), heaps);

		const D3D12_CPU_DESCRIPTOR_HANDLE backBufferRTV = m_RHI->GetCurrentBackBufferRTV();
		cl->OMSetRenderTargets(1, &backBufferRTV, TRUE, nullptr);

		SetViewportAndScissor(cl, m_RHI->GetBackBufferWidth(), m_RHI->GetBackBufferHeight());
	}

	// ImGui (バックバッファは RenderPostProcessing で RENDER_TARGET 済み)
	{
		ImGui::Render();
		ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), cl);
	}

	// バックバッファ: RENDER_TARGET -> PRESENT
	TransitionBackBuffer(cl, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);

	// コマンド発行 + Present + 前フレーム待ち + Reset
	m_RHI->Present();
}


// ============================================================
//  Screenshot: 要求 / 終了前の書き出し / F9 用自動パス
// ============================================================
void FSceneRenderer::RequestScreenshot(const std::string& Path)
{
	m_Screenshot->Request(Path.empty() ? MakeAutoScreenshotPath() : Path);
}

void FSceneRenderer::FlushScreenshots()
{
	m_Screenshot->FlushScreenshots();
}

std::string FSceneRenderer::MakeAutoScreenshotPath() const
{
	// <frame> = キャプチャされるフレーム。要求は Update (F9) か ImGui 構築中に来るので、
	// 実際に描かれるのは次の ImGui::NewFrame (BeginFrame) のフレーム = 現在値 + 1。
	// SP / AA 手法 / パス / 品質は直近のビューファミリ (実効値) から書く
	// (TAA が走らない構成は pass = None, Q = -)。
	const FViewFamilyInfo& F = m_ViewFamily;
	const int sp = (int)std::lround(F.ResolutionFraction * 100.0f);
	char quality[8] = "-";
	if (F.bTemporalAA)
	{
		sprintf_s(quality, "%d", (int)F.TAAQuality);
	}
	// 先頭にローカル時刻 (セッションを跨いでも名前が重ならず、以前のキャプチャを上書きしない)
	char stamp[32] = "";
	const std::time_t now = std::time(nullptr);
	std::tm lt{};
	if (localtime_s(&lt, &now) == 0)
	{
		std::strftime(stamp, sizeof(stamp), "%Y%m%d_%H%M%S_", &lt);
	}
	char path[256];
	sprintf_s(path, "Saved/Screenshots/%s%d_%d_%s_%s_Q%s.bmp",
		stamp, ImGui::GetFrameCount() + 1, sp,
		GetAntiAliasingMethodName(F.AntiAliasingMethod),
		F.bTemporalAA ? GetTAAPassConfigName(F.TAAPass) : "None",
		quality);
	return path;
}


void FSceneRenderer::DrawScreenPass()
{
	m_RHI->SetVertexBuffer(m_ScreenQuad.get());
	m_RHI->GetGraphicsCommandList()->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
	m_RHI->GetGraphicsCommandList()->DrawInstanced(4, 1, 0, 0);
}


// デファードパス / トランスルーセンシーパス共通のフォワードライティング入力
void FSceneRenderer::BindForwardLightingResources()
{
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
}


// LUMEN 定数 (b6) を解決してアップロードする (m_LumenScene 非 null 前提)
void FSceneRenderer::UploadLumenConstant()
{
	LUMEN_CONSTANT lumenConstant{};
	m_LumenScene->FillLumenConstant(lumenConstant);
	m_RHI->SetConstant(RenderManager::CONSTANT_TYPE::LUMEN,
		&lumenConstant, sizeof(lumenConstant));
}


// ============================================================
//  Gaussian Depth of Field.
//   1. CoC/prep : SceneColor -> half-res (RGB=premul color, A=CoC)
//   2. Blur H   : DOFPrep -> DOFPing  (separable, radius = CoC)
//   3. Blur V   : DOFPing -> DOFBlur  (separable, radius = CoC)
//   4. Composite: sharp SceneColor + DOFBlur -> SceneColor
//  SceneColor must be in PIXEL_SHADER_RESOURCE on entry and is left
//  in PIXEL_SHADER_RESOURCE on exit.
//  レンダー解像度 R で走る (ハーフ解像度 = ((R.x+1)/2, (R.y+1)/2))。
//  ボケ半径 = CoC x MaxBlurSize [ハーフ解像度テクセル] (DOFBlurPS.hlsl) なので、
//  MaxBlurSize を R.x/O.x 倍して出力画素換算の半径をスクリーンパーセンテージに
//  依らず一定にする (§5.3。100 % では 1 倍 = 従来と同一)。終了時に元へ戻す。
// ============================================================
void FSceneRenderer::RenderDOF()
{
	ID3D12GraphicsCommandList* cl = m_RHI->GetGraphicsCommandList();

	const FLOAT clr[4] = { 0, 0, 0, 0 };

	const XMUINT2 O = m_ViewFamily.OutputExtent;
	const XMUINT2 R = m_ViewFamily.RenderExtent;

	// ボケ半径のスクリーンパーセンテージ不変化 (レンダラ専有コピーのみ変更。最後に復元)
	const float savedMaxBlurSize = m_FinalSettings.MaxBlurSize;
	m_FinalSettings.MaxBlurSize *= (float)R.x / (float)O.x;

	// 線形深度は RenderLighting 側で既に読み取り状態 (PIXEL | NON_PIXEL)。CoC 計算で t4 を読む。
	// SceneColor も入口で PIXEL_SHADER_RESOURCE。

	// ---- 1. CoC / prep : SceneColor(t0) + LinearDepth(t4) -> DOFPrep ----
	// ハーフ解像度テクセルサイズを渡す (ブラー内で使用)。
	SetTexelSize(m_DOFWidth, m_DOFHeight);
	UploadPostProcessConstant();

	TransitionToRenderTarget(cl, m_DOFPrep.get());
	cl->OMSetRenderTargets(1, &m_DOFPrep->RTVHandle, TRUE, nullptr);
	cl->ClearRenderTargetView(m_DOFPrep->RTVHandle, clr, 0, nullptr);
	SetViewportAndScissor(cl, m_DOFWidth, m_DOFHeight);
	m_RHI->SetPipelineState("PostProcessDOFCoC");
	m_RHI->SetTexture(RenderManager::TEXTURE_TYPE::BASE_COLOR, m_SceneTextures.SceneColor.get()); // t0
	m_RHI->SetTexture(RenderManager::TEXTURE_TYPE::LINEAR_DEPTH, m_SceneTextures.LinearDepth.get()); // t4
	DrawScreenPass();
	TransitionToShaderResource(cl, m_DOFPrep.get());

	// ---- 2. Horizontal blur : DOFPrep -> DOFPing (DofPad=0 => 水平) ----
	m_FinalSettings.DofPad = 0.0f;
	UploadPostProcessConstant();
	TransitionToRenderTarget(cl, m_DOFPing.get());
	cl->OMSetRenderTargets(1, &m_DOFPing->RTVHandle, TRUE, nullptr);
	cl->ClearRenderTargetView(m_DOFPing->RTVHandle, clr, 0, nullptr);
	SetViewportAndScissor(cl, m_DOFWidth, m_DOFHeight);
	m_RHI->SetPipelineState("PostProcessDOFBlur");
	m_RHI->SetTexture(RenderManager::TEXTURE_TYPE::BASE_COLOR, m_DOFPrep.get()); // t0
	DrawScreenPass();
	TransitionToShaderResource(cl, m_DOFPing.get());

	// ---- 3. Vertical blur : DOFPing -> DOFBlur (DofPad=1 => 垂直) ----
	m_FinalSettings.DofPad = 1.0f;
	UploadPostProcessConstant();
	TransitionToRenderTarget(cl, m_DOFBlur.get());
	cl->OMSetRenderTargets(1, &m_DOFBlur->RTVHandle, TRUE, nullptr);
	cl->ClearRenderTargetView(m_DOFBlur->RTVHandle, clr, 0, nullptr);
	SetViewportAndScissor(cl, m_DOFWidth, m_DOFHeight);
	m_RHI->SetPipelineState("PostProcessDOFBlur");
	m_RHI->SetTexture(RenderManager::TEXTURE_TYPE::BASE_COLOR, m_DOFPing.get()); // t0
	DrawScreenPass();
	TransitionToShaderResource(cl, m_DOFBlur.get());
	m_FinalSettings.DofPad = 0.0f;
	UploadPostProcessConstant();

	// ---- 4. Composite : sharp(t0) + DOFBlur(t12) -> DOFSharp(full-res) ----
	//  SceneColor 自身を read/write 同時参照できないため、合成結果は一旦
	//  full-res の DOFSharp に書き、その後 CopyResource で SceneColor に戻す。
	TransitionToRenderTarget(cl, m_DOFSharp.get());
	cl->OMSetRenderTargets(1, &m_DOFSharp->RTVHandle, TRUE, nullptr);
	cl->ClearRenderTargetView(m_DOFSharp->RTVHandle, clr, 0, nullptr);
	SetViewportAndScissor(cl, (int)R.x, (int)R.y);
	m_RHI->SetPipelineState("PostProcessDOFComposite");
	m_RHI->SetTexture(RenderManager::TEXTURE_TYPE::BASE_COLOR, m_SceneTextures.SceneColor.get()); // t0 sharp
	m_RHI->SetTexture(RenderManager::TEXTURE_TYPE::LINEAR_DEPTH, m_SceneTextures.LinearDepth.get()); // t4
	m_RHI->BindRootTableBySRVIndex((unsigned int)RenderManager::TEXTURE_TYPE::DOF, m_DOFBlur->SRVIndex); // t12
	DrawScreenPass();
	TransitionToShaderResource(cl, m_DOFSharp.get());

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

	// レンダー解像度 R のビューポート / テクセルサイズ、元の MaxBlurSize を復元。
	m_FinalSettings.MaxBlurSize = savedMaxBlurSize;
	SetViewportAndScissor(cl, (int)R.x, (int)R.y);
	SetTexelSize((int)R.x, (int)R.y);
	UploadPostProcessConstant();
}


// ============================================================
//  Bloom: bright-pass -> downsample chain -> upsample chain.
//  Reads Input (the actual post-process input: SceneColor at R without TAA;
//  must already be in PIXEL_SHADER_RESOURCE) by UV, so any input size works
//  (the chain is fixed at O/2; the bright-pass 4-tap box prefilters the input).
//  Result ends in m_BloomUp[0] (full bloom-res), left in SRV state.
//  最後にビューポート / テクセルサイズを RestoreExtent (実際のポスト入力サイズ) へ戻す。
// ============================================================
void FSceneRenderer::RenderBloom(RENDER_TARGET* Input, XMUINT2 RestoreExtent)
{
	ID3D12GraphicsCommandList* cl = m_RHI->GetGraphicsCommandList();

	const FLOAT clr[4] = { 0,0,0,1 };

	// ---- Bright-pass: Input -> m_BloomMip[0] ----
	TransitionToRenderTarget(cl, m_BloomMip[0].get());
	cl->OMSetRenderTargets(1, &m_BloomMip[0]->RTVHandle, TRUE, nullptr);
	cl->ClearRenderTargetView(m_BloomMip[0]->RTVHandle, clr, 0, nullptr);
	SetViewportAndScissor(cl, m_BloomMipW[0], m_BloomMipH[0]);
	SetTexelSize(m_BloomMipW[0], m_BloomMipH[0]);						// 4 タップボックスの間隔 = mip0 テクセルの 1/4
	UploadPostProcessConstant();
	m_RHI->SetPipelineState("PostProcessBloomThreshold");
	m_RHI->SetTexture(RenderManager::TEXTURE_TYPE::BASE_COLOR, Input); // t0
	DrawScreenPass();
	TransitionToShaderResource(cl, m_BloomMip[0].get());

	// ---- Downsample chain: mip[i-1] -> mip[i] ----
	for (int i = 1; i < BLOOM_MIPS; ++i)
	{
		SetTexelSize(m_BloomMipW[i - 1], m_BloomMipH[i - 1]);
		UploadPostProcessConstant();

		TransitionToRenderTarget(cl, m_BloomMip[i].get());
		cl->OMSetRenderTargets(1, &m_BloomMip[i]->RTVHandle, TRUE, nullptr);
		cl->ClearRenderTargetView(m_BloomMip[i]->RTVHandle, clr, 0, nullptr);
		SetViewportAndScissor(cl, m_BloomMipW[i], m_BloomMipH[i]);          // dest mip size
		m_RHI->SetPipelineState("PostProcessBloomDownsample");
		m_RHI->SetTexture(RenderManager::TEXTURE_TYPE::BASE_COLOR, m_BloomMip[i - 1].get()); // t0 = source
		DrawScreenPass();
		TransitionToShaderResource(cl, m_BloomMip[i].get());
	}

	// ---- Upsample chain (deepest down mip -> up[0]) ----
	for (int i = BLOOM_MIPS - 2; i >= 0; --i)
	{
		RENDER_TARGET* lowSrc = (i == BLOOM_MIPS - 2)
			? m_BloomMip[BLOOM_MIPS - 1].get()
			: m_BloomUp[i + 1].get();

		SetTexelSize(m_BloomMipW[i + 1], m_BloomMipH[i + 1]);
		UploadPostProcessConstant();

		TransitionToRenderTarget(cl, m_BloomUp[i].get());
		cl->OMSetRenderTargets(1, &m_BloomUp[i]->RTVHandle, TRUE, nullptr);
		cl->ClearRenderTargetView(m_BloomUp[i]->RTVHandle, clr, 0, nullptr);
		SetViewportAndScissor(cl, m_BloomMipW[i], m_BloomMipH[i]);          // dest (up) mip size
		m_RHI->SetPipelineState("PostProcessBloomUpsample");
		m_RHI->SetTexture(RenderManager::TEXTURE_TYPE::BASE_COLOR, lowSrc);              // t0 low-res
		m_RHI->SetTexture(RenderManager::TEXTURE_TYPE::BLOOM, m_BloomMip[i].get()); // t9 hi-res same-res mip
		DrawScreenPass();
		TransitionToShaderResource(cl, m_BloomUp[i].get());
	}

	// restore the post-input viewport + texel size for the tonemap pass.
	SetViewportAndScissor(cl, (int)RestoreExtent.x, (int)RestoreExtent.y);
	SetTexelSize((int)RestoreExtent.x, (int)RestoreExtent.y);
	UploadPostProcessConstant();
}
