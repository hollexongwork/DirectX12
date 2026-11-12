#include "Main.h"
#include "RenderManager.h"
#include "HeterogeneousVolumes.h"
#include "D3DX12.h"
#include <fstream>
#include <vector>
#include <algorithm>
#include <cstring>
#include <cmath>

// ============================================================
//  FHeterogeneousVolumeRenderResource
// ============================================================

void FHeterogeneousVolumeRenderResource::ReleaseTexture(FVolumeTexture& Texture)
{
	RenderManager* rhi = RenderManager::GetInstance();
	if (Texture.Resource && rhi)
	{
		// in-flight のコマンドリストが参照している可能性があるので遅延削除キューへ
		rhi->DeferredRelease(std::move(Texture.Resource), (int)Texture.SRVIndex, -1);
		rhi->ReleaseShaderResourceView(Texture.UAVIndex);	// UAV も SRV ヒープの枠
	}
	Texture.Resource.Reset();
	Texture.SRVIndex = 0;
	Texture.UAVIndex = 0;
	Texture.State = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
	Texture.Size = { 0, 0, 0 };
	Texture.Format = DXGI_FORMAT_UNKNOWN;
}

FHeterogeneousVolumeRenderResource::~FHeterogeneousVolumeRenderResource()
{
	ReleaseTexture(Density);
	ReleaseTexture(LightingCache);
}

// ============================================================
//  FHeterogeneousVolumeRenderer
// ============================================================

namespace
{
	// 密度を読む状態 (コンピュートのライティングキャッシュ + ピクセルのレイマーチ)
	const D3D12_RESOURCE_STATES DENSITY_READ_STATE =
		D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

	unsigned long long HashBytes(const void* Data, size_t Size, unsigned long long Hash = 1469598103934665603ull)
	{
		const unsigned char* bytes = static_cast<const unsigned char*>(Data);
		for (size_t i = 0; i < Size; ++i)
		{
			Hash ^= bytes[i];
			Hash *= 1099511628211ull;
		}
		return Hash;
	}

	template <class T>
	unsigned long long HashValue(const T& Value, unsigned long long Hash)
	{
		return HashBytes(&Value, sizeof(T), Hash);
	}
}

FHeterogeneousVolumeRenderer::FHeterogeneousVolumeRenderer(RenderManager* owner)
	: m_Owner(owner)
{
}

FHeterogeneousVolumeRenderer::~FHeterogeneousVolumeRenderer()
{
	for (int i = 0; i < 2; ++i)
	{
		if (m_ParamBuffer[i] && m_ParamPtr[i])
			m_ParamBuffer[i]->Unmap(0, nullptr);
	}
}

ID3D12Device* FHeterogeneousVolumeRenderer::Device()
{
	return m_Owner->GetDevice();
}

ID3D12GraphicsCommandList* FHeterogeneousVolumeRenderer::CommandList()
{
	return m_Owner->GetGraphicsCommandList();
}

ComPtr<ID3D12PipelineState> FHeterogeneousVolumeRenderer::CreateComputePipeline(const char* csoFile)
{
	std::vector<char> cs;
	{
		std::ifstream file(csoFile, std::ios_base::in | std::ios_base::binary);
		if (!file)
		{
			// .cso 欠落: ログを出して無効化する (Release でも落とさない)
			char msg[512];
			sprintf_s(msg, "[HeterogeneousVolumes] shader .cso not found: %s (heterogeneous volumes are disabled)\n", csoFile);
			OutputDebugStringA(msg);
			return nullptr;
		}
		file.seekg(0, std::ios_base::end);
		const int filesize = (int)file.tellg();
		file.seekg(0, std::ios_base::beg);
		if (filesize <= 0)
		{
			return nullptr;
		}
		cs.resize(filesize);
		file.read(&cs[0], filesize);
	}

	D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
	desc.pRootSignature = m_RootSignature.Get();
	desc.CS.pShaderBytecode = cs.data();
	desc.CS.BytecodeLength = cs.size();

	ComPtr<ID3D12PipelineState> pso;
	HRESULT hr = Device()->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pso));
	if (FAILED(hr))
	{
		char msg[512];
		sprintf_s(msg, "[HeterogeneousVolumes] CreateComputePipelineState failed: %s (hr=0x%08X)\n", csoFile, (unsigned int)hr);
		OutputDebugStringA(msg);
		return nullptr;
	}
	return pso;
}

