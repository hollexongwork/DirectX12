#include "Main.h"
#include "RenderManager.h"
#include "LumenHardwareRayTracing.h"

#include "D3DX12.h"

// ============================================================
//  Lifetime
// ============================================================
FLumenHardwareRayTracing::FLumenHardwareRayTracing(RenderManager* RHI)
	: m_RHI(RHI)
{
}


FLumenHardwareRayTracing::~FLumenHardwareRayTracing()
{
	for (int i = 0; i < 2; i++)
	{
		if (m_InstanceBuffer[i])
		{
			m_InstanceBuffer[i]->Unmap(0, nullptr);
		}
	}

	if (m_TLAS) { m_RHI->DeferredRelease(m_TLAS); m_TLAS = nullptr; }
	if (m_TLASScratch) { m_RHI->DeferredRelease(m_TLASScratch); m_TLASScratch = nullptr; }
}


// ============================================================
//  Init
// ============================================================
void FLumenHardwareRayTracing::Init(unsigned int MaxInstances)
{
	if (!m_RHI->IsRayTracingSupported() || MaxInstances == 0)
	{
		return;
	}

	// 加速構造のビルドには Device5 と GraphicsCommandList4 の両方が要る
	// (FBXModel::BuildRayTracingGeometry と同じ判定にそろえる)
	ID3D12Device5* device5 = m_RHI->GetDevice5();
	if (device5 == nullptr || m_RHI->GetGraphicsCommandList4() == nullptr)
	{
		return;
	}

	m_MaxInstances = MaxInstances;

	// ---- インスタンスバッファ (ダブルバッファ + 永続 Map) ----
	for (int i = 0; i < 2; i++)
	{
		const UINT64 size = sizeof(D3D12_RAYTRACING_INSTANCE_DESC) * MaxInstances;

		HRESULT hr = m_RHI->GetDevice()->CreateCommittedResource(
			&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD),
			D3D12_HEAP_FLAG_NONE,
			&CD3DX12_RESOURCE_DESC::Buffer(size),
			D3D12_RESOURCE_STATE_GENERIC_READ,
			nullptr,
			IID_PPV_ARGS(&m_InstanceBuffer[i]));
		assert(SUCCEEDED(hr));

		hr = m_InstanceBuffer[i]->Map(0, nullptr, (void**)&m_InstancePointer[i]);
		assert(SUCCEEDED(hr));
		memset(m_InstancePointer[i], 0, (size_t)size);
	}

	// ---- TLAS / スクラッチ (最大インスタンス数でプレビルド) ----
	D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs{};
	inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
	inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;
	inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
	inputs.NumDescs = MaxInstances;

	D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO prebuild{};
	device5->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &prebuild);
	if (prebuild.ResultDataMaxSizeInBytes == 0)
	{
		return;
	}

	HRESULT hr = m_RHI->GetDevice()->CreateCommittedResource(
		&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
		D3D12_HEAP_FLAG_NONE,
		&CD3DX12_RESOURCE_DESC::Buffer(prebuild.ResultDataMaxSizeInBytes,
			D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),
		D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,
		nullptr,
		IID_PPV_ARGS(&m_TLAS));
	assert(SUCCEEDED(hr));
	m_TLAS->SetName(L"LumenTLAS");

	hr = m_RHI->GetDevice()->CreateCommittedResource(
		&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
		D3D12_HEAP_FLAG_NONE,
		&CD3DX12_RESOURCE_DESC::Buffer(prebuild.ScratchDataSizeInBytes,
			D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),
		D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
		nullptr,
		IID_PPV_ARGS(&m_TLASScratch));
	assert(SUCCEEDED(hr));
	m_TLASScratch->SetName(L"LumenTLASScratch");

	m_bAvailable = true;
}


// ============================================================
//  毎フレームの TLAS 再構築
// ============================================================
D3D12_RAYTRACING_INSTANCE_DESC* FLumenHardwareRayTracing::BeginInstances()
{
	if (!m_bAvailable)
	{
		return nullptr;
	}

	m_Frame ^= 1;
	m_NumInstances = 0;
	return m_InstancePointer[m_Frame];
}


void FLumenHardwareRayTracing::BuildTLAS(unsigned int NumInstances)
{
	m_NumInstances = 0;

	if (!m_bAvailable || NumInstances == 0)
	{
		return;
	}

	m_NumInstances = min(NumInstances, m_MaxInstances);

	ID3D12GraphicsCommandList4* cl4 = m_RHI->GetGraphicsCommandList4();

	D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build{};
	build.Inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
	build.Inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;
	build.Inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
	build.Inputs.NumDescs = m_NumInstances;
	build.Inputs.InstanceDescs = m_InstanceBuffer[m_Frame]->GetGPUVirtualAddress();
	build.ScratchAccelerationStructureData = m_TLASScratch->GetGPUVirtualAddress();
	build.DestAccelerationStructureData = m_TLAS->GetGPUVirtualAddress();

	// 前フレームのトレースとの順序は同一キューの直列実行で保証される。
	// リスト内の後続ディスパッチに対しては UAV バリアで完了を保証する。
	cl4->BuildRaytracingAccelerationStructure(&build, 0, nullptr);
	cl4->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::UAV(m_TLAS.Get()));
}
