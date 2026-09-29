#include "Main.h"
#include "RenderManager.h"
#include "FogRendering.h"
#include "ExponentialHeightFogComponent.h"
#include "LightSceneProxy.h"
#include <cmath>
#include <cfloat>

// ============================================================
//  FExponentialHeightFogSceneInfo
//  FogRendering.cpp の FExponentialHeightFogSceneInfo コンストラクタ相当。
//  コンポーネントのアーティスト値をレンダー側の単位へ換算する:
//    - Density / HeightFalloff : / 10 [1/m]
//    - VolumetricFogEmissive   : / 100 [/m]
//    - 高さ = コンポーネントのワールド Y
// ============================================================
FExponentialHeightFogSceneInfo::FExponentialHeightFogSceneInfo(const UExponentialHeightFogComponent* InComponent)
	: Component(InComponent)
{
	const XMFLOAT3 location = InComponent->GetComponentLocation();

	FogData[0].Height = location.y;
	FogData[1].Height = FogData[0].Height + InComponent->GetSecondFogData().FogHeightOffset;
	FogData[0].Density = InComponent->GetFogDensity() / 10.0f;
	FogData[0].HeightFalloff = InComponent->GetFogHeightFalloff() / 10.0f;
	FogData[1].Density = InComponent->GetSecondFogData().FogDensity / 10.0f;
	FogData[1].HeightFalloff = InComponent->GetSecondFogData().FogHeightFalloff / 10.0f;

	FogMaxOpacity = InComponent->GetFogMaxOpacity();
	StartDistance = InComponent->GetStartDistance();
	EndDistance = (InComponent->GetEndDistance() > 0.0f) ? InComponent->GetEndDistance() : 0.0f;
	FogCutoffDistance = InComponent->GetFogCutoffDistance();

	DirectionalInscatteringExponent = InComponent->GetDirectionalInscatteringExponent();
	DirectionalInscatteringStartDistance = InComponent->GetDirectionalInscatteringStartDistance();
	DirectionalInscatteringColor = InComponent->GetDirectionalInscatteringLuminance();

	// キューブマップ使用時はティントがインスキャッタ色になる 
	bInscatteringColorCubemap = InComponent->GetInscatteringColorCubemap();
	FogColor = bInscatteringColorCubemap
		? InComponent->GetInscatteringTextureTint()
		: InComponent->GetFogInscatteringLuminance();
	InscatteringColorCubemapAngle = XMConvertToRadians(InComponent->GetInscatteringColorCubemapAngle());
	FullyDirectionalInscatteringColorDistance = InComponent->GetFullyDirectionalInscatteringColorDistance();
	NonDirectionalInscatteringColorDistance = InComponent->GetNonDirectionalInscatteringColorDistance();

	// ---- Volumetric Fog ----
	bEnableVolumetricFog = InComponent->GetVolumetricFog();
	{
		float g = InComponent->GetVolumetricFogScatteringDistribution();
		VolumetricFogScatteringDistribution = (g < -0.99f) ? -0.99f : ((g > 0.99f) ? 0.99f : g);
	}
	VolumetricFogAlbedo = InComponent->GetVolumetricFogAlbedo();

	// アーティストが極小値を扱わずに済むようスケールを掛ける
	{
		const float UnitScale = 1.0f / 100.0f;
		const XMFLOAT3& e = InComponent->GetVolumetricFogEmissive();
		VolumetricFogEmissive.x = (std::max)(e.x * UnitScale, 0.0f);
		VolumetricFogEmissive.y = (std::max)(e.y * UnitScale, 0.0f);
		VolumetricFogEmissive.z = (std::max)(e.z * UnitScale, 0.0f);
	}
	VolumetricFogExtinctionScale = (std::max)(InComponent->GetVolumetricFogExtinctionScale(), 0.0f);
	VolumetricFogDistance = (std::max)(InComponent->GetVolumetricFogDistance(), 0.0f);
	VolumetricFogStartDistance = (std::max)(InComponent->GetVolumetricFogStartDistance(), 0.0f);
	VolumetricFogNearFadeInDistance = (std::max)(InComponent->GetVolumetricFogNearFadeInDistance(), 0.0f);
	VolumetricFogStaticLightingScatteringIntensity = (std::max)(InComponent->GetVolumetricFogStaticLightingScatteringIntensity(), 0.0f);
	bOverrideLightColorsWithFogInscatteringColors = InComponent->GetOverrideLightColorsWithFogInscatteringColors();
}


