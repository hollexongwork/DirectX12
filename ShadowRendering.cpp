#include "Main.h"
#include "RenderManager.h"
#include "ShadowRendering.h"
#include "ConvexVolume.h"
#include "Scene.h"
#include "PrimitiveSceneProxy.h"
#include "CameraComponent.h"
#include "FBXModel.h"
#include "DistanceFieldAtlas.h"

#include "D3DX12.h"

// ============================================================
//  Lifetime
// ============================================================
FShadowSceneRenderer::FShadowSceneRenderer(RenderManager* RHI)
	: m_RHI(RHI)
{
	// DSV ヒープ (シャドウ専有。CSM + ローカルの全スライス分)
	{
		D3D12_DESCRIPTOR_HEAP_DESC desc{};
		desc.NumDescriptors = MAX_SHADOW_CASCADES + MAX_LOCAL_SHADOW_SLICES;
		desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
		desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
		desc.NodeMask = 0;

		HRESULT hr = m_RHI->GetDevice()->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&m_DSVHeap));
		assert(SUCCEEDED(hr));
	}

	InitShadowDepthTarget(m_CSMTarget, CSM_RESOLUTION, MAX_SHADOW_CASCADES, L"DirectionalShadowCascades");
	InitShadowDepthTarget(m_LocalTarget, LOCAL_SHADOW_RESOLUTION, MAX_LOCAL_SHADOW_SLICES, L"LocalLightShadows");
	InitShadowParamBuffers();
	InitDistanceFieldBuffers();

	// DF アトラスを確定させる (メッシュ未登録でも t17 の SRV を有効化する。
	// DeferredPS が静的に t17/t18 を参照するため必須)
	FDistanceFieldAtlas::Get();
}


FShadowSceneRenderer::~FShadowSceneRenderer()
{
	if (m_RHI)
	{
		m_RHI->ReleaseShaderResourceView(m_CSMTarget.SRVIndex);
		m_RHI->ReleaseShaderResourceView(m_LocalTarget.SRVIndex);
		for (int i = 0; i < 2; ++i)
		{
			m_RHI->ReleaseShaderResourceView(m_ShadowParamSRVIndex[i]);
			m_RHI->ReleaseShaderResourceView(m_DFObjectSRVIndex[i]);
		}
	}
}


// ============================================================
//  Resources
// ============================================================
void FShadowSceneRenderer::InitShadowDepthTarget(FShadowDepthTarget& Target,
	unsigned int Resolution, unsigned int ArraySize, const wchar_t* Name)
{
	ID3D12Device* device = m_RHI->GetDevice();

	// R32_TYPELESS で生成し、DSV は D32_FLOAT、SRV は R32_FLOAT として読む
	// (メイン深度バッファと同じ方式)。初期ステートは PIXEL_SHADER_RESOURCE
	// で作り、深度パスの前後で DEPTH_WRITE と往復させる。
	D3D12_RESOURCE_DESC resourceDesc{};
	resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	resourceDesc.Width = Resolution;
	resourceDesc.Height = Resolution;
	resourceDesc.DepthOrArraySize = (UINT16)ArraySize;
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

	HRESULT hr = device->CreateCommittedResource(
		&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
		D3D12_HEAP_FLAG_NONE,
		&resourceDesc,
		D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
		&clearValue,
		IID_PPV_ARGS(&Target.Resource));
	assert(SUCCEEDED(hr));
	Target.Resource->SetName(Name);

	// ---- SRV (Texture2DArray, R32_FLOAT) ----
	Target.SRVIndex = m_RHI->AllocateDescriptor();

	D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
	srvDesc.Format = DXGI_FORMAT_R32_FLOAT;
	srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
	srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	srvDesc.Texture2DArray.MostDetailedMip = 0;
	srvDesc.Texture2DArray.MipLevels = 1;
	srvDesc.Texture2DArray.FirstArraySlice = 0;
	srvDesc.Texture2DArray.ArraySize = ArraySize;

	device->CreateShaderResourceView(Target.Resource.Get(), &srvDesc,
		m_RHI->GetCPUDescriptorHandle(Target.SRVIndex));

	// ---- スライスごとの DSV (D32_FLOAT) ----
	const UINT increment = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);

	Target.SliceDSV.resize(ArraySize);
	for (unsigned int i = 0; i < ArraySize; ++i)
	{
		D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
		dsvDesc.Format = DXGI_FORMAT_D32_FLOAT;
		dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
		dsvDesc.Flags = D3D12_DSV_FLAG_NONE;
		dsvDesc.Texture2DArray.MipSlice = 0;
		dsvDesc.Texture2DArray.FirstArraySlice = i;
		dsvDesc.Texture2DArray.ArraySize = 1;

		D3D12_CPU_DESCRIPTOR_HANDLE handle = m_DSVHeap->GetCPUDescriptorHandleForHeapStart();
		handle.ptr += (SIZE_T)increment * m_DSVHeapUsed;
		m_DSVHeapUsed++;

		device->CreateDepthStencilView(Target.Resource.Get(), &dsvDesc, handle);
		Target.SliceDSV[i] = handle;
	}
}


