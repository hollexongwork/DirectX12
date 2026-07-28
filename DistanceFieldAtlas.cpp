#include "Main.h"
#include "RenderManager.h"
#include "DistanceFieldAtlas.h"

#include "D3DX12.h"

#include <cstdio>
#include <fstream>

// ============================================================
//  ベイク用定数バッファ (HLSL DistanceFieldBake_CS.hlsl と 1:1)
// ============================================================
namespace
{
	struct DF_BAKE_PARAMS
	{
		XMFLOAT3     VolumeMin;				// ボリューム最小コーナー (ローカル)
		float        InvNormalizeExtent;	// 1 / 正規化半幅
		XMFLOAT3     VoxelSize;				// ボクセルサイズ (ローカル)
		unsigned int NumTriangles;
		unsigned int VolumeResolution;		// = DF_VOLUME_RES
		unsigned int _pad0;
		unsigned int _pad1;
		unsigned int _pad2;
	};
	static_assert(sizeof(DF_BAKE_PARAMS) == 48, "DF_BAKE_PARAMS must mirror HLSL cbuffer");

	// float -> half (IEEE 754 binary16, 最近接丸め簡易版)
	unsigned short FloatToHalf(float Value)
	{
		unsigned int bits;
		memcpy(&bits, &Value, sizeof(bits));

		const unsigned int sign = (bits >> 16) & 0x8000u;
		int exponent = (int)((bits >> 23) & 0xFFu) - 127 + 15;
		unsigned int mantissa = bits & 0x7FFFFFu;

		if (exponent <= 0)
		{
			return (unsigned short)sign;					// 非正規は 0 へ
		}
		if (exponent >= 31)
		{
			return (unsigned short)(sign | 0x7BFFu);		// 最大値へクランプ
		}
		return (unsigned short)(sign | ((unsigned int)exponent << 10) | (mantissa >> 13));
	}
}


// ============================================================
//  Get (グローバルアトラス)
// ============================================================
FDistanceFieldAtlas& FDistanceFieldAtlas::Get()
{
	static FDistanceFieldAtlas* s_Instance = nullptr;
	if (s_Instance == nullptr)
	{
		s_Instance = new FDistanceFieldAtlas(RenderManager::GetInstance());
	}
	return *s_Instance;
}