// ============================================================
//  FFogSceneRenderer
// ============================================================
FFogSceneRenderer::FFogSceneRenderer(RenderManager* RHI)
	: m_RHI(RHI)
{
}


FFogSceneRenderer::~FFogSceneRenderer() = default;


void FFogSceneRenderer::Init()
{
	m_VolumetricFog = std::make_unique<FVolumetricFog>(m_RHI);
	m_VolumetricFog->Init();

	// キューブマップ未登録時の t33 用に、有効なデスクリプタで初期化しておく
	m_InscatteringCubemapSRVIndex = m_VolumetricFog->GetIntegratedLightScatteringSRVIndex();
	m_InscatteringCubemapNumMips = 1;
}


void FFogSceneRenderer::SetInscatteringColorCubemap(unsigned int SRVIndex, unsigned int NumMips)
{
	m_InscatteringCubemapSRVIndex = SRVIndex;
	m_InscatteringCubemapNumMips = (NumMips > 0) ? NumMips : 1;
}


// ============================================================
//  InitFogConstants
//  FSceneRenderer::InitFogConstants + SetupFogUniformParameters 相当。
//  観測者 (カメラ) の高さで密度を畳み込んだ ExponentialFogParameters と、
//  Directional Inscattering / キューブマップ / Volumetric Fog の
//  ビュー毎パラメータを FOG 定数 (b7) へ解決する。
//  フォグ不在のフレームは「フォグなし」の恒等値 (密度 0 / 透過率 1 /
//  Directional 無効 / ApplyVolumetricFog 0) を積む。
// ============================================================
void FFogSceneRenderer::InitFogConstants(const FScene* Scene, const VIEW_CONSTANT& View, const FLightSceneProxy* DirectionalLight)
{
	m_FogConstant = FOG_CONSTANT{};
	m_bHasFog = (Scene != nullptr) && Scene->HasAnyExponentialHeightFog();
	m_bVolumetricFogActive = false;

	// ビュー前方 (VIEW 定数は転置格納: 行 3 = 元の第 3 列 = forward)
	{
		XMVECTOR forward = XMVector3Normalize(XMVectorSet(View.View._31, View.View._32, View.View._33, 0.0f));
		XMFLOAT3 f;
		XMStoreFloat3(&f, forward);
		m_FogConstant.VolumetricFogViewForward = { f.x, f.y, f.z, 0.0f };
	}

	if (!m_bHasFog)
	{
		return;
	}

	// 先頭のフォグのみ使う
	m_FogInfo = Scene->GetExponentialFogs()[0];
	const FExponentialHeightFogSceneInfo& FogInfo = m_FogInfo;

	const float viewOriginY = View.WorldCameraOrigin.y;

	auto clampf = [](float v, float lo, float hi) { return (v < lo) ? lo : ((v > hi) ? hi : v); };

	// 観測者高さで畳み込んだ密度 (指数は IEEE float の範囲にクランプ)
	const float CollapsedFogParameterPower = clampf(
		-FogInfo.FogData[0].HeightFalloff * (viewOriginY - FogInfo.FogData[0].Height),
		-126.0f + 1.0f, 127.0f - 1.0f);
	const float CollapsedFogParameterPowerSecond = clampf(
		-FogInfo.FogData[1].HeightFalloff * (viewOriginY - FogInfo.FogData[1].Height),
		-126.0f + 1.0f, 127.0f - 1.0f);

	const float CollapsedFogParameter[2] =
	{
		FogInfo.FogData[0].Density * std::exp2(CollapsedFogParameterPower),
		FogInfo.FogData[1].Density * std::exp2(CollapsedFogParameterPowerSecond),
	};

	// 観測者高さのクランプ (シェーダの数値精度対策。65536cm / 上限 1,000,000cm)
	const float MaxObserverHeightDifference = 655.36f;
	float MaxObserverHeight = FLT_MAX;
	for (int i = 0; i < 2; ++i)
	{
		if (FogInfo.FogData[i].Density > 0.0f)
		{
			MaxObserverHeight = (std::min)(MaxObserverHeight, FogInfo.FogData[i].Height + MaxObserverHeightDifference);
		}
	}
	MaxObserverHeight = (std::min)(MaxObserverHeight, 10000.0f);

	m_FogConstant.ExponentialFogParameters = { CollapsedFogParameter[0], FogInfo.FogData[0].HeightFalloff, MaxObserverHeight, FogInfo.StartDistance };
	m_FogConstant.ExponentialFogParameters2 = { CollapsedFogParameter[1], FogInfo.FogData[1].HeightFalloff, FogInfo.FogData[1].Density, FogInfo.FogData[1].Height };
	m_FogConstant.ExponentialFogColorParameter = { FogInfo.FogColor.x, FogInfo.FogColor.y, FogInfo.FogColor.z, 1.0f - FogInfo.FogMaxOpacity };
	m_FogConstant.ExponentialFogParameters3 = { FogInfo.FogData[0].Density, FogInfo.FogData[0].Height, FogInfo.bInscatteringColorCubemap ? 1.0f : 0.0f, FogInfo.FogCutoffDistance };
	m_FogConstant.ExponentialFogParameters4 = { FogInfo.EndDistance, 0.0f, 0.0f, 0.0f };

	// ---- Inscattering Color Cubemap ----
	m_FogConstant.SinCosInscatteringColorCubemapRotation = {
		std::sin(FogInfo.InscatteringColorCubemapAngle), std::cos(FogInfo.InscatteringColorCubemapAngle), 0.0f, 0.0f };
	{
		const float InvRange = 1.0f / (std::max)(FogInfo.FullyDirectionalInscatteringColorDistance - FogInfo.NonDirectionalInscatteringColorDistance, 0.00001f);
		// 非指向性色 = 最終ミップ (全方位平均に近いぼかし)
		const float LastMip = (float)(m_InscatteringCubemapNumMips - 1);
		m_FogConstant.FogInscatteringTextureParameters = { InvRange, -FogInfo.NonDirectionalInscatteringColorDistance * InvRange, LastMip, 0.0f };
	}

	// ---- Directional Inscattering (太陽ライトがあるときのみ) ----
	{
		const bool bUseDirectionalInscattering = (DirectionalLight != nullptr);
		m_FogConstant.InscatteringLightDirection = {
			View.DirectionalLightDirection.x, View.DirectionalLightDirection.y, View.DirectionalLightDirection.z,
			bUseDirectionalInscattering ? (std::max)(0.0f, FogInfo.DirectionalInscatteringStartDistance) : -1.0f };
		m_FogConstant.DirectionalInscatteringColor = {
			FogInfo.DirectionalInscatteringColor.x, FogInfo.DirectionalInscatteringColor.y, FogInfo.DirectionalInscatteringColor.z,
			clampf(FogInfo.DirectionalInscatteringExponent, 0.000001f, 1000.0f) };
	}

	// ---- Volumetric Fog ----
	m_bVolumetricFogActive = FogInfo.bEnableVolumetricFog && (m_VolumetricFog != nullptr);
	if (m_bVolumetricFogActive)
	{
		const float nearPlane = View.NearFar.x;
		const float maxDistance = FVolumetricFog::ComputeMaxDistance(nearPlane, FogInfo.VolumetricFogDistance);
		const XMFLOAT3 gridZ = FVolumetricFog::ComputeGridZParams(nearPlane, maxDistance);

		const float gridW = (float)(m_VolumetricFog->GetGridSizeX() * VOLUMETRIC_FOG_GRID_PIXEL_SIZE);
		const float gridH = (float)(m_VolumetricFog->GetGridSizeY() * VOLUMETRIC_FOG_GRID_PIXEL_SIZE);

		m_FogConstant.VolumetricFogGridZParams = { gridZ.x, gridZ.y, gridZ.z, 1.0f };
		m_FogConstant.VolumetricFogParameters = { maxDistance, 1.0f / (float)m_VolumetricFog->GetGridSizeZ(), 1.0f / gridW, 1.0f / gridH };
	}
}


