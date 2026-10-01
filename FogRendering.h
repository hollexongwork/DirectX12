#pragma once
#include "RenderManager.h"
#include "Scene.h"
#include "VolumetricFog.h"

class FLightSceneProxy;
class FLightGridInjection;
class FShadowSceneRenderer;

// ============================================================
//  FogRendering
//  FogRendering.cpp (FSceneRenderer::InitFogConstants /
//  SetupFogUniformParameters / FDeferredShadingSceneRenderer::RenderFog)
//  に相当する Exponential Height Fog のレンダー側。
//
//  データフロー:
//    FScene::ExponentialFogs[0] (FExponentialHeightFogSceneInfo)
//      -> InitFogConstants     (ビュー / 太陽光と合わせて FOG 定数 (b7) を解決)
//      -> ComputeVolumetricFog (FVolumetricFog: froxel ボリューム構築)
//      -> RenderFog            (フルスクリーンパス: SceneColor に合成)
//      -> BindFogResources     (トランスルーセンシー用: b7 + t33/t34)
//
//  FSceneRenderer が所有し、各パスの適切なタイミングで呼ぶ
//  (SceneRenderer.cpp のパス列を参照)。ゲーム側オブジェクトには触れない。
// ============================================================

// ============================================================
//  FOG_CONSTANT (b7)
//  HLSL 側 FogUniformParameters (ConstantBuffers.hlsl) と 1:1 ミラー必須。
//  FFogUniformParameters (FogRendering.h / SceneView.h の Exponential*
//  フィールド群) の本エンジン版。単位はメートル、高さは Y。
// ============================================================
struct FOG_CONSTANT
{
	// x = Density0 * exp2(-Falloff0 * (ObserverY - Height0)), y = Falloff0 [1/m], z = MaxObserverHeight [m], w = StartDistance [m]
	XMFLOAT4 ExponentialFogParameters = { 0.0f, 0.0f, 1.0e6f, 0.0f };
	// x = Density1 * exp2(-Falloff1 * (ObserverY - Height1)), y = Falloff1 [1/m], z = Density1 [1/m], w = Height1 [m]
	XMFLOAT4 ExponentialFogParameters2 = { 0.0f, 0.0f, 0.0f, 0.0f };
	// rgb = インスキャッタ色, w = 1 - FogMaxOpacity
	XMFLOAT4 ExponentialFogColorParameter = { 0.0f, 0.0f, 0.0f, 1.0f };
	// x = Density0 [1/m], y = Height0 [m], z = キューブマップ使用 (0/1), w = FogCutoffDistance [m]
	XMFLOAT4 ExponentialFogParameters3 = { 0.0f, 0.0f, 0.0f, 0.0f };
	// xyz = 受光点 -> ライト方向, w = DirectionalInscatteringStartDistance (負 = 無効)
	XMFLOAT4 InscatteringLightDirection = { 0.0f, 1.0f, 0.0f, -1.0f };
	// rgb = DirectionalInscatteringLuminance, w = DirectionalInscatteringExponent
	XMFLOAT4 DirectionalInscatteringColor = { 0.0f, 0.0f, 0.0f, 4.0f };
	// x = sin(角度), y = cos(角度), zw = 未使用
	XMFLOAT4 SinCosInscatteringColorCubemapRotation = { 0.0f, 1.0f, 0.0f, 0.0f };
	// x = InvRange, y = -NonDirectional * InvRange, z = 最終ミップ, w = 未使用
	XMFLOAT4 FogInscatteringTextureParameters = { 0.0f, 0.0f, 0.0f, 0.0f };
	// x = EndDistance [m] (0 = 無効), yzw = 予約
	XMFLOAT4 ExponentialFogParameters4 = { 0.0f, 0.0f, 0.0f, 0.0f };
	// xyz = froxel Z 分布 (B, O, S), w = ApplyVolumetricFog (0/1)
	XMFLOAT4 VolumetricFogGridZParams = { 1.0f, 0.0f, 1.0f, 0.0f };
	// x = VolumetricFogMaxDistance [m] (無効時 0), y = 1/GridSizeZ, zw = SVPos -> ボリューム UV
	XMFLOAT4 VolumetricFogParameters = { 0.0f, 1.0f, 0.0f, 0.0f };
	// xyz = ビュー前方 (正規化), w = 未使用
	XMFLOAT4 VolumetricFogViewForward = { 0.0f, 0.0f, 1.0f, 0.0f };
};
static_assert(sizeof(FOG_CONSTANT) == 192, "FOG_CONSTANT must mirror HLSL FogUniformParameters (b7)");

