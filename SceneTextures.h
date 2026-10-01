#pragma once
#include "RenderManager.h"

// ============================================================
//  FSceneTextures
//  FSceneTextures に相当。1 フレームのシーン描画で使う
//  レンダー解像度 R のレンダーターゲット群 (G-Buffer / SceneColor /
//  LinearDepth) と深度 SRV をまとめて所有する。
//  RHI 層 (RenderManager) からレンダーターゲット所有権を分離した。
//
//  全テクスチャは exact-size (R ちょうど。ビューポートは常に (0,0) 起点)。
//  R が変わると FSceneRenderer::ResizeRenderTargets が
//  Release -> (GPU 待ち) -> Init(RHI, W, H) で作り直す (§5.2)。
//  深度バッファ本体は RenderManager 所有 (ReleaseDepthBuffer / CreateDepthBuffer)
//  なので、Init より前に新しいサイズで作り直しておくこと (深度 SRV を張るため)。
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
	// 前フレームリプロジェクションで採光する。
	// 常在状態は (PIXEL | NON_PIXEL) — コンピュートからも読むため。
	std::unique_ptr<RENDER_TARGET> PrevSceneColor;

	// ---- 前フレーム LinearDepth 履歴 (R32G32_FLOAT) ----
	// PrevSceneColor と同時に確定。Lumen のスクリーンスペーストレースが
	// 前フレームへリプロジェクションした採光点の深度を検証し、
	// ディスオクルージョン (カメラ移動で前フレームには写っていなかった面)
	// の誤採光を棄却するために使う。
	std::unique_ptr<RENDER_TARGET> PrevLinearDepth;

	// ---- Linear depth (R32G32_FLOAT) ----
	std::unique_ptr<RENDER_TARGET> LinearDepth;

	// ---- シーンベロシティ (R16G16_UNORM, VelocityCommon.hlsl のエンコード) ----
	// FSceneRenderer::RenderVelocities が動いたプリミティブだけを書く (クリア (0,0,0,1) の
	// RG = 0 = 未書き込み)。TAA (t2) / 可視化 (t35) / ImGui が読む。常在状態は (PIXEL | NON_PIXEL)。
	// 今フレーム描いたかは FSceneRenderer::m_bVelocityValid (偽ならダミーを読む)
	std::unique_ptr<RENDER_TARGET> Velocity;

	// ---- Responsive AA マスク (R8_UNORM) ----
	// Responsive AA (bEnableResponsiveAA) の半透明を 1 にするマスク (UE のステンシル bit 3 の代替)。
	// 書き手は RenderResponsiveAAMask。常在状態は (PIXEL | NON_PIXEL)。
	// 今フレーム描いたかは FSceneRenderer::m_bResponsiveMaskValid
	std::unique_ptr<RENDER_TARGET> ResponsiveAAMask;

	// 深度バッファ SRV (R32_TYPELESS -> R32_FLOAT)
	unsigned int                DepthSRVIndex = 0;

	// ImGui 表示用の線形深度 SRV (G チャンネルをグレースケール表示)。
	// Release で枠を返せるようにインデックスも保持する
	unsigned int                LinearDepthDisplaySRVIndex = 0;
	D3D12_GPU_DESCRIPTOR_HANDLE LinearDepthDisplaySRVHandle{};

	// 確保済みの寸法 (= レンダー解像度 R)。{0,0} = 未確保 (Release 済み)
	DirectX::XMUINT2            Extent{ 0u, 0u };

public:
	// Width x Height (= R) で全ターゲット + 深度 SRV / 表示用 SRV を生成し、
	// 読み取り状態 (PIXEL | NON_PIXEL) への初期バリアを記録する。
	// 深度バッファは RenderManager 側で同じサイズに作成済みであること
	void Init(RenderManager* RHI, unsigned int Width, unsigned int Height);

	// 全ターゲット (~RENDER_TARGET 経由) と深度 SRV / 表示用 SRV を遅延削除キューへ返す。
	// 実解放は呼び出し側の WaitGPU (ResizeRenderTargets の手順 (2))
	void Release(RenderManager* RHI);
};