void FShadowSceneRenderer::InitShadowParamBuffers()
{
	// ローカルシャドウパラメータ (t16)。ライトバッファ (t13) と同じ
	// ダブルバッファ方式のアップロードヒープ + 永続 Map。
	ID3D12Device* device = m_RHI->GetDevice();

	const UINT64 bufferSize = sizeof(FLocalShadowParameters) * MAX_LOCAL_LIGHTS;

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
			D3D12_RESOURCE_STATE_GENERIC_READ,
			nullptr,
			IID_PPV_ARGS(&m_ShadowParamBuffer[i]));
		assert(SUCCEEDED(hr));
		m_ShadowParamBuffer[i]->SetName(L"LocalShadowParamBuffer");

		hr = m_ShadowParamBuffer[i]->Map(0, nullptr, (void**)&m_ShadowParamPointer[i]);
		assert(SUCCEEDED(hr));
		memset(m_ShadowParamPointer[i], 0, bufferSize);

		m_ShadowParamSRVIndex[i] = m_RHI->AllocateDescriptor();

		D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
		srvDesc.Format = DXGI_FORMAT_UNKNOWN;
		srvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
		srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		srvDesc.Buffer.FirstElement = 0;
		srvDesc.Buffer.NumElements = MAX_LOCAL_LIGHTS;
		srvDesc.Buffer.StructureByteStride = sizeof(FLocalShadowParameters);
		srvDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;

		device->CreateShaderResourceView(
			m_ShadowParamBuffer[i].Get(), &srvDesc,
			m_RHI->GetCPUDescriptorHandle(m_ShadowParamSRVIndex[i]));
	}
}


// ============================================================
//  InitDynamicShadows
//  (FSceneRenderer::InitDynamicShadows)
// ============================================================
void FShadowSceneRenderer::InitDynamicShadows(
	const FLightSceneProxy* Directional,
	const std::vector<const FLightSceneProxy*>& LocalLights,
	UCameraComponent* Camera, float AspectRatio)
{
	// 書き込み先をフリップ (GPU が読んでいる前フレーム分を避ける)
	m_ShadowParamFrame ^= 1;

	m_ShadowViews.clear();
	m_bUsedCSM = false;
	m_bUsedLocal = false;

	// キャスターカリング統計のリセット (RenderShadowDepthMaps が累積する)
	m_CullingStats = FShadowCullingStats{};

	// ---- 既定値: 影なし ----
	m_DirectionalConstant = DIRECTIONAL_SHADOW_CONSTANT{};
	m_DirectionalConstant.CascadeSplits = { 1.0e9f, 1.0e9f, 1.0e9f, 1.0e9f };

	// DF 既定値 (x=オブジェクト数 は UpdateDistanceFieldObjects が毎フレーム設定。
	// y/z の自己遮蔽オフセットはディレクショナルの ShadowBias / SlopeBias から
	// SetupDirectionalShadows で上書きされる。ローカルライト DF は t16 側の
	// 各ライト値 (DFSelfShadowBias / NormalOffsetWorld) を参照する)
	m_DirectionalConstant.DFShadowParams0 = { (float)m_NumDFObjects, 0.0f, 0.0f, 0.0f };
	m_DirectionalConstant.DFShadowParams1 = {
		0.0f,
		SHADOW_BIAS_WORLD_SCALE * 0.5f,
		SHADOW_BIAS_WORLD_SCALE * 0.5f,
		0.0f };

	FLocalShadowParameters* params = m_ShadowParamPointer[m_ShadowParamFrame];
	for (unsigned int i = 0; i < MAX_LOCAL_LIGHTS; ++i)
	{
		params[i] = FLocalShadowParameters{};
		params[i].ShadowSliceIndex = -1;
	}

	// ---- ディレクショナル (Whole-Scene CSM) ----
	if (Directional && Directional->AffectsWorld() && Directional->CastsShadows() && Camera)
	{
		SetupDirectionalShadows(Directional, Camera, AspectRatio);
	}

	// ---- ローカル (Spot / Rect / Point) ----
	SetupLocalShadows(LocalLights);
}