// ============================================================
//  FFogSceneRenderer
// ============================================================
class FFogSceneRenderer
{
private:
	RenderManager* m_RHI = nullptr;

	// Volumetric Fog (froxel ボリューム構築。コンピュート)
	std::unique_ptr<FVolumetricFog> m_VolumetricFog;

	// 今フレームの解決結果
	FOG_CONSTANT                    m_FogConstant{};
	FExponentialHeightFogSceneInfo  m_FogInfo{};		// ExponentialFogs[0] のコピー
	bool                            m_bHasFog = false;	// フォグが登録されているか
	bool                            m_bVolumetricFogActive = false;	// 今フレーム Volumetric Fog を適用するか

	// t33 (Inscattering Color Cubemap) に割り当てる SRV。
	// 本エンジンでは IBL の prefilter キューブ (t7 と同じ) を使う。
	unsigned int m_InscatteringCubemapSRVIndex = 0;
	unsigned int m_InscatteringCubemapNumMips = 1;

public:
	explicit FFogSceneRenderer(RenderManager* RHI);
	~FFogSceneRenderer();

	void Init();

	// キューブマップ (IBL prefilter) の SRV / ミップ数を登録する (InitIBL の後に 1 回)。
	// 未登録の間は t33 に Volumetric Fog の SRV がダミーとして入るだけなので、必ず登録すること。
	void SetInscatteringColorCubemap(unsigned int SRVIndex, unsigned int NumMips);

	// ---- 毎フレーム: FScene のフォグ + ビュー -> FOG 定数 (InitFogConstants) ----
	//  Scene            : ExponentialFogs[0] を使う (無ければ「フォグなし」の恒等値)
	//  View             : 解決済み VIEW 定数 (カメラ位置 / 行列 / 太陽光)
	//  DirectionalLight : SetupLightConstants が選んだ太陽ライトのプロキシ (null = なし)
	// RenderBasePass の SetupLightConstants 直後に呼ぶこと。
	void InitFogConstants(const FScene* Scene, const VIEW_CONSTANT& View, const FLightSceneProxy* DirectionalLight);

	// ---- Volumetric Fog のコンピュートパス (ComputeVolumetricFog) ----
	// RenderLighting のライトグリッド構築後 / デファードパス前に呼ぶ。
	// bEnableVolumetricFog = false / フォグ不在のフレームは何もしない。
	struct FComputeInputs
	{
		const VIEW_CONSTANT* View = nullptr;
		const FORWARD_LIGHT_CONSTANT* ForwardLightData = nullptr;
		XMFLOAT4X4                    PrevViewProjectionT{};
		bool                          bHistoryValid = false;
		unsigned int                  LightBufferSRVIndex = 0;
		FLightGridInjection* LightGrid = nullptr;
		FShadowSceneRenderer* ShadowRenderer = nullptr;
		unsigned int                  SkyIrradianceSRVIndex = 0;
		const FLightSceneProxy* DirectionalLight = nullptr;
	};
	void ComputeVolumetricFog(const FComputeInputs& Inputs);

	// ---- フォグパス (RenderFog) ----
	// SceneColor が RENDER_TARGET (デファードライティング直後) の状態で
	// 呼ぶ。b7 / t3 / t33 / t34 をバインドし、フルスクリーンクアッドを
	// PSO "HeightFog" (One / SrcAlpha) で描く。フォグ不在なら何もしない。
	void RenderFog(const VERTEX_BUFFER* ScreenQuad, unsigned int DepthSRVIndex);

	// ---- トランスルーセンシー用バインド (b7 + t33 + t34) ----
	void BindFogResources();

	// ---- アクセサ ----
	bool ShouldRenderFog() const { return m_bHasFog; }
	bool IsVolumetricFogActive() const { return m_bVolumetricFogActive; }
	const FOG_CONSTANT& GetFogConstant() const { return m_FogConstant; }
	FVolumetricFog* GetVolumetricFog() { return m_VolumetricFog.get(); }
};