// ============================================================
//  Lifetime
// ============================================================
FDistanceFieldAtlas::FDistanceFieldAtlas(RenderManager* RHI)
	: m_RHI(RHI)
{
	ID3D12Device* device = m_RHI->GetDevice();

	// ---- ベイク用ルートシグネチャ (b0 / t0 / u0) ----
	{
		D3D12_DESCRIPTOR_RANGE rangeSRV{};
		rangeSRV.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
		rangeSRV.NumDescriptors = 1;
		rangeSRV.BaseShaderRegister = 0;	// t0
		rangeSRV.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

		D3D12_DESCRIPTOR_RANGE rangeUAV{};
		rangeUAV.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
		rangeUAV.NumDescriptors = 1;
		rangeUAV.BaseShaderRegister = 0;	// u0
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

		D3D12_ROOT_SIGNATURE_DESC rs{};
		rs.NumParameters = _countof(params);
		rs.pParameters = params;
		rs.NumStaticSamplers = 0;
		rs.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

		ComPtr<ID3DBlob> blob, err;
		HRESULT hr = D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err);
		assert(SUCCEEDED(hr));
		hr = device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
			IID_PPV_ARGS(&m_BakeRootSignature));
		assert(SUCCEEDED(hr));
	}

	// ---- ベイク用 PSO ----
	{
		std::ifstream file("Shader/cso/DistanceFieldBake_CS.cso", std::ios_base::in | std::ios_base::binary);
		assert(file && "DistanceFieldBake_CS.cso not found");

		file.seekg(0, std::ios_base::end);
		int filesize = (int)file.tellg();
		file.seekg(0, std::ios_base::beg);

		std::vector<char> cs(filesize);
		file.read(cs.data(), filesize);
		file.close();

		D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
		desc.pRootSignature = m_BakeRootSignature.Get();
		desc.CS.pShaderBytecode = cs.data();
		desc.CS.BytecodeLength = cs.size();

		HRESULT hr = device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&m_BakePSO));
		assert(SUCCEEDED(hr));
	}

	// ---- ベイクパラメータ CB (UPLOAD, 256B) ----
	{
		D3D12_HEAP_PROPERTIES prop{};
		prop.Type = D3D12_HEAP_TYPE_UPLOAD;

		D3D12_RESOURCE_DESC d{};
		d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		d.Width = 256;
		d.Height = 1;
		d.DepthOrArraySize = 1;
		d.MipLevels = 1;
		d.Format = DXGI_FORMAT_UNKNOWN;
		d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		d.SampleDesc.Count = 1;

		HRESULT hr = device->CreateCommittedResource(&prop, D3D12_HEAP_FLAG_NONE, &d,
			D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_BakeParamBuffer));
		assert(SUCCEEDED(hr));
		m_BakeParamBuffer->SetName(L"DFBakeParams");
	}

	// ---- アトラス本体 (Texture3D R16_FLOAT) ----
	{
		D3D12_RESOURCE_DESC desc{};
		desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
		desc.Width = DF_VOLUME_RES * MAX_DF_MESHES;
		desc.Height = DF_VOLUME_RES;
		desc.DepthOrArraySize = (UINT16)DF_VOLUME_RES;
		desc.MipLevels = 1;
		desc.Format = DXGI_FORMAT_R16_FLOAT;
		desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
		desc.SampleDesc.Count = 1;
		desc.Flags = D3D12_RESOURCE_FLAG_NONE;

		HRESULT hr = device->CreateCommittedResource(
			&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
			D3D12_HEAP_FLAG_NONE,
			&desc,
			D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
			nullptr,
			IID_PPV_ARGS(&m_AtlasTexture));
		assert(SUCCEEDED(hr));
		m_AtlasTexture->SetName(L"DistanceFieldAtlas");

		m_AtlasSRVIndex = m_RHI->AllocateDescriptor();

		D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
		srv.Format = DXGI_FORMAT_R16_FLOAT;
		srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
		srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		srv.Texture3D.MostDetailedMip = 0;
		srv.Texture3D.MipLevels = 1;

		device->CreateShaderResourceView(m_AtlasTexture.Get(), &srv,
			m_RHI->GetCPUDescriptorHandle(m_AtlasSRVIndex));
	}

	// ---- アトラスアップロード用スクラッチ (RowPitch 256 x 64 x 64 = 1MB) ----
	{
		D3D12_HEAP_PROPERTIES prop{};
		prop.Type = D3D12_HEAP_TYPE_UPLOAD;

		D3D12_RESOURCE_DESC d{};
		d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		d.Width = 256ull * DF_VOLUME_RES * DF_VOLUME_RES;
		d.Height = 1;
		d.DepthOrArraySize = 1;
		d.MipLevels = 1;
		d.Format = DXGI_FORMAT_UNKNOWN;
		d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		d.SampleDesc.Count = 1;

		HRESULT hr = device->CreateCommittedResource(&prop, D3D12_HEAP_FLAG_NONE, &d,
			D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_UploadScratch));
		assert(SUCCEEDED(hr));
		m_UploadScratch->SetName(L"DFAtlasUploadScratch");
	}
}


FDistanceFieldAtlas::~FDistanceFieldAtlas()
{
	if (m_RHI)
	{
		m_RHI->ReleaseShaderResourceView(m_AtlasSRVIndex);
	}
}


// ============================================================
//  キャッシュ入出力 (<fbx>.mdf : "MDF1" + 解像度 + float ボクセル列)
// ============================================================
bool FDistanceFieldAtlas::TryLoadCache(const std::string& CachePath, std::vector<float>& OutVoxels) const
{
	std::ifstream file(CachePath, std::ios_base::in | std::ios_base::binary);
	if (!file) return false;

	char magic[4]{};
	unsigned int resolution = 0;
	file.read(magic, 4);
	file.read((char*)&resolution, sizeof(resolution));

	if (memcmp(magic, "MDF1", 4) != 0 || resolution != DF_VOLUME_RES)
	{
		return false;	// フォーマット / 解像度不一致 -> 再ベイク
	}

	const size_t count = (size_t)DF_VOLUME_RES * DF_VOLUME_RES * DF_VOLUME_RES;
	OutVoxels.resize(count);
	file.read((char*)OutVoxels.data(), count * sizeof(float));
	return (bool)file;
}


void FDistanceFieldAtlas::SaveCache(const std::string& CachePath, const std::vector<float>& Voxels) const
{
	std::ofstream file(CachePath, std::ios_base::out | std::ios_base::binary);
	if (!file) return;

	const unsigned int resolution = DF_VOLUME_RES;
	file.write("MDF1", 4);
	file.write((const char*)&resolution, sizeof(resolution));
	file.write((const char*)Voxels.data(), Voxels.size() * sizeof(float));
}