// ------------------------------------------------------------
//  CSM カスケード構築。
//  分割はCascadeDistributionExponent 方式:
//    SplitFar[i] = Near + (D - Near) * ((i+1)/N)^Exponent
//  各カスケードはサブフラスタ 8 頂点の外接球でフィットし、
//  ライトビューのテクセルグリッドへスナップして安定化する。
// ------------------------------------------------------------
void FShadowSceneRenderer::SetupDirectionalShadows(const FLightSceneProxy* Directional,
	UCameraComponent* Camera, float AspectRatio)
{
	// ---- カメラ基底 (LookToLH と同じ構成 = 描画ビューと一致) ----
	XMMATRIX camWorld = Camera->GetComponentToWorld();
	XMVECTOR camPos = camWorld.r[3];
	XMVECTOR camFwd = XMVector3Normalize(camWorld.r[2]);

	XMVECTOR worldUp = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
	if (fabsf(XMVectorGetY(camFwd)) > 0.99f)
	{
		worldUp = XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f);	// 真上 / 真下向きの退避
	}
	XMVECTOR camRight = XMVector3Normalize(XMVector3Cross(worldUp, camFwd));
	XMVECTOR camUp = XMVector3Cross(camFwd, camRight);

	const float nearClip = Camera->GetNearClip();
	const float shadowDistance = fminf(Directional->GetDynamicShadowDistance(), Camera->GetFarClip());
	const int   numCascades = max(1, min(Directional->GetNumDynamicShadowCascades(), (int)MAX_SHADOW_CASCADES));
	const float exponent = fmaxf(Directional->GetCascadeDistributionExponent(), 1.0f);
	const float fadeFraction = fmaxf(fminf(Directional->GetShadowDistanceFadeoutFraction(), 0.9f), 0.0f);

	const float tanHalfFovY = tanf(XMConvertToRadians(Camera->GetFieldOfView()) * 0.5f);
	const float tanHalfFovX = tanHalfFovY * AspectRatio;

	// ---- ライト方向 (発光方向) ----
	const XMFLOAT3& lightDirection = Directional->GetDirection();
	XMVECTOR lightDir = XMVector3Normalize(XMLoadFloat3(&lightDirection));

	XMVECTOR lightUp = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
	if (fabsf(XMVectorGetY(lightDir)) > 0.99f)
	{
		lightUp = XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f);
	}

	// ---- 指数分割 ----
	float splitFar[MAX_SHADOW_CASCADES]{};
	for (int i = 0; i < numCascades; ++i)
	{
		const float t = (float)(i + 1) / (float)numCascades;
		splitFar[i] = nearClip + (shadowDistance - nearClip) * powf(t, exponent);
	}

	const float shadowBias = Directional->GetShadowBias();
	const float shadowSlopeBias = Directional->GetShadowSlopeBias();

	float splits[4] = { 1.0e9f, 1.0e9f, 1.0e9f, 1.0e9f };
	float normalOffsets[4]{};
	float depthBiases[4]{};

	for (int i = 0; i < numCascades; ++i)
	{
		const float sn = (i == 0) ? nearClip : splitFar[i - 1];
		const float sf = splitFar[i];

		// ---- サブフラスタ 8 頂点 -> 外接球 ----
		XMVECTOR corners[8];
		int c = 0;
		for (int d = 0; d < 2; ++d)
		{
			const float dist = (d == 0) ? sn : sf;
			const float hw = tanHalfFovX * dist;
			const float hh = tanHalfFovY * dist;
			XMVECTOR centerAt = camPos + camFwd * dist;
			corners[c++] = centerAt - camRight * hw - camUp * hh;
			corners[c++] = centerAt + camRight * hw - camUp * hh;
			corners[c++] = centerAt - camRight * hw + camUp * hh;
			corners[c++] = centerAt + camRight * hw + camUp * hh;
		}

		XMVECTOR center = XMVectorZero();
		for (int k = 0; k < 8; ++k) center += corners[k];
		center *= (1.0f / 8.0f);

		float radius = 0.0f;
		for (int k = 0; k < 8; ++k)
		{
			radius = fmaxf(radius, XMVectorGetX(XMVector3Length(corners[k] - center)));
		}
		// 半径をわずかに量子化して、フレーム間の半径ゆらぎによる
		// シャドウのちらつきを抑える
		radius = ceilf(radius * 16.0f) / 16.0f;
		radius = fmaxf(radius, 0.1f);

		// ---- ライトビュー / オルソ射影 ----
		// ニアはカスケード外 (背後) のキャスターを拾うため backup 分だけ引く
		const float backup = fmaxf(30.0f, radius);
		const float depthRange = backup + radius * 2.0f;

		XMVECTOR eye = center - lightDir * (radius + backup);
		XMMATRIX view = XMMatrixLookAtLH(eye, center, lightUp);
		XMMATRIX proj = XMMatrixOrthographicLH(radius * 2.0f, radius * 2.0f, 0.0f, depthRange);

		// ---- テクセルスナップ (シャドウの安定化) ----
		// ワールド原点のクリップ座標をテクセル単位に丸め、その差分で
		// 射影を平行移動する (カメラ移動でシャドウ縁が泳がない)。
		{
			XMMATRIX viewProj = view * proj;
			XMVECTOR originCS = XMVector3TransformCoord(XMVectorZero(), viewProj);

			const float halfRes = (float)CSM_RESOLUTION * 0.5f;
			const float ox = XMVectorGetX(originCS) * halfRes;
			const float oy = XMVectorGetY(originCS) * halfRes;
			const float dx = (roundf(ox) - ox) / halfRes;
			const float dy = (roundf(oy) - oy) / halfRes;

			proj = proj * XMMatrixTranslation(dx, dy, 0.0f);
		}

		// ---- 定数 / シャドウビューへ格納 ----
		XMMATRIX viewProj = view * proj;
		XMStoreFloat4x4(&m_DirectionalConstant.WorldToShadowCascade[i], XMMatrixTranspose(viewProj));

		splits[i] = sf;

		// テクセルの世界サイズに比例した法線オフセット (スロープバイアス相当)
		const float texelWorldSize = (radius * 2.0f) / (float)CSM_RESOLUTION;
		normalOffsets[i] = texelWorldSize * shadowSlopeBias * 2.0f;

		// 深度バイアスは NDC (深度レンジで正規化)。
		// ワールド換算は shadowBias * SHADOW_BIAS_WORLD_SCALE [m] で DF と一致
		depthBiases[i] = (shadowBias * SHADOW_BIAS_WORLD_SCALE) / depthRange;

		FProjectedShadowInfo info;
		XMStoreFloat4x4(&info.ViewMatrixT, XMMatrixTranspose(view));
		XMStoreFloat4x4(&info.ProjectionMatrixT, XMMatrixTranspose(proj));
		XMStoreFloat4x4(&info.ViewProjection, viewProj);	// キャスターカリング用 (転置前)
		info.SliceIndex = (unsigned int)i;
		info.bDirectional = true;
		m_ShadowViews.push_back(info);
	}

	m_DirectionalConstant.CascadeSplits = { splits[0], splits[1], splits[2], splits[3] };
	m_DirectionalConstant.CascadeNormalOffset = { normalOffsets[0], normalOffsets[1], normalOffsets[2], normalOffsets[3] };
	m_DirectionalConstant.CascadeDepthBias = { depthBiases[0], depthBiases[1], depthBiases[2], depthBiases[3] };
	m_DirectionalConstant.DirectionalShadowParams = {
		(float)numCascades,
		shadowDistance,
		shadowDistance * (1.0f - fadeFraction),
		1.0f / (float)CSM_RESOLUTION };

	// ---- Distance Field Shadows (RayTraced Distance Field Shadows) ----
	// CSM の距離フェード領域から DistanceFieldShadowDistance まで
	// メッシュ SDF のレイマーチで影を継続する。
	if (Directional->UseRayTracedDistanceFieldShadows())
	{
		const float sourceAngleDeg = fmaxf(Directional->GetLightSourceAngle(), 0.05f);
		m_DirectionalConstant.DFShadowParams0.y = Directional->GetDistanceFieldShadowDistance();
		m_DirectionalConstant.DFShadowParams0.z = tanf(XMConvertToRadians(sourceAngleDeg) * 0.5f);
		m_DirectionalConstant.DFShadowParams0.w = Directional->GetDistanceFieldTraceDistance();
		m_DirectionalConstant.DFShadowParams1.x = 1.0f;

		// 自己遮蔽オフセットはライトの ShadowBias / SlopeBias から決める
		// (シャドウマップ用より大きい距離が必要なため専用スケール)
		m_DirectionalConstant.DFShadowParams1.y = SHADOW_BIAS_WORLD_SCALE * Directional->GetShadowBias();
		m_DirectionalConstant.DFShadowParams1.z = SHADOW_BIAS_WORLD_SCALE * Directional->GetShadowSlopeBias();
	}

	m_bUsedCSM = true;
}


