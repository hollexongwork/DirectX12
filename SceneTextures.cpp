#include "Main.h"
#include "RenderManager.h"
#include "SceneTextures.h"
#include "D3DX12.h"

void FSceneTextures::Init(RenderManager* RHI)
{
	const int width = RHI->GetBackBufferWidth();
	const int height = RHI->GetBackBufferHeight();

	// G-Buffer の各レンダーターゲット生成 
	GBufferC = RHI->CreateRenderTarget(width, height, DXGI_FORMAT_R16G16B16A16_FLOAT);
	GBufferC->Resource->SetName(L"GBufferC");

	GBufferA = RHI->CreateRenderTarget(width, height, DXGI_FORMAT_R16G16B16A16_FLOAT);
	GBufferA->Resource->SetName(L"GBufferA");

	// Metallic(R)  Specular(G)  Roughness(B)  AO(A)
	GBufferB = RHI->CreateRenderTarget(width, height, DXGI_FORMAT_R16G16B16A16_FLOAT);
	GBufferB->Resource->SetName(L"GBufferB");

	// Substrate Slab パックデータ (RT3/RT4)。UINT RTV のクリアは
	// float クリア値 {0,0,0,1} が uint へ変換されるため x = 0
	// (= 非 Substrate ヘッダ) が保証される。
	SubstrateMaterial0 = RHI->CreateRenderTarget(width, height, DXGI_FORMAT_R32G32B32A32_UINT);
	SubstrateMaterial0->Resource->SetName(L"SubstrateMaterial0");

	SubstrateMaterial1 = RHI->CreateRenderTarget(width, height, DXGI_FORMAT_R32G32B32A32_UINT);
	SubstrateMaterial1->Resource->SetName(L"SubstrateMaterial1");

	// Linear depth
	LinearDepth = RHI->CreateRenderTarget(width, height, DXGI_FORMAT_R32G32_FLOAT);
	LinearDepth->Resource->SetName(L"LinearDepthBuffer");

	// HDR SceneColor (linear lit result, tonemapped later)
	SceneColor = RHI->CreateRenderTarget(width, height, DXGI_FORMAT_R16G16B16A16_FLOAT);
	SceneColor->Resource->SetName(L"SceneColorBuffer");

	// 屈折用シーンカラーコピー (半透明パス直前に CopyResource で確定)
	SceneColorCopy = RHI->CreateRenderTarget(width, height, DXGI_FORMAT_R16G16B16A16_FLOAT);
	SceneColorCopy->Resource->SetName(L"SceneColorCopyBuffer");

	// 前フレーム SceneColor 履歴 (Lumen スクリーンスペーストレース用)
	PrevSceneColor = RHI->CreateRenderTarget(width, height, DXGI_FORMAT_R16G16B16A16_FLOAT);
	PrevSceneColor->Resource->SetName(L"PrevSceneColorBuffer");

	// MRT 順 = RT0..RT4 (ベースパス出力 / RenderManager の gbuffer[] と 1:1)
	GBuffers =
	{
		GBufferC.get(),
		GBufferA.get(),
		GBufferB.get(),
		SubstrateMaterial0.get(),
		SubstrateMaterial1.get(),
	};

	// 深度バッファ用 SRV (R32_TYPELESS -> R32_FLOAT)
	{
		DepthSRVIndex = RHI->AllocateDescriptor();

		D3D12_CPU_DESCRIPTOR_HANDLE srvHandle = RHI->GetCPUDescriptorHandle(DepthSRVIndex);

		D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
		srvDesc.Format = DXGI_FORMAT_R32_FLOAT; // R32_TYPELESS -> R32_FLOAT
		srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
		srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		srvDesc.Texture2D.MipLevels = 1;
		srvDesc.Texture2D.MostDetailedMip = 0;

		RHI->GetDevice()->CreateShaderResourceView(RHI->GetDepthBufferResource(), &srvDesc, srvHandle);

		DepthSRVHandle = RHI->GetGPUDescriptorHandle(DepthSRVIndex);
	}

	// ---- 常在読み取り状態を (PIXEL | NON_PIXEL) へ引き上げる ----
	// G-Buffer / LinearDepth / PrevSceneColor は Lumen のコンピュート
	// パス (スクリーンプローブ / 反射) からも読まれるため、
	// 「読み取り状態」を PIXEL 単独から (PIXEL | NON_PIXEL) に統一する。
	// (CreateRenderTarget の初期状態は PIXEL のみ。以降の全遷移は
	//  SceneRenderer.cpp 側がこの複合状態を before/after に使う)
	{
		const D3D12_RESOURCE_STATES readState =
			D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

		D3D12_RESOURCE_BARRIER barriers[7] = {
			CD3DX12_RESOURCE_BARRIER::Transition(GBufferC->Resource.Get(),
				D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, readState),
			CD3DX12_RESOURCE_BARRIER::Transition(GBufferA->Resource.Get(),
				D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, readState),
			CD3DX12_RESOURCE_BARRIER::Transition(GBufferB->Resource.Get(),
				D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, readState),
			CD3DX12_RESOURCE_BARRIER::Transition(SubstrateMaterial0->Resource.Get(),
				D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, readState),
			CD3DX12_RESOURCE_BARRIER::Transition(SubstrateMaterial1->Resource.Get(),
				D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, readState),
			CD3DX12_RESOURCE_BARRIER::Transition(LinearDepth->Resource.Get(),
				D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, readState),
			CD3DX12_RESOURCE_BARRIER::Transition(PrevSceneColor->Resource.Get(),
				D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, readState),
		};
		RHI->GetGraphicsCommandList()->ResourceBarrier(_countof(barriers), barriers);
	}

	// ImGui 表示用の線形深度 SRV (R チャンネルをグレースケール表示)
	{
		unsigned int dispIndex = RHI->AllocateDescriptor();

		D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle = RHI->GetCPUDescriptorHandle(dispIndex);

		D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
		srvDesc.Format = DXGI_FORMAT_R32G32_FLOAT;
		srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
		// G 成分 (= 線形深度) を RGB に複製し、A を 1 に固定して可視化
		srvDesc.Shader4ComponentMapping = D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(
			D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_1, // R <- G
			D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_1, // G <- G
			D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_1, // B <- G
			D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_1);          // A <- 1
		srvDesc.Texture2D.MipLevels = 1;
		srvDesc.Texture2D.MostDetailedMip = 0;

		RHI->GetDevice()->CreateShaderResourceView(LinearDepth->Resource.Get(), &srvDesc, cpuHandle);

		LinearDepthDisplaySRVHandle = RHI->GetGPUDescriptorHandle(dispIndex);
	}
}