// ============================================================
//  BakeOnGPU
//  三角形列をアップロード -> Dispatch -> リードバックで CPU へ回収。
//  ロード時専用 (FlushAndResetCommandList で即時完了させる)。
// ============================================================
void FDistanceFieldAtlas::BakeOnGPU(const std::vector<XMFLOAT3>& TriangleVertices,
	const XMFLOAT3& VolumeMin, const XMFLOAT3& VoxelSize,
	float InvNormalizeExtent, std::vector<float>& OutVoxels)
{
	ID3D12Device* device = m_RHI->GetDevice();
	ID3D12GraphicsCommandList* cl = m_RHI->GetGraphicsCommandList();

	const size_t voxelCount = (size_t)DF_VOLUME_RES * DF_VOLUME_RES * DF_VOLUME_RES;
	const unsigned int numTriangles = (unsigned int)(TriangleVertices.size() / 3);

	// ---- 三角形バッファ (UPLOAD + StructuredBuffer SRV) ----
	ComPtr<ID3D12Resource> triangleBuffer;
	{
		D3D12_HEAP_PROPERTIES prop{};
		prop.Type = D3D12_HEAP_TYPE_UPLOAD;

		D3D12_RESOURCE_DESC d{};
		d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		d.Width = sizeof(XMFLOAT3) * TriangleVertices.size();
		d.Height = 1;
		d.DepthOrArraySize = 1;
		d.MipLevels = 1;
		d.Format = DXGI_FORMAT_UNKNOWN;
		d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		d.SampleDesc.Count = 1;

		HRESULT hr = device->CreateCommittedResource(&prop, D3D12_HEAP_FLAG_NONE, &d,
			D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&triangleBuffer));
		assert(SUCCEEDED(hr));

		void* ptr = nullptr;
		triangleBuffer->Map(0, nullptr, &ptr);
		memcpy(ptr, TriangleVertices.data(), sizeof(XMFLOAT3) * TriangleVertices.size());
		triangleBuffer->Unmap(0, nullptr);
	}

	unsigned int triangleSRV = m_RHI->AllocateDescriptor();
	{
		D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
		srv.Format = DXGI_FORMAT_UNKNOWN;
		srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
		srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		srv.Buffer.FirstElement = 0;
		srv.Buffer.NumElements = (UINT)TriangleVertices.size();
		srv.Buffer.StructureByteStride = sizeof(XMFLOAT3);
		srv.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;

		device->CreateShaderResourceView(triangleBuffer.Get(), &srv,
			m_RHI->GetCPUDescriptorHandle(triangleSRV));
	}

	// ---- 出力ボリュームバッファ (DEFAULT + UAV) ----
	ComPtr<ID3D12Resource> volumeBuffer;
	{
		D3D12_RESOURCE_DESC d{};
		d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		d.Width = voxelCount * sizeof(float);
		d.Height = 1;
		d.DepthOrArraySize = 1;
		d.MipLevels = 1;
		d.Format = DXGI_FORMAT_UNKNOWN;
		d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		d.SampleDesc.Count = 1;
		d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

		HRESULT hr = device->CreateCommittedResource(
			&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
			D3D12_HEAP_FLAG_NONE, &d,
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&volumeBuffer));
		assert(SUCCEEDED(hr));
	}

	unsigned int volumeUAV = m_RHI->AllocateDescriptor();
	{
		D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
		uav.Format = DXGI_FORMAT_UNKNOWN;
		uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
		uav.Buffer.FirstElement = 0;
		uav.Buffer.NumElements = (UINT)voxelCount;
		uav.Buffer.StructureByteStride = sizeof(float);

		device->CreateUnorderedAccessView(volumeBuffer.Get(), nullptr, &uav,
			m_RHI->GetCPUDescriptorHandle(volumeUAV));
	}

	// ---- リードバック ----
	ComPtr<ID3D12Resource> readback;
	{
		D3D12_HEAP_PROPERTIES prop{};
		prop.Type = D3D12_HEAP_TYPE_READBACK;

		D3D12_RESOURCE_DESC d{};
		d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		d.Width = voxelCount * sizeof(float);
		d.Height = 1;
		d.DepthOrArraySize = 1;
		d.MipLevels = 1;
		d.Format = DXGI_FORMAT_UNKNOWN;
		d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		d.SampleDesc.Count = 1;

		HRESULT hr = device->CreateCommittedResource(&prop, D3D12_HEAP_FLAG_NONE, &d,
			D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback));
		assert(SUCCEEDED(hr));
	}

	// ---- ベイクパラメータ ----
	{
		DF_BAKE_PARAMS p{};
		p.VolumeMin = VolumeMin;
		p.InvNormalizeExtent = InvNormalizeExtent;
		p.VoxelSize = VoxelSize;
		p.NumTriangles = numTriangles;
		p.VolumeResolution = DF_VOLUME_RES;

		void* ptr = nullptr;
		m_BakeParamBuffer->Map(0, nullptr, &ptr);
		memcpy(ptr, &p, sizeof(p));
		m_BakeParamBuffer->Unmap(0, nullptr);
	}

	// ---- 記録 -> Dispatch -> コピー -> 即時完了 ----
	{
		ID3D12DescriptorHeap* dh[] = { m_RHI->GetSRVDescriptorHeap() };
		cl->SetDescriptorHeaps(_countof(dh), dh);
		cl->SetComputeRootSignature(m_BakeRootSignature.Get());
		cl->SetPipelineState(m_BakePSO.Get());
		cl->SetComputeRootConstantBufferView(0, m_BakeParamBuffer->GetGPUVirtualAddress());
		cl->SetComputeRootDescriptorTable(1, m_RHI->GetGPUDescriptorHandle(triangleSRV));
		cl->SetComputeRootDescriptorTable(2, m_RHI->GetGPUDescriptorHandle(volumeUAV));

		const unsigned int groups = DF_VOLUME_RES / 4;	// numthreads(4,4,4)
		cl->Dispatch(groups, groups, groups);

		D3D12_RESOURCE_BARRIER uavBarrier{};
		uavBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
		uavBarrier.UAV.pResource = volumeBuffer.Get();
		cl->ResourceBarrier(1, &uavBarrier);

		cl->ResourceBarrier(1,
			&CD3DX12_RESOURCE_BARRIER::Transition(volumeBuffer.Get(),
				D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE));

		cl->CopyBufferRegion(readback.Get(), 0, volumeBuffer.Get(), 0, voxelCount * sizeof(float));

		m_RHI->FlushAndResetCommandList();
	}

	// ---- 読み出し ----
	OutVoxels.resize(voxelCount);
	{
		void* ptr = nullptr;
		readback->Map(0, nullptr, &ptr);
		memcpy(OutVoxels.data(), ptr, voxelCount * sizeof(float));
		D3D12_RANGE noWrite{ 0, 0 };
		readback->Unmap(0, &noWrite);
	}

	m_RHI->ReleaseShaderResourceView(triangleSRV);
	m_RHI->ReleaseShaderResourceView(volumeUAV);
}