// ------------------------------------------------------------
//  ローカルライトのシャドウビュー + t16 パラメータ構築。
//  スライスは登録順 (= ライトバッファ順) に先着で確保する。
//    Spot / Rect : 1 スライス (単一透視投影)
//    Point       : 6 スライス (world 軸整列キューブ 6 面, 90 度透視)
// ------------------------------------------------------------
void FShadowSceneRenderer::SetupLocalShadows(const std::vector<const FLightSceneProxy*>& LocalLights)
{
	// キューブ 6 面の基底 (HLSL 側 ShadowFilteringCommon.hlsl と 1:1 必須)
	static const XMVECTOR CubeFaceForward[6] =
	{
		XMVectorSet( 1.0f, 0.0f, 0.0f, 0.0f),
		XMVectorSet(-1.0f, 0.0f, 0.0f, 0.0f),
		XMVectorSet( 0.0f, 1.0f, 0.0f, 0.0f),
		XMVectorSet( 0.0f,-1.0f, 0.0f, 0.0f),
		XMVectorSet( 0.0f, 0.0f, 1.0f, 0.0f),
		XMVectorSet( 0.0f, 0.0f,-1.0f, 0.0f),
	};
	static const XMVECTOR CubeFaceUp[6] =
	{
		XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f),
		XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f),
		XMVectorSet(0.0f, 0.0f,-1.0f, 0.0f),
		XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f),
		XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f),
		XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f),
	};

	FLocalShadowParameters* params = m_ShadowParamPointer[m_ShadowParamFrame];

	unsigned int slice = 0;
	const unsigned int numLights = min((unsigned int)LocalLights.size(), MAX_LOCAL_LIGHTS);

	for (unsigned int i = 0; i < numLights; ++i)
	{
		const FLightSceneProxy* proxy = LocalLights[i];
		if (proxy == nullptr || !proxy->CastsShadows())
		{
			continue;
		}

		const float radius = proxy->GetAttenuationRadius();
		if (radius <= 0.0f)
		{
			continue;
		}

		const XMFLOAT3& position = proxy->GetPosition();
		const XMFLOAT3& direction = proxy->GetDirection();
		XMVECTOR lightPos = XMLoadFloat3(&position);
		XMVECTOR lightDir = XMVector3Normalize(XMLoadFloat3(&direction));

		const float nearPlane = 0.05f;
		const float farPlane = fmaxf(radius, nearPlane * 2.0f);

		FLocalShadowParameters& sp = params[i];
		sp.DepthBiasNDC = proxy->GetShadowBias() * 0.001f;
		sp.InvShadowResolution = 1.0f / (float)LOCAL_SHADOW_RESOLUTION;
		sp.NormalOffsetWorld = proxy->GetShadowSlopeBias() * SHADOW_BIAS_WORLD_SCALE;
		sp.ShadowNearPlane = nearPlane;
		sp.ShadowFarPlane = farPlane;

		// bUseRayTracedDistanceFieldShadows — シャドウマップの
		// 代わりにメッシュ SDF をレイマーチで評価する。アトラスの
		// スライスは消費しない (ShadowSliceIndex = -1 のまま)。
		if (proxy->UseRayTracedDistanceFieldShadows())
		{
			sp.DFShadow = 1.0f;

			// 自己遮蔽オフセットはシャドウマップと同じワールド換算を使う
			// (NormalOffsetWorld は上で設定済みの共通値をそのまま流用)。
			// DF 固有の必要量は SDF ボクセル幅の下駄がシェーダ側で自動適用
			// されるため、ここはスライダーの純粋な調整量になる。
			sp.DFSelfShadowBias = SHADOW_BIAS_WORLD_SCALE * proxy->GetShadowBias();
			continue;
		}

		if (proxy->GetLightType() == ELightType::Point)
		{
			// ---- ポイント: キューブ 6 面 ----
			if (slice + 6 > MAX_LOCAL_SHADOW_SLICES)
			{
				continue;	// アトラス満杯 (影なしのまま)
			}

			// 90 度 + ガードバンド (シーム側に有効データの余白を作る)。
			// tan(fov/2) = 1 / guardScale。深度の z マッピングは FOV 非依存の
			// ため受光側のデバイス深度再構築はそのまま成立する
			const float guardScale =
				1.0f - 2.0f * POINT_SHADOW_GUARD_TEXELS / (float)LOCAL_SHADOW_RESOLUTION;
			const float faceFov = 2.0f * atanf(1.0f / guardScale);
			XMMATRIX proj = XMMatrixPerspectiveFovLH(faceFov, 1.0f, nearPlane, farPlane);

			for (int face = 0; face < 6; ++face)
			{
				XMMATRIX view = XMMatrixLookToLH(lightPos, CubeFaceForward[face], CubeFaceUp[face]);

				FProjectedShadowInfo info;
				XMStoreFloat4x4(&info.ViewMatrixT, XMMatrixTranspose(view));
				XMStoreFloat4x4(&info.ProjectionMatrixT, XMMatrixTranspose(proj));
				XMStoreFloat4x4(&info.ViewProjection, view * proj);	// キャスターカリング用 (転置前)
				info.SliceIndex = slice + (unsigned int)face;
				info.bDirectional = false;
				m_ShadowViews.push_back(info);
			}

			XMStoreFloat4x4(&sp.WorldToShadow, XMMatrixIdentity());	// ポイントでは未使用
			sp.ShadowSliceIndex = (int)slice;
			slice += 6;
			m_bUsedLocal = true;
		}
		else
		{
			// ---- スポット / レクト: 単一透視投影 ----
			if (slice + 1 > MAX_LOCAL_SHADOW_SLICES)
			{
				continue;
			}

			float fov;
			if (proxy->GetLightType() == ELightType::Spot)
			{
				// アウターコーン全角 + ガード 4 度
				const float cosOuter = fmaxf(fminf(proxy->GetSpotAngles().x, 1.0f), -1.0f);
				fov = 2.0f * acosf(cosOuter) + XMConvertToRadians(4.0f);
			}
			else
			{
				// レクト: バーンドア開き角ベース (透視 1 枚の近似。
				// 150 度を超える範囲の影は落とせない制限あり)
				const float barnCos = fmaxf(fminf(proxy->GetRectBarnCosAngle(), 1.0f), -1.0f);
				fov = 2.0f * acosf(barnCos) + XMConvertToRadians(4.0f);
			}
			fov = fmaxf(fminf(fov, XMConvertToRadians(150.0f)), XMConvertToRadians(20.0f));

			XMVECTOR up = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
			if (fabsf(XMVectorGetY(lightDir)) > 0.99f)
			{
				up = XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f);
			}

			XMMATRIX view = XMMatrixLookToLH(lightPos, lightDir, up);
			XMMATRIX proj = XMMatrixPerspectiveFovLH(fov, 1.0f, nearPlane, farPlane);

			XMStoreFloat4x4(&sp.WorldToShadow, XMMatrixTranspose(view * proj));
			sp.ShadowSliceIndex = (int)slice;

			FProjectedShadowInfo info;
			XMStoreFloat4x4(&info.ViewMatrixT, XMMatrixTranspose(view));
			XMStoreFloat4x4(&info.ProjectionMatrixT, XMMatrixTranspose(proj));
			XMStoreFloat4x4(&info.ViewProjection, view * proj);	// キャスターカリング用 (転置前)
			info.SliceIndex = slice;
			info.bDirectional = false;
			m_ShadowViews.push_back(info);

			slice += 1;
			m_bUsedLocal = true;
		}
	}
}