void FHeterogeneousVolumeRenderer::Init()
{
	// ------------------------------------------------------------
	//  コンピュートルートシグネチャ
	//  [0] CBV b0 (ルート CBV)  [1] SRV t0 テーブル  [2] UAV u0 テーブル  s0 = 線形クランプ
	// ------------------------------------------------------------
	{
		D3D12_DESCRIPTOR_RANGE rangeSRV{};
		rangeSRV.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
		rangeSRV.NumDescriptors = 1;
		rangeSRV.BaseShaderRegister = 0;
		rangeSRV.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

		D3D12_DESCRIPTOR_RANGE rangeUAV{};
		rangeUAV.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
		rangeUAV.NumDescriptors = 1;
		rangeUAV.BaseShaderRegister = 0;
		rangeUAV.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

		D3D12_ROOT_PARAMETER params[3]{};
		params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
		params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
		params[0].Descriptor.ShaderRegister = 0;
		params[0].Descriptor.RegisterSpace = 0;

		params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
		params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
		params[1].DescriptorTable.NumDescriptorRanges = 1;
		params[1].DescriptorTable.pDescriptorRanges = &rangeSRV;

		params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
		params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
		params[2].DescriptorTable.NumDescriptorRanges = 1;
		params[2].DescriptorTable.pDescriptorRanges = &rangeUAV;

		D3D12_STATIC_SAMPLER_DESC sampler{};
		sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
		sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
		sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
		sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
		sampler.MaxAnisotropy = 1;
		sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
		sampler.BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
		sampler.MinLOD = 0.0f;
		sampler.MaxLOD = D3D12_FLOAT32_MAX;
		sampler.ShaderRegister = 0;
		sampler.RegisterSpace = 0;
		sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

		D3D12_ROOT_SIGNATURE_DESC rs{};
		rs.NumParameters = _countof(params);
		rs.pParameters = params;
		rs.NumStaticSamplers = 1;
		rs.pStaticSamplers = &sampler;
		rs.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

		ComPtr<ID3DBlob> blob, err;
		HRESULT hr = D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err);
		if (FAILED(hr))
		{
			OutputDebugStringA("[HeterogeneousVolumes] D3D12SerializeRootSignature failed:\n");
			if (err && err->GetBufferPointer())
			{
				OutputDebugStringA((const char*)err->GetBufferPointer());
			}
			assert(false && "HeterogeneousVolumes root signature");
			return;
		}
		hr = Device()->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
			IID_PPV_ARGS(&m_RootSignature));
		assert(SUCCEEDED(hr));
	}

	m_PSOCloudBake = CreateComputePipeline("Shader/cso/HeterogeneousVolumeCloudBake_CS.cso");
	m_PSOFireBake = CreateComputePipeline("Shader/cso/HeterogeneousVolumeFireBake_CS.cso");
	m_PSOLightingCache = CreateComputePipeline("Shader/cso/HeterogeneousVolumeLightingCache_CS.cso");

	// ------------------------------------------------------------
	//  b0 リング x2 (アップロードヒープ、常時マップ)
	// ------------------------------------------------------------
	{
		D3D12_HEAP_PROPERTIES prop{};
		prop.Type = D3D12_HEAP_TYPE_UPLOAD;

		D3D12_RESOURCE_DESC d{};
		d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		d.Width = (UINT64)PARAM_SLOT_SIZE * PARAM_SLOT_COUNT;
		d.Height = 1;
		d.DepthOrArraySize = 1;
		d.MipLevels = 1;
		d.Format = DXGI_FORMAT_UNKNOWN;
		d.SampleDesc.Count = 1;
		d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

		for (int i = 0; i < 2; ++i)
		{
			HRESULT hr = Device()->CreateCommittedResource(&prop, D3D12_HEAP_FLAG_NONE,
				&d, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
				IID_PPV_ARGS(&m_ParamBuffer[i]));
			assert(SUCCEEDED(hr));
			m_ParamBuffer[i]->SetName(L"HeterogeneousVolumeComputeParams");
			m_ParamBuffer[i]->Map(0, nullptr, &m_ParamPtr[i]);
		}
	}
}