// ============================================================
//  UploadSlot
//  float ボクセル列 -> half 変換 -> アトラススロットへコピー
// ============================================================
void FDistanceFieldAtlas::UploadSlot(unsigned int SlotIndex, const std::vector<float>& Voxels)
{
	ID3D12GraphicsCommandList* cl = m_RHI->GetGraphicsCommandList();

	const unsigned int rowPitch = 256;									// 64 * 2B -> 256 アライン
	const unsigned int slicePitch = rowPitch * DF_VOLUME_RES;

	// ---- スクラッチへ half で書き込み ----
	{
		unsigned char* base = nullptr;
		m_UploadScratch->Map(0, nullptr, (void**)&base);

		for (unsigned int z = 0; z < DF_VOLUME_RES; ++z)
		{
			for (unsigned int y = 0; y < DF_VOLUME_RES; ++y)
			{
				unsigned short* row = (unsigned short*)(base + (size_t)z * slicePitch + (size_t)y * rowPitch);
				const float* src = &Voxels[((size_t)z * DF_VOLUME_RES + y) * DF_VOLUME_RES];
				for (unsigned int x = 0; x < DF_VOLUME_RES; ++x)
				{
					row[x] = FloatToHalf(src[x]);
				}
			}
		}
		m_UploadScratch->Unmap(0, nullptr);
	}

	// ---- アトラスへコピー (PSR -> COPY_DEST -> PSR) ----
	cl->ResourceBarrier(1,
		&CD3DX12_RESOURCE_BARRIER::Transition(m_AtlasTexture.Get(),
			D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST));

	D3D12_TEXTURE_COPY_LOCATION src{};
	src.pResource = m_UploadScratch.Get();
	src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
	src.PlacedFootprint.Offset = 0;
	src.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R16_FLOAT;
	src.PlacedFootprint.Footprint.Width = DF_VOLUME_RES;
	src.PlacedFootprint.Footprint.Height = DF_VOLUME_RES;
	src.PlacedFootprint.Footprint.Depth = DF_VOLUME_RES;
	src.PlacedFootprint.Footprint.RowPitch = rowPitch;

	D3D12_TEXTURE_COPY_LOCATION dst{};
	dst.pResource = m_AtlasTexture.Get();
	dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	dst.SubresourceIndex = 0;

	cl->CopyTextureRegion(&dst, SlotIndex * DF_VOLUME_RES, 0, 0, &src, nullptr);

	cl->ResourceBarrier(1,
		&CD3DX12_RESOURCE_BARRIER::Transition(m_AtlasTexture.Get(),
			D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE));

	// スクラッチを次のメッシュで使い回すため即時完了させる
	m_RHI->FlushAndResetCommandList();
}