// ============================================================
//  RenderShadowDepthMaps
//  (FSceneRenderer::RenderShadowDepthMaps)
//  各シャドウビューごとに VIEW 定数 (b0) をライトの View/Projection で
//  詰め直し、FScene のプリミティブプロキシ列を深度のみで描く。
//  ※ b0 を上書きするため、後続パスはカメラの VIEW 定数を積み直すこと。
// ============================================================
void FShadowSceneRenderer::RenderShadowDepthMaps(FScene* Scene)
{
	if (Scene == nullptr || m_ShadowViews.empty())
	{
		return;	// 影なし: バリアもスキップ (リソースは PIXEL_SHADER_RESOURCE のまま)
	}

	ID3D12GraphicsCommandList* cl = m_RHI->GetGraphicsCommandList();

	// ---- SRV -> DEPTH_WRITE ----
	if (m_bUsedCSM)
	{
		cl->ResourceBarrier(1,
			&CD3DX12_RESOURCE_BARRIER::Transition(
				m_CSMTarget.Resource.Get(),
				D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
				D3D12_RESOURCE_STATE_DEPTH_WRITE));
	}
	if (m_bUsedLocal)
	{
		cl->ResourceBarrier(1,
			&CD3DX12_RESOURCE_BARRIER::Transition(
				m_LocalTarget.Resource.Get(),
				D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
				D3D12_RESOURCE_STATE_DEPTH_WRITE));
	}

	m_RHI->SetPipelineState("ShadowDepth");

	for (const FProjectedShadowInfo& shadow : m_ShadowViews)
	{
		const FShadowDepthTarget& target = shadow.bDirectional ? m_CSMTarget : m_LocalTarget;
		const unsigned int resolution = shadow.bDirectional ? CSM_RESOLUTION : LOCAL_SHADOW_RESOLUTION;

		// RTV なしの深度のみターゲット
		D3D12_CPU_DESCRIPTOR_HANDLE dsv = target.SliceDSV[shadow.SliceIndex];
		cl->OMSetRenderTargets(0, nullptr, FALSE, &dsv);
		cl->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

		D3D12_VIEWPORT vp{ 0.0f, 0.0f, (FLOAT)resolution, (FLOAT)resolution, 0.0f, 1.0f };
		D3D12_RECT     sc{ 0, 0, (LONG)resolution, (LONG)resolution };
		cl->RSSetViewports(1, &vp);
		cl->RSSetScissorRects(1, &sc);

		// VIEW 定数 (b0) をライトの View / Projection で詰め直す
		// (ShadowDepthVS は View / Projection のみ参照する)
		VIEW_CONSTANT viewConstant{};
		viewConstant.View = shadow.ViewMatrixT;
		viewConstant.Projection = shadow.ProjectionMatrixT;
		m_RHI->SetConstant(RenderManager::CONSTANT_TYPE::VIEW, &viewConstant, sizeof(viewConstant));

		// ---- シャドウビューのフラスタムでキャスターをカリング ----
		// (FProjectedShadowInfo の受影範囲外キャスター除外に相当)。
		// CSM のオルソ近平面は backup 分だけライト側へ引いてあるため、
		// 6 平面すべてでの判定はラスタライザのクリップ結果と一致する
		// (= カリングによる描画結果の変化はない)。
		FConvexVolume shadowFrustum;
		GetViewFrustumBounds(shadowFrustum, XMLoadFloat4x4(&shadow.ViewProjection), true, true);

		m_CullingStats.NumViews++;

		// FScene のプリミティブプロキシ列を巡回 (ベースパスと同じ列)。
		// カメラの可視性マップは使わない — 視界外のキャスターでも影は落ちる
		for (const FPrimitiveSceneInfo& info : Scene->GetPrimitives())
		{
			if (info.Proxy && info.Proxy->IsVisible() && info.Proxy->CastsShadow())
			{
				m_CullingStats.NumProcessed++;

				if (m_bFrustumCullingEnabled &&
					!shadowFrustum.IntersectBounds(info.Proxy->GetBounds()))
				{
					m_CullingStats.NumCulled++;
					continue;
				}

				m_CullingStats.NumDrawn++;
				info.Proxy->DrawShadowDepth(m_RHI);
			}
		}
	}

	// ---- DEPTH_WRITE -> SRV ----
	if (m_bUsedCSM)
	{
		cl->ResourceBarrier(1,
			&CD3DX12_RESOURCE_BARRIER::Transition(
				m_CSMTarget.Resource.Get(),
				D3D12_RESOURCE_STATE_DEPTH_WRITE,
				D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE));
	}
	if (m_bUsedLocal)
	{
		cl->ResourceBarrier(1,
			&CD3DX12_RESOURCE_BARRIER::Transition(
				m_LocalTarget.Resource.Get(),
				D3D12_RESOURCE_STATE_DEPTH_WRITE,
				D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE));
	}

	// ---- フル解像度ビューポート / シザーを復元 ----
	// (後続の LinearDepth / デファードパスは BeginFrame のビューポートを
	//  前提にしているため、ここで必ず戻す)
	D3D12_VIEWPORT fullVP{ 0.0f, 0.0f,
		(FLOAT)m_RHI->GetBackBufferWidth(), (FLOAT)m_RHI->GetBackBufferHeight(), 0.0f, 1.0f };
	D3D12_RECT fullSC{ 0, 0,
		(LONG)m_RHI->GetBackBufferWidth(), (LONG)m_RHI->GetBackBufferHeight() };
	cl->RSSetViewports(1, &fullVP);
	cl->RSSetScissorRects(1, &fullSC);
}


