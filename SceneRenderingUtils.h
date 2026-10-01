#pragma once
#include "RenderManager.h"
#include "D3DX12.h"

// ============================================================
//  SceneRenderingUtils
//  スクリーンパス用ヘルパー (UE の FScreenPassTexture / AddDrawScreenPass
//  周辺ユーティリティ相当の最小セット)。
//  SceneRenderer.cpp の無名名前空間から移設したもの。無名名前空間の
//  関数は他の翻訳単位から見えないため、VelocityRendering.cpp /
//  PostProcessUpscale.cpp と共有できるようヘッダのみの
//  inline 関数にしている。
//
//  状態の略記:
//    PSR = PIXEL_SHADER_RESOURCE
//    RD  = PIXEL_SHADER_RESOURCE | NON_PIXEL_SHADER_RESOURCE
//          (ピクセルシェーダとコンピュートの両方が読む常駐状態)
// ============================================================

// PIXEL_SHADER_RESOURCE -> RENDER_TARGET
inline void TransitionToRenderTarget(ID3D12GraphicsCommandList* cl, RENDER_TARGET* rt)
{
	cl->ResourceBarrier(1,
		&CD3DX12_RESOURCE_BARRIER::Transition(rt->Resource.Get(),
			D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
			D3D12_RESOURCE_STATE_RENDER_TARGET));
}

// RENDER_TARGET -> PIXEL_SHADER_RESOURCE
inline void TransitionToShaderResource(ID3D12GraphicsCommandList* cl, RENDER_TARGET* rt)
{
	cl->ResourceBarrier(1,
		&CD3DX12_RESOURCE_BARRIER::Transition(rt->Resource.Get(),
			D3D12_RESOURCE_STATE_RENDER_TARGET,
			D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE));
}

// 読み取り (PIXEL | NON_PIXEL) -> RENDER_TARGET
// (コンピュートからも読まれる常駐 RD のターゲット用。Velocity 等)
inline void TransitionReadToRenderTarget(ID3D12GraphicsCommandList* cl, RENDER_TARGET* rt)
{
	cl->ResourceBarrier(1,
		&CD3DX12_RESOURCE_BARRIER::Transition(rt->Resource.Get(),
			D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
			D3D12_RESOURCE_STATE_RENDER_TARGET));
}

// RENDER_TARGET -> 読み取り (PIXEL | NON_PIXEL)
inline void TransitionRenderTargetToRead(ID3D12GraphicsCommandList* cl, RENDER_TARGET* rt)
{
	cl->ResourceBarrier(1,
		&CD3DX12_RESOURCE_BARRIER::Transition(rt->Resource.Get(),
			D3D12_RESOURCE_STATE_RENDER_TARGET,
			D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
}

// フルスクリーンクアッドは NDC -1..1 を覆うため、ラスタライズ範囲は
// ビューポート / シザーだけで決まる。BeginFrame の既定ビューポートは
// レンダー解像度 R なので、それ以外のサイズのターゲット (Bloom ミップ /
// ハーフ解像度 DOF / トーンマップ出力 / バックバッファ O) は必ず自前で
// 設定すること (設定しないと一部にしか描かれない / はみ出す)。
inline void SetViewportAndScissor(ID3D12GraphicsCommandList* cl, int w, int h)
{
	D3D12_VIEWPORT vp{ 0.0f, 0.0f, (FLOAT)w, (FLOAT)h, 0.0f, 1.0f };
	D3D12_RECT     sc{ 0, 0, (LONG)w, (LONG)h };
	cl->RSSetViewports(1, &vp);
	cl->RSSetScissorRects(1, &sc);
}