// ============================================================
//  AddMesh
// ============================================================
void FDistanceFieldAtlas::AddMesh(const char* FilePath,
	const std::vector<XMFLOAT3>& TriangleVertices,
	const XMFLOAT3& BoundsMin, const XMFLOAT3& BoundsMax,
	FDistanceFieldMeshInfo& OutInfo)
{
	OutInfo = FDistanceFieldMeshInfo{};

	if (TriangleVertices.size() < 3)
	{
		return;
	}
	if (m_NumAllocatedSlots >= MAX_DF_MESHES)
	{
		OutputDebugStringA("[DistanceFieldAtlas] slot full, mesh skipped\n");
		return;
	}

	// ---- パディング済み境界 (spread) ----
	XMFLOAT3 center{
		(BoundsMin.x + BoundsMax.x) * 0.5f,
		(BoundsMin.y + BoundsMax.y) * 0.5f,
		(BoundsMin.z + BoundsMax.z) * 0.5f };
	XMFLOAT3 halfExtent{
		(BoundsMax.x - BoundsMin.x) * 0.5f,
		(BoundsMax.y - BoundsMin.y) * 0.5f,
		(BoundsMax.z - BoundsMin.z) * 0.5f };

	const float maxExtentRaw = fmaxf(fmaxf(halfExtent.x, halfExtent.y), fmaxf(halfExtent.z, 0.001f));
	const float border = maxExtentRaw * DF_BORDER_FRACTION;
	halfExtent = { halfExtent.x + border, halfExtent.y + border, halfExtent.z + border };

	const float normalizeExtent = fmaxf(fmaxf(halfExtent.x, halfExtent.y), halfExtent.z);

	XMFLOAT3 volumeMin{ center.x - halfExtent.x, center.y - halfExtent.y, center.z - halfExtent.z };
	XMFLOAT3 voxelSize{
		halfExtent.x * 2.0f / (float)DF_VOLUME_RES,
		halfExtent.y * 2.0f / (float)DF_VOLUME_RES,
		halfExtent.z * 2.0f / (float)DF_VOLUME_RES };

	// ---- キャッシュ -> なければ GPU ベイク ----
	const std::string cachePath = std::string(FilePath) + ".mdf";

	std::vector<float> voxels;
	if (!TryLoadCache(cachePath, voxels))
	{
		char msg[512];
		sprintf_s(msg, "[DistanceFieldAtlas] baking SDF: %s (%u tris)\n",
			FilePath, (unsigned int)(TriangleVertices.size() / 3));
		OutputDebugStringA(msg);

		BakeOnGPU(TriangleVertices, volumeMin, voxelSize, 1.0f / normalizeExtent, voxels);
		SaveCache(cachePath, voxels);
	}

	// ---- アトラススロットへ格納 ----
	const unsigned int slot = m_NumAllocatedSlots;
	UploadSlot(slot, voxels);
	m_NumAllocatedSlots++;

	// ---- メッシュ情報 (半テクセル内側マッピングでスロット間ブリード防止) ----
	const float atlasWidth = (float)(DF_VOLUME_RES * MAX_DF_MESHES);
	const float res = (float)DF_VOLUME_RES;

	OutInfo.bValid = true;
	OutInfo.SlotIndex = (int)slot;

	// ベイクはボクセル中心整列 (テクセル i の中心 = uvw (i+0.5)/RES) の
	// ため、[0,1] の uvw をそのままテクセル座標へ対応させる恒等
	// マッピングが正しい (旧実装の (RES-1)/RES + 0.5/RES はコーナー
	// 整列 LUT 用で、SDF 表面が最大半ボクセル歪み影の位置ずれを生んだ)。
	// スロット間のリニア補間ブリード防止はシェーダ側の uvw クランプ
	// (半テクセル内側) が担う (DistanceFieldShadowing.hlsl)
	OutInfo.UVScale = { res / atlasWidth, 1.0f, 1.0f };
	OutInfo.UVAdd = { ((float)slot * res) / atlasWidth, 0.0f, 0.0f };
	OutInfo.LocalBoundsCenter = center;
	OutInfo.LocalBoundsExtent = halfExtent;
	OutInfo.DistanceScaleLocal = normalizeExtent;
}