// ============================================================
//  BindShadowResources
//  デファードライティング直前に b5 + t14/t15/t16 をバインドする。
// ============================================================
// ------------------------------------------------------------
//  InitDistanceFieldBuffers
//  DF オブジェクトバッファ (t18)。t16 と同じダブルバッファの
//  アップロードヒープ + 永続 Map。
// ------------------------------------------------------------
void FShadowSceneRenderer::InitDistanceFieldBuffers()
{
	ID3D12Device* device = m_RHI->GetDevice();

	const UINT64 bufferSize = sizeof(FDFObjectData) * MAX_DF_OBJECTS;

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
			D3D12_RESOURCE_STATE_GENERIC_READ,
			nullptr,
			IID_PPV_ARGS(&m_DFObjectBuffer[i]));
		assert(SUCCEEDED(hr));
		m_DFObjectBuffer[i]->SetName(L"DFObjectBuffer");

		hr = m_DFObjectBuffer[i]->Map(0, nullptr, (void**)&m_DFObjectPointer[i]);
		assert(SUCCEEDED(hr));
		memset(m_DFObjectPointer[i], 0, bufferSize);

		m_DFObjectSRVIndex[i] = m_RHI->AllocateDescriptor();

		D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
		srvDesc.Format = DXGI_FORMAT_UNKNOWN;
		srvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
		srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		srvDesc.Buffer.FirstElement = 0;
		srvDesc.Buffer.NumElements = MAX_DF_OBJECTS;
		srvDesc.Buffer.StructureByteStride = sizeof(FDFObjectData);
		srvDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;

		device->CreateShaderResourceView(m_DFObjectBuffer[i].Get(), &srvDesc,
			m_RHI->GetCPUDescriptorHandle(m_DFObjectSRVIndex[i]));
	}
}