void FHeterogeneousVolumeRenderer::CreateVolumeTexture(FHeterogeneousVolumeRenderResource::FVolumeTexture& Out,
	const XMUINT3& Size, DXGI_FORMAT Format, const wchar_t* Name)
{
	FHeterogeneousVolumeRenderResource::ReleaseTexture(Out);

	D3D12_HEAP_PROPERTIES prop{};
	prop.Type = D3D12_HEAP_TYPE_DEFAULT;

	D3D12_RESOURCE_DESC d{};
	d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
	d.Width = Size.x;
	d.Height = Size.y;
	d.DepthOrArraySize = (UINT16)Size.z;
	d.MipLevels = 1;
	d.Format = Format;
	d.SampleDesc.Count = 1;
	d.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
	d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

	HRESULT hr = Device()->CreateCommittedResource(&prop, D3D12_HEAP_FLAG_NONE,
		&d, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
		IID_PPV_ARGS(&Out.Resource));
	if (FAILED(hr))
	{
		OutputDebugStringA("[HeterogeneousVolumes] CreateCommittedResource (Texture3D) failed\n");
		Out.Resource.Reset();
		return;
	}
	Out.Resource->SetName(Name);
	Out.State = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
	Out.Size = Size;
	Out.Format = Format;

	Out.SRVIndex = m_Owner->AllocateDescriptor();
	{
		D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
		srv.Format = Format;
		srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
		srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		srv.Texture3D.MostDetailedMip = 0;
		srv.Texture3D.MipLevels = 1;
		srv.Texture3D.ResourceMinLODClamp = 0.0f;
		Device()->CreateShaderResourceView(Out.Resource.Get(), &srv,
			m_Owner->GetCPUDescriptorHandle(Out.SRVIndex));
	}

	Out.UAVIndex = m_Owner->AllocateDescriptor();
	{
		D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
		uav.Format = Format;
		uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
		uav.Texture3D.MipSlice = 0;
		uav.Texture3D.FirstWSlice = 0;
		uav.Texture3D.WSize = Size.z;
		Device()->CreateUnorderedAccessView(Out.Resource.Get(), nullptr, &uav,
			m_Owner->GetCPUDescriptorHandle(Out.UAVIndex));
	}
}

void FHeterogeneousVolumeRenderer::Transition(FHeterogeneousVolumeRenderResource::FVolumeTexture& Texture,
	D3D12_RESOURCE_STATES NewState)
{
	if (Texture.State == NewState || Texture.Resource == nullptr)
	{
		return;
	}
	CommandList()->ResourceBarrier(1,
		&CD3DX12_RESOURCE_BARRIER::Transition(Texture.Resource.Get(), Texture.State, NewState));
	Texture.State = NewState;
}

D3D12_GPU_VIRTUAL_ADDRESS FHeterogeneousVolumeRenderer::UploadComputeParams(const FHeterogeneousVolumeComputeParams& Params)
{
	const unsigned int frame = m_Owner->GetCurrentFrameIndex() & 1u;
	if (m_ParamSlot >= PARAM_SLOT_COUNT || m_ParamPtr[frame] == nullptr)
	{
		return 0;
	}

	const UINT64 offset = (UINT64)PARAM_SLOT_SIZE * m_ParamSlot;
	std::memcpy(static_cast<char*>(m_ParamPtr[frame]) + offset, &Params, sizeof(Params));
	++m_ParamSlot;
	return m_ParamBuffer[frame]->GetGPUVirtualAddress() + offset;
}

XMFLOAT3 FHeterogeneousVolumeRenderer::ComputeExtent(const XMFLOAT4X4& LocalToWorld)
{
	// 行ベクトル規約: 各行がローカル軸のワールド方向 x スケール
	const XMMATRIX m = XMLoadFloat4x4(&LocalToWorld);
	XMFLOAT3 extent;
	extent.x = (std::max)(XMVectorGetX(XMVector3Length(m.r[0])), 1e-3f);
	extent.y = (std::max)(XMVectorGetX(XMVector3Length(m.r[1])), 1e-3f);
	extent.z = (std::max)(XMVectorGetX(XMVector3Length(m.r[2])), 1e-3f);
	return extent;
}

XMFLOAT3 FHeterogeneousVolumeRenderer::ComputeAspectScale(const XMFLOAT3& Extent)
{
	const float maxExtent = (std::max)(Extent.x, (std::max)(Extent.y, Extent.z));
	// 0.01 刻みに丸める (スケールのドラッグ中に毎フレーム再ベイクしない)
	auto quantize = [&](float v)
		{
			return (std::max)(std::round(v / maxExtent * 100.0f) / 100.0f, 0.05f);
		};
	return XMFLOAT3(quantize(Extent.x), quantize(Extent.y), quantize(Extent.z));
}