// ============================================================
//  ComputeVolumetricFog
// ============================================================
void FFogSceneRenderer::ComputeVolumetricFog(const FComputeInputs& Inputs)
{
	if (m_VolumetricFog == nullptr)
	{
		return;
	}

	FVolumetricFogInputs in;
	in.View = Inputs.View;
	in.ForwardLightData = Inputs.ForwardLightData;
	in.PrevViewProjectionT = Inputs.PrevViewProjectionT;
	in.bHistoryValid = Inputs.bHistoryValid;
	in.LightBufferSRVIndex = Inputs.LightBufferSRVIndex;
	in.LightGrid = Inputs.LightGrid;
	in.ShadowRenderer = Inputs.ShadowRenderer;
	in.SkyIrradianceSRVIndex = Inputs.SkyIrradianceSRVIndex;

	if (Inputs.View)
	{
		in.DirectionalLightDirection = Inputs.View->DirectionalLightDirection;
		in.DirectionalLightColor = Inputs.View->DirectionalLightColor;
	}
	in.bHasDirectionalLight = (Inputs.DirectionalLight != nullptr);
	in.DirectionalLightVolumetricScatteringIntensity =
		Inputs.DirectionalLight ? Inputs.DirectionalLight->GetVolumetricScatteringIntensity() : 0.0f;

	// 無効フレームは null を渡す (履歴を捨てて何もしない)
	in.FogInfo = (m_bHasFog && m_bVolumetricFogActive) ? &m_FogInfo : nullptr;

	m_VolumetricFog->Dispatch(in);
}