// ------------------------------------------------------------
//  UpdateDistanceFieldObjects
//  可視 + CastShadow + AffectDistanceField かつ有効な SDF を持つ
//  メッシュプロキシから FDFObjectData (t18) を詰め直す。
//  ボリューム空間: パディング済みローカル境界を [-1,1] に写像。
// ------------------------------------------------------------
void FShadowSceneRenderer::UpdateDistanceFieldObjects(FScene* Scene)
{
	m_DFObjectFrame ^= 1;

	FDFObjectData* dst = m_DFObjectPointer[m_DFObjectFrame];
	unsigned int count = 0;

	if (Scene != nullptr)
	{
		for (const FPrimitiveSceneInfo& info : Scene->GetPrimitives())
		{
			if (count >= MAX_DF_OBJECTS)
			{
				break;
			}

			const FPrimitiveSceneProxy* proxy = info.Proxy.get();
			if (proxy == nullptr || !proxy->IsVisible() ||
				!proxy->CastsShadow() || !proxy->AffectsDistanceField())
			{
				continue;
			}

			const FBXModel* mesh = proxy->GetDistanceFieldMesh();
			if (mesh == nullptr)
			{
				continue;
			}

			const FDistanceFieldMeshInfo& df = mesh->GetDistanceField();

			// ---- ワールド -> ボリューム [-1,1] ----
			XMMATRIX localToWorld = XMLoadFloat4x4(&proxy->GetLocalToWorld());
			XMMATRIX volumeToLocal =
				XMMatrixScaling(df.LocalBoundsExtent.x, df.LocalBoundsExtent.y, df.LocalBoundsExtent.z) *
				XMMatrixTranslation(df.LocalBoundsCenter.x, df.LocalBoundsCenter.y, df.LocalBoundsCenter.z);
			XMMATRIX worldToVolume = XMMatrixInverse(nullptr, volumeToLocal * localToWorld);

			// 距離復元用の最大軸スケール (非均一スケールは最大軸で近似)
			float scaleX = XMVectorGetX(XMVector3Length(localToWorld.r[0]));
			float scaleY = XMVectorGetX(XMVector3Length(localToWorld.r[1]));
			float scaleZ = XMVectorGetX(XMVector3Length(localToWorld.r[2]));
			float maxScale = fmaxf(scaleX, fmaxf(scaleY, scaleZ));

			FDFObjectData& o = dst[count];
			XMStoreFloat4x4(&o.WorldToVolume, XMMatrixTranspose(worldToVolume));
			o.VolumeUVScaleAndDistance = {
				df.UVScale.x, df.UVScale.y, df.UVScale.z,
				df.DistanceScaleLocal * maxScale };
			o.VolumeUVAdd = { df.UVAdd.x, df.UVAdd.y, df.UVAdd.z, 0.0f };

			count++;
		}
	}

	m_NumDFObjects = count;
	m_DirectionalConstant.DFShadowParams0.x = (float)count;
}


