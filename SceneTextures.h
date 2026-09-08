#pragma once
#include "RenderManager.h"

// ============================================================
//  FSceneTextures
//  FSceneTextures に相当。1 フレームのシーン描画で使う
//  スクリーンサイズのレンダーターゲット群 (G-Buffer / SceneColor /
//  LinearDepth) と深度 SRV をまとめて所有する。
//  RHI 層 (RenderManager) からレンダーターゲット所有権を分離した。
// ============================================================

class FSceneTextures
{
public:
	// ---- G-Buffer (ベースパスの MRT 出力) ----
	// ワールド座標バッファは廃止 (深度 + InvViewProjection から再構築)。
	// MRT スロットは t0/t1/t2 のレジスタ意味論を保つため C/A/B 順。
	std::unique_ptr<RENDER_TARGET> GBufferC;   // RT0: BaseColor (Substrate では DiffuseAlbedo)
	std::unique_ptr<RENDER_TARGET> GBufferA;   // RT1: World Normal
	std::unique_ptr<RENDER_TARGET> GBufferB;   // RT2: Metallic / Specular / Roughness / AO

	// ---- Substrate マテリアルバッファ (RT3/RT4, RGBA32_UINT) ----
	// ベースパスが SubstratePackSlabData の結果を書き、ライティング
	// パスが t22/t23 として Load 読みする (Substrate.hlsl)。
	// x = ヘッダ (クリア値 0 = 非 Substrate ピクセル -> レガシー経路)。
	std::unique_ptr<RENDER_TARGET> SubstrateMaterial0;
	std::unique_ptr<RENDER_TARGET> SubstrateMaterial1;

	std::vector<RENDER_TARGET*> GBuffers;      // 上記 5 枚 (バリア / OMSet 用)

	// ---- HDR SceneColor (linear, R16G16B16A16_FLOAT) ----
	// デファードライティングが書き込み、Tonemap が SDR に解決する。
	std::unique_ptr<RENDER_TARGET> SceneColor;

	// ---- 屈折用シーンカラーコピー (R16G16B16A16_FLOAT) ----
	// 半透明ありのフレームのみ、半透明パス直前に SceneColor から
	// CopyResource され、t21 (SceneColorCopyTexture) として半透明 PS が
	// 屈折オフセット付きで読む (RefractionCommon.hlsl)。
	std::unique_ptr<RENDER_TARGET> SceneColorCopy;

	// ---- 前フレーム SceneColor 履歴 (R16G16B16A16_FLOAT) ----
	// FSceneRenderer::CopySceneColorHistory がポストプロセス直前
	// (= ライティング + 半透明合成後の線形 HDR) に毎フレーム確定する。
	// Lumen のスクリーンスペーストレース (スクリーンプローブ / 反射) が
	// 前フレームリプロジェクションで採光する (UE の Prev SceneColor 相当)。
	// 常在状態は (PIXEL | NON_PIXEL) — コンピュートからも読むため。
	std::unique_ptr<RENDER_TARGET> PrevSceneColor;

	// ---- 前フレーム LinearDepth 履歴 (R32G32_FLOAT) ----
	// PrevSceneColor と同時に確定。Lumen のスクリーンスペーストレースが
	// 前フレームへリプロジェクションした採光点の深度を検証し、
	// ディスオクルージョン (カメラ移動で前フレームには写っていなかった面)
	// の誤採光を棄却するために使う (UE の HistoryDepth 相当)。
	std::unique_ptr<RENDER_TARGET> PrevLinearDepth;

	// ---- Linear depth (R32G32_FLOAT) ----
	std::unique_ptr<RENDER_TARGET> LinearDepth;

	// 深度バッファ SRV (R32_TYPELESS -> R32_FLOAT)
	unsigned int                DepthSRVIndex = 0;
	D3D12_GPU_DESCRIPTOR_HANDLE DepthSRVHandle{};

	// ImGui 表示用の線形深度 SRV (G チャンネルをグレースケール表示)
	D3D12_GPU_DESCRIPTOR_HANDLE LinearDepthDisplaySRVHandle{};

public:
	void Init(RenderManager* RHI);
};