float FHeterogeneousVolumeRenderer::ComputeMinVoxelSize(const XMFLOAT3& Extent, const XMUINT3& Resolution)
{
	const float vx = 2.0f * Extent.x / (float)(std::max)(Resolution.x, 1u);
	const float vy = 2.0f * Extent.y / (float)(std::max)(Resolution.y, 1u);
	const float vz = 2.0f * Extent.z / (float)(std::max)(Resolution.z, 1u);
	return (std::max)((std::min)(vx, (std::min)(vy, vz)), 1e-3f);
}

// ------------------------------------------------------------
//  UpdateVolume: 密度ベイク + ライティングキャッシュ (必要な時だけ)
// ------------------------------------------------------------
void FHeterogeneousVolumeRenderer::UpdateVolume(const FHeterogeneousVolumeData& Volume, const VIEW_CONSTANT& View, float Time)
{
	FHeterogeneousVolumeRenderResource* resource = Volume.RenderResource.get();
	if (resource == nullptr)
	{
		return;
	}

	// ---- 密度のソース (雲 = R16F / 炎 = RG16F) ----
	const bool bFire = (Volume.Source == EHeterogeneousVolumeSource::ProceduralFire);
	ID3D12PipelineState* bakePipeline = bFire ? m_PSOFireBake.Get() : m_PSOCloudBake.Get();
	if (bakePipeline == nullptr)
	{
		return;	// .cso 欠落
	}
	const DXGI_FORMAT densityFormat = bFire ? DXGI_FORMAT_R16G16_FLOAT : DXGI_FORMAT_R16_FLOAT;

	ID3D12GraphicsCommandList* cl = CommandList();

	// ---- 解像度 ----
	XMUINT3 densitySize;
	densitySize.x = (std::min)((std::max)(Volume.VolumeResolution.x, 16u), 384u);
	densitySize.y = (std::min)((std::max)(Volume.VolumeResolution.y, 16u), 384u);
	densitySize.z = (std::min)((std::max)(Volume.VolumeResolution.z, 16u), 384u);

	const float downsample = (std::max)(Volume.LightingDownsampleFactor, 1.0f);
	XMUINT3 cacheSize;
	cacheSize.x = (std::max)((unsigned int)std::ceil((float)densitySize.x / downsample), 8u);
	cacheSize.y = (std::max)((unsigned int)std::ceil((float)densitySize.y / downsample), 8u);
	cacheSize.z = (std::max)((unsigned int)std::ceil((float)densitySize.z / downsample), 8u);

	auto sameSize = [](const XMUINT3& a, const XMUINT3& b)
		{
			return a.x == b.x && a.y == b.y && a.z == b.z;
		};

	if (resource->Density.Resource == nullptr || !sameSize(resource->Density.Size, densitySize)
		|| resource->Density.Format != densityFormat)
	{
		CreateVolumeTexture(resource->Density, densitySize, densityFormat, L"HeterogeneousVolumeDensity");
		resource->bDensityValid = false;
		resource->bLightingValid = false;
	}
	if (resource->LightingCache.Resource == nullptr || !sameSize(resource->LightingCache.Size, cacheSize))
	{
		CreateVolumeTexture(resource->LightingCache, cacheSize, DXGI_FORMAT_R16G16B16A16_FLOAT, L"HeterogeneousVolumeLightingCache");
		resource->bLightingValid = false;
	}
	if (resource->Density.Resource == nullptr || resource->LightingCache.Resource == nullptr)
	{
		return;	// 確保失敗
	}

	// ---- 変換 ----
	const XMFLOAT3 extent = ComputeExtent(Volume.LocalToWorld);
	const XMFLOAT3 aspect = ComputeAspectScale(extent);
	const XMMATRIX boxToWorld = XMLoadFloat4x4(&Volume.LocalToWorld);
	const XMMATRIX worldToBox = XMMatrixInverse(nullptr, boxToWorld);

	// ワールドの方向 (1 m) -> ボックス空間の移動量
	auto directionToBox = [&](FXMVECTOR worldDirection)
		{
			XMFLOAT3 out;
			XMStoreFloat3(&out, XMVector3TransformNormal(worldDirection, worldToBox));
			return out;
		};

	// ---- 共通パラメータ ----
	FHeterogeneousVolumeComputeParams p{};
	p.VolumeResolution = { (float)densitySize.x, (float)densitySize.y, (float)densitySize.z, 0.0f };
	p.CacheResolution = { (float)cacheSize.x, (float)cacheSize.y, (float)cacheSize.z, 0.0f };

	const FProceduralCloudParams& cloud = Volume.Cloud;
	const unsigned int numPuffs = cloud.GeneratePuffs(aspect, p.Puffs);
	p.AspectScale = { aspect.x, aspect.y, aspect.z, (float)numPuffs };
	p.ShapeParams = { cloud.SmoothUnion, cloud.BottomHeight * aspect.y, cloud.BottomSoftness, cloud.EdgeSoftness };
	p.BillowParams = { cloud.BillowFrequency, cloud.BillowStrength, (float)cloud.BillowOctaves, cloud.BillowPerlinMix };
	p.DetailParams = { cloud.DetailFrequency, cloud.DetailStrength, (float)cloud.DetailOctaves, cloud.DensityVariation };
	const XMFLOAT3 noiseOffset = cloud.ComputeNoiseOffset();
	p.NoiseOffset = { noiseOffset.x, noiseOffset.y, noiseOffset.z, cloud.BorderFade };

	// ライト (b0 の代表ディレクショナルライト。色が 0 = ライト不在)
	const XMFLOAT4& lightDirection = View.DirectionalLightDirection;
	const XMFLOAT4& lightColor = View.DirectionalLightColor;
	const bool bHasSun = (lightColor.x + lightColor.y + lightColor.z) > 0.0f;
	const XMFLOAT3 sunBox = directionToBox(XMVector3Normalize(XMVectorSet(lightDirection.x, lightDirection.y, lightDirection.z, 0.0f)));
	p.SunDirectionBox = { sunBox.x, sunBox.y, sunBox.z, bHasSun ? 1.0f : 0.0f };

	const float voxelSize = ComputeMinVoxelSize(extent, densitySize);
	const float shadowStep = voxelSize * (std::max)(Volume.ShadowStepFactor, 0.25f);
	const float shadowBias = voxelSize * (std::max)(Volume.ShadowBiasFactor, 0.0f);
	p.ShadowParams = { shadowStep, (float)(std::max)(m_Params.MaxShadowStepCount, 1), shadowBias,
		(float)(std::max)(m_Params.AmbientStepCount, 1) };

	// 上空コーン: 真上 + 55 度傾けた 4 方向 (ワールド Y アップ)
	{
		const float tilt = XMConvertToRadians(55.0f);
		const float s = std::sin(tilt);
		const float c = std::cos(tilt);
		const XMVECTOR directions[5] =
		{
			XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f),
			XMVectorSet(s * 0.7071f, c, s * 0.7071f, 0.0f),
			XMVectorSet(-s * 0.7071f, c, s * 0.7071f, 0.0f),
			XMVectorSet(-s * 0.7071f, c, -s * 0.7071f, 0.0f),
			XMVectorSet(s * 0.7071f, c, -s * 0.7071f, 0.0f),
		};
		for (int i = 0; i < 5; ++i)
		{
			const XMFLOAT3 d = directionToBox(directions[i]);
			p.AmbientDirectionBox[i] = { d.x, d.y, d.z, 0.0f };
		}
		const XMFLOAT3 down = directionToBox(XMVectorSet(0.0f, -1.0f, 0.0f, 0.0f));
		p.GroundDirectionBox = { down.x, down.y, down.z, 0.0f };
	}

	// ---- 炎 (FProceduralFireParams)。ノイズと速度はメートルなので半幅も渡す ----
	const FProceduralFireParams& fire = Volume.Fire;
	const float fireTime = (fire.bAnimate != 0) ? Time : 0.0f;
	p.FireShape = { fire.BaseRadius, fire.TipRadiusScale, fire.TaperPower, fire.FlameHeight };
	p.FireNoise = { fire.NoiseFrequency, fire.NoiseStrength, fire.HeightErosion, (float)fire.NoiseOctaves };
	p.FireMotion = { fire.RiseSpeed, fire.EvolutionSpeed, fire.Turbulence, fireTime };
	p.FireSmoke = { fire.FlameOpacity, fire.SmokeDensity, fire.SmokeStart, fire.HeatGain };
	p.FireExtent = { extent.x, extent.y, extent.z, fire.ComputeNoiseOffset() };

	// ---- キー ----
	unsigned long long densityKey = 0;
	if (bFire)
	{
		// 時間がキーに入る = アニメーション中は毎フレームベイクする
		densityKey = fire.ComputeHash();
		densityKey = HashValue(p.FireMotion, densityKey);
		densityKey = HashValue(p.FireExtent, densityKey);
	}
	else
	{
		densityKey = cloud.ComputeHash();
		densityKey = HashValue(aspect, densityKey);
	}
	densityKey = HashValue(densitySize, densityKey);
	densityKey = HashValue((int)Volume.Source, densityKey);

	unsigned long long lightingKey = densityKey;
	lightingKey = HashValue(cacheSize, lightingKey);
	lightingKey = HashValue(p.SunDirectionBox, lightingKey);
	lightingKey = HashValue(p.ShadowParams, lightingKey);
	lightingKey = HashBytes(p.AmbientDirectionBox, sizeof(p.AmbientDirectionBox), lightingKey);

	const UINT TG = 4;	// HETEROGENEOUS_VOLUME_THREADGROUP_SIZE

	// ---- 1. 密度ベイク ----
	if (!resource->bDensityValid || resource->DensityKey != densityKey)
	{
		const D3D12_GPU_VIRTUAL_ADDRESS params = UploadComputeParams(p);
		if (params == 0)
		{
			return;	// リングが満杯: 次フレームに回す
		}

		Transition(resource->Density, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

		cl->SetPipelineState(bakePipeline);
		cl->SetComputeRootConstantBufferView(0, params);
		// t0 (密度の SRV) はベイクのシェーダが参照しないので設定しない
		cl->SetComputeRootDescriptorTable(2, m_Owner->GetGPUDescriptorHandle(resource->Density.UAVIndex));
		cl->Dispatch((densitySize.x + TG - 1) / TG, (densitySize.y + TG - 1) / TG, (densitySize.z + TG - 1) / TG);

		Transition(resource->Density, DENSITY_READ_STATE);

		resource->DensityKey = densityKey;
		resource->bDensityValid = true;
		resource->bLightingValid = false;
		++m_Stats.NumDensityBakes;
		++m_Stats.TotalDensityBakes;
	}

	// ---- 2. ライティングキャッシュ ----
	if (!resource->bLightingValid || resource->LightingKey != lightingKey)
	{
		const D3D12_GPU_VIRTUAL_ADDRESS params = UploadComputeParams(p);
		if (params == 0)
		{
			return;
		}

		Transition(resource->Density, DENSITY_READ_STATE);
		Transition(resource->LightingCache, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

		cl->SetPipelineState(m_PSOLightingCache.Get());
		cl->SetComputeRootConstantBufferView(0, params);
		cl->SetComputeRootDescriptorTable(1, m_Owner->GetGPUDescriptorHandle(resource->Density.SRVIndex));
		cl->SetComputeRootDescriptorTable(2, m_Owner->GetGPUDescriptorHandle(resource->LightingCache.UAVIndex));
		cl->Dispatch((cacheSize.x + TG - 1) / TG, (cacheSize.y + TG - 1) / TG, (cacheSize.z + TG - 1) / TG);

		Transition(resource->LightingCache, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

		resource->LightingKey = lightingKey;
		resource->bLightingValid = true;
		++m_Stats.NumLightingCacheUpdates;
		++m_Stats.TotalLightingCacheUpdates;
	}
}

void FHeterogeneousVolumeRenderer::PrepareVolumes(const std::vector<const FHeterogeneousVolumeData*>& Volumes,
	const VIEW_CONSTANT& View, float Time)
{
	m_Stats.NumVolumes = 0;
	m_Stats.NumDensityBakes = 0;
	m_Stats.NumLightingCacheUpdates = 0;
	m_Stats.VolumeMemoryBytes = 0;

	if (!m_Params.bEnable || Volumes.empty()
		|| m_RootSignature == nullptr || m_PSOLightingCache == nullptr)
	{
		return;
	}

	ID3D12GraphicsCommandList* cl = CommandList();
	ID3D12DescriptorHeap* heap = m_Owner->GetSRVDescriptorHeap();
	cl->SetDescriptorHeaps(1, &heap);
	cl->SetComputeRootSignature(m_RootSignature.Get());

	m_ParamSlot = 0;

	for (const FHeterogeneousVolumeData* volume : Volumes)
	{
		if (volume != nullptr)
		{
			UpdateVolume(*volume, View, Time);
		}
	}
}

// ------------------------------------------------------------
//  RenderHeterogeneousVolumes: 後→前にレイマーチして SceneColor へ合成
// ------------------------------------------------------------
void FHeterogeneousVolumeRenderer::RenderHeterogeneousVolumes(const std::vector<const FHeterogeneousVolumeData*>& Volumes,
	const VIEW_CONSTANT& View, const VERTEX_BUFFER* ScreenQuad, unsigned int DepthSRVIndex)
{
	if (!m_Params.bEnable || Volumes.empty() || ScreenQuad == nullptr
		|| !m_Owner->HasPipelineState("HeterogeneousVolume"))
	{
		return;
	}

	// ---- 描けるボリュームを後→前に並べる (カメラから境界中心までの距離) ----
	struct FSortedVolume
	{
		const FHeterogeneousVolumeData* Volume;
		float DistanceSq;
	};
	std::vector<FSortedVolume> sorted;
	sorted.reserve(Volumes.size());

	const XMVECTOR cameraOrigin = XMVectorSet(View.WorldCameraOrigin.x, View.WorldCameraOrigin.y, View.WorldCameraOrigin.z, 0.0f);
	for (const FHeterogeneousVolumeData* volume : Volumes)
	{
		if (volume == nullptr || volume->RenderResource == nullptr
			|| !volume->RenderResource->bDensityValid || !volume->RenderResource->bLightingValid)
		{
			continue;	// 今フレームにベイクできなかった (リング満杯など)
		}
		const XMVECTOR toVolume = XMVectorSubtract(XMLoadFloat3(&volume->Bounds.Origin), cameraOrigin);
		sorted.push_back({ volume, XMVectorGetX(XMVector3LengthSq(toVolume)) });
	}
	if (sorted.empty())
	{
		return;
	}
	std::sort(sorted.begin(), sorted.end(),
		[](const FSortedVolume& a, const FSortedVolume& b) { return a.DistanceSq > b.DistanceSq; });

	ID3D12GraphicsCommandList* cl = CommandList();

	m_Owner->SetPipelineState("HeterogeneousVolume");
	m_Owner->BindRootTableBySRVIndex((unsigned int)RenderManager::TEXTURE_TYPE::DEPTH, DepthSRVIndex);
	m_Owner->SetVertexBuffer(ScreenQuad);
	cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);

	// ワールド -> クリップ (VIEW 定数は転置済みなので戻す)。シザー矩形の計算用
	const XMMATRIX viewProjection =
		XMMatrixTranspose(XMLoadFloat4x4(&View.View)) * XMMatrixTranspose(XMLoadFloat4x4(&View.Projection));
	const float viewportWidth = (float)m_Owner->GetDefaultViewportWidth();
	const float viewportHeight = (float)m_Owner->GetDefaultViewportHeight();
	const float nearPlane = View.NearFar.x;

	for (const FSortedVolume& entry : sorted)
	{
		const FHeterogeneousVolumeData& volume = *entry.Volume;
		FHeterogeneousVolumeRenderResource* resource = volume.RenderResource.get();
		const FHeterogeneousVolumeMaterial& material = volume.Material;

		const XMMATRIX boxToWorld = XMLoadFloat4x4(&volume.LocalToWorld);
		const XMMATRIX worldToBox = XMMatrixInverse(nullptr, boxToWorld);
		const XMFLOAT3 extent = ComputeExtent(volume.LocalToWorld);

		// ---- シザー: OBB の 8 頂点の画面矩形 (ニアをまたぐ時は全画面) ----
		D3D12_RECT scissor{ 0, 0, (LONG)viewportWidth, (LONG)viewportHeight };
		{
			float minX = 1.0e9f, minY = 1.0e9f, maxX = -1.0e9f, maxY = -1.0e9f;
			bool bCrossesNear = false;
			for (int i = 0; i < 8; ++i)
			{
				const XMVECTOR corner = XMVectorSet((i & 1) ? 1.0f : -1.0f, (i & 2) ? 1.0f : -1.0f, (i & 4) ? 1.0f : -1.0f, 1.0f);
				const XMVECTOR clip = XMVector4Transform(XMVector3TransformCoord(corner, boxToWorld), viewProjection);
				const float w = XMVectorGetW(clip);
				if (w <= nearPlane)
				{
					bCrossesNear = true;
					break;
				}
				const float px = (XMVectorGetX(clip) / w * 0.5f + 0.5f) * viewportWidth;
				const float py = (0.5f - XMVectorGetY(clip) / w * 0.5f) * viewportHeight;
				minX = (std::min)(minX, px);
				maxX = (std::max)(maxX, px);
				minY = (std::min)(minY, py);
				maxY = (std::max)(maxY, py);
			}
			if (!bCrossesNear)
			{
				// TAA ジッタと丸めの分を 2 px 広げる
				scissor.left = (LONG)(std::max)(std::floor(minX) - 2.0f, 0.0f);
				scissor.top = (LONG)(std::max)(std::floor(minY) - 2.0f, 0.0f);
				scissor.right = (LONG)(std::min)(std::ceil(maxX) + 2.0f, viewportWidth);
				scissor.bottom = (LONG)(std::min)(std::ceil(maxY) + 2.0f, viewportHeight);
				if (scissor.right <= scissor.left || scissor.bottom <= scissor.top)
				{
					continue;	// 画面外
				}
			}
		}
		cl->RSSetScissorRects(1, &scissor);

		// ---- b2: パス定数 ----
		FHeterogeneousVolumePassConstant c{};
		XMStoreFloat4x4(&c.WorldToBox, XMMatrixTranspose(worldToBox));
		c.Albedo = { material.Albedo.x, material.Albedo.y, material.Albedo.z, (std::max)(material.Extinction, 0.0f) };
		c.Emissive = { (std::max)(material.Emissive.x, 0.0f), (std::max)(material.Emissive.y, 0.0f),
			(std::max)(material.Emissive.z, 0.0f), (std::max)(material.AmbientIntensity, 0.0f) };

		const float stepSize = ComputeMinVoxelSize(extent, resource->Density.Size) * (std::max)(volume.StepFactor, 0.25f);
		c.MarchParams = { stepSize, (float)(std::max)(m_Params.MaxStepCount, 1),
			m_Params.bJitter ? 1.0f : 0.0f, m_Params.bApplyFog ? 1.0f : 0.0f };

		auto clampG = [](float g) { return (g < -0.95f) ? -0.95f : ((g > 0.95f) ? 0.95f : g); };
		c.PhaseParams = { clampG(material.PhaseGForward), clampG(material.PhaseGBackward),
			material.PhaseForwardWeight, m_Params.bIndirectLighting ? 1.0f : 0.0f };
		c.MultiScatter = { material.MultiScatteringContribution, material.MultiScatteringOcclusion,
			material.MultiScatteringEccentricity, (float)material.MultiScatteringOctaves };
		c.LightParams = { (std::max)(material.DirectionalLightIntensityScale, 0.0f),
			(std::max)(material.AmbientOcclusionScale, 0.0f), (std::max)(material.GroundBounce, 0.0f), 0.0f };
		c.Blackbody = { (std::max)(material.BlackbodyTemperatureMin, 300.0f),
			(std::max)(material.BlackbodyTemperatureMax, 300.0f), (std::max)(material.BlackbodyIntensity, 0.0f), 0.0f };

		m_Owner->SetConstant(RenderManager::CONSTANT_TYPE::MATERIAL, &c, sizeof(c));

		// ---- t41 / t42 ----
		m_Owner->BindRootTableBySRVIndex(
			(unsigned int)RenderManager::TEXTURE_TYPE::HETEROGENEOUS_VOLUME_DENSITY, resource->Density.SRVIndex);
		m_Owner->BindRootTableBySRVIndex(
			(unsigned int)RenderManager::TEXTURE_TYPE::HETEROGENEOUS_VOLUME_LIGHTING_CACHE, resource->LightingCache.SRVIndex);

		cl->DrawInstanced(4, 1, 0, 0);

		++m_Stats.NumVolumes;
		m_Stats.VolumeMemoryBytes +=
			(unsigned long long)resource->Density.Size.x * resource->Density.Size.y * resource->Density.Size.z
			* ((resource->Density.Format == DXGI_FORMAT_R16G16_FLOAT) ? 4ull : 2ull)
			+ (unsigned long long)resource->LightingCache.Size.x * resource->LightingCache.Size.y * resource->LightingCache.Size.z * 8ull;
	}

	// シザーを既定 (レンダー解像度全体) へ戻す
	m_Owner->RestoreDefaultViewport();
}