// ============================================================
//  BindFogResources: b7 + t33 (キューブマップ) + t34 (Volumetric Fog)
// ============================================================
void FFogSceneRenderer::BindFogResources()
{
	m_RHI->SetConstant(RenderManager::CONSTANT_TYPE::FOG, &m_FogConstant, sizeof(m_FogConstant));

	m_RHI->BindRootTableBySRVIndex(
		(unsigned int)RenderManager::TEXTURE_TYPE::FOG_INSCATTERING_CUBEMAP, m_InscatteringCubemapSRVIndex);

	if (m_VolumetricFog)
	{
		m_RHI->BindRootTableBySRVIndex(
			(unsigned int)RenderManager::TEXTURE_TYPE::VOLUMETRIC_FOG_INTEGRATED,
			m_VolumetricFog->GetIntegratedLightScatteringSRVIndex());
	}
}


// ============================================================
//  RenderFog (FDeferredShadingSceneRenderer::RenderFog / RenderViewFog)
//  デファードライティング直後、SceneColor が RENDER_TARGET の状態で
//  フルスクリーンクアッドを PSO "HeightFog" で描く。b0 (View) は
//  デファードパスがバインド済みのものをそのまま使う。
// ============================================================
void FFogSceneRenderer::RenderFog(const VERTEX_BUFFER* ScreenQuad, unsigned int DepthSRVIndex)
{
	if (!m_bHasFog || ScreenQuad == nullptr)
	{
		return;
	}

	m_RHI->SetPipelineState("HeightFog");

	// 深度 (t3): ワールド座標の再構築用
	m_RHI->BindRootTableBySRVIndex((unsigned int)RenderManager::TEXTURE_TYPE::DEPTH, DepthSRVIndex);

	BindFogResources();

	ID3D12GraphicsCommandList* cl = m_RHI->GetGraphicsCommandList();
	m_RHI->SetVertexBuffer(ScreenQuad);
	cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
	cl->DrawInstanced(4, 1, 0, 0);
}