void FShadowSceneRenderer::BindShadowResources()
{
	// b5: ディレクショナルシャドウ定数
	m_RHI->SetConstant(RenderManager::CONSTANT_TYPE::SHADOW,
		&m_DirectionalConstant, sizeof(m_DirectionalConstant));

	// t14: CSM アレイ / t15: ローカルアトラス / t16: ローカルシャドウパラメータ
	m_RHI->BindRootTableBySRVIndex(
		(unsigned int)RenderManager::TEXTURE_TYPE::DIRECTIONAL_SHADOW, m_CSMTarget.SRVIndex);
	m_RHI->BindRootTableBySRVIndex(
		(unsigned int)RenderManager::TEXTURE_TYPE::LOCAL_SHADOW, m_LocalTarget.SRVIndex);
	m_RHI->BindRootTableBySRVIndex(
		(unsigned int)RenderManager::TEXTURE_TYPE::LOCAL_SHADOW_DATA,
		m_ShadowParamSRVIndex[m_ShadowParamFrame]);

	// t17: DF アトラス / t18: DF オブジェクトバッファ
	m_RHI->BindRootTableBySRVIndex(
		(unsigned int)RenderManager::TEXTURE_TYPE::DF_ATLAS,
		FDistanceFieldAtlas::Get().GetAtlasSRVIndex());
	m_RHI->BindRootTableBySRVIndex(
		(unsigned int)RenderManager::TEXTURE_TYPE::DF_OBJECTS,
		m_DFObjectSRVIndex[m_DFObjectFrame]);
}
