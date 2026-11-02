#include "Main.h"
#include "SkeletalRenderGPUSkin.h"

FSkeletalMeshObjectGPUSkin::FSkeletalMeshObjectGPUSkin(unsigned int NumBones)
	: m_NumBones((std::max)(1u, NumBones))
{
	RenderManager* rm = RenderManager::GetInstance();
	if (rm == nullptr) return;
	ID3D12Device* device = rm->GetDevice();

	const UINT64 bufferSize = sizeof(FBoneMatrix) * m_NumBones;

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

	// 単位行列 (転置しても同じ) で初期化しておく = Update 前はバインドポーズのまま描ける
	FBoneMatrix identity{};
	identity.Row[0] = { 1.0f, 0.0f, 0.0f, 0.0f };
	identity.Row[1] = { 0.0f, 1.0f, 0.0f, 0.0f };
	identity.Row[2] = { 0.0f, 0.0f, 1.0f, 0.0f };

	for (unsigned int i = 0; i < NumBuffers; ++i)
	{
		FBoneBuffer& buffer = m_Buffers[i];

		HRESULT hr = device->CreateCommittedResource(
			&heapProperties,
			D3D12_HEAP_FLAG_NONE,
			&resourceDesc,
			D3D12_RESOURCE_STATE_GENERIC_READ,
			nullptr,
			IID_PPV_ARGS(&buffer.Resource));
		assert(SUCCEEDED(hr));
		if (FAILED(hr)) return;
		buffer.Resource->SetName(L"GPUSkinBoneBuffer");

		hr = buffer.Resource->Map(0, nullptr, reinterpret_cast<void**>(&buffer.Mapped));
		assert(SUCCEEDED(hr));
		if (FAILED(hr)) return;
		for (unsigned int b = 0; b < m_NumBones; ++b)
		{
			buffer.Mapped[b] = identity;
		}

		buffer.SRVIndex = rm->AllocateDescriptor();

		D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
		srvDesc.Format = DXGI_FORMAT_UNKNOWN;
		srvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
		srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		srvDesc.Buffer.FirstElement = 0;
		srvDesc.Buffer.NumElements = m_NumBones;
		srvDesc.Buffer.StructureByteStride = sizeof(FBoneMatrix);
		srvDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
		device->CreateShaderResourceView(buffer.Resource.Get(), &srvDesc, rm->GetCPUDescriptorHandle(buffer.SRVIndex));
	}

	m_bValid = true;
}

FSkeletalMeshObjectGPUSkin::~FSkeletalMeshObjectGPUSkin()
{
	// GPU が in-flight のフレームで読んでいる可能性があるので遅延解放する
	RenderManager* rm = RenderManager::GetInstance();
	for (unsigned int i = 0; i < NumBuffers; ++i)
	{
		FBoneBuffer& buffer = m_Buffers[i];
		if (buffer.Resource)
		{
			if (buffer.Mapped)
			{
				buffer.Resource->Unmap(0, nullptr);
				buffer.Mapped = nullptr;
			}
			if (rm)
			{
				rm->DeferredRelease(std::move(buffer.Resource), m_bValid ? static_cast<int>(buffer.SRVIndex) : -1);
			}
			buffer.Resource.Reset();
		}
	}
}

void FSkeletalMeshObjectGPUSkin::Update(const std::vector<XMFLOAT4X4>& ReferenceToLocal, unsigned long long FrameNumber)
{
	if (!m_bValid) return;

	// 同じフレーム内の 2 回目以降の Update は同じ本へ上書きする (リングを進めない)
	if (m_Buffers[m_Current].UpdatedFrame != FrameNumber)
	{
		m_Current = (m_Current + 1) % NumBuffers;
	}
	FBoneBuffer& buffer = m_Buffers[m_Current];
	buffer.UpdatedFrame = FrameNumber;

	const unsigned int count = (std::min)(m_NumBones, static_cast<unsigned int>(ReferenceToLocal.size()));
	for (unsigned int i = 0; i < count; ++i)
	{
		// 行ベクトル規約の 4x4 を転置し、上 3 行を格納する (mul(float3x4, float4(p,1)) 用)
		XMFLOAT4X4 t;
		XMStoreFloat4x4(&t, XMMatrixTranspose(XMLoadFloat4x4(&ReferenceToLocal[i])));
		buffer.Mapped[i].Row[0] = { t._11, t._12, t._13, t._14 };
		buffer.Mapped[i].Row[1] = { t._21, t._22, t._23, t._24 };
		buffer.Mapped[i].Row[2] = { t._31, t._32, t._33, t._34 };
	}
	for (unsigned int i = count; i < m_NumBones; ++i)
	{
		buffer.Mapped[i].Row[0] = { 1.0f, 0.0f, 0.0f, 0.0f };
		buffer.Mapped[i].Row[1] = { 0.0f, 1.0f, 0.0f, 0.0f };
		buffer.Mapped[i].Row[2] = { 0.0f, 0.0f, 1.0f, 0.0f };
	}
}

unsigned int FSkeletalMeshObjectGPUSkin::GetPreviousBoneBufferSRVIndex(unsigned long long FrameNumber) const
{
	// 直前のフレームに更新された本を探す (通常は 1 つ前の本)。
	// 無ければ今の本 (= ボーン由来の速度 0。コンポーネントの移動分は b1 が担う)
	for (unsigned int i = 0; i < NumBuffers; ++i)
	{
		if (i != m_Current && m_Buffers[i].UpdatedFrame != NeverUpdated &&
			m_Buffers[i].UpdatedFrame + 1 == FrameNumber && m_Buffers[m_Current].UpdatedFrame == FrameNumber)
		{
			return m_Buffers[i].SRVIndex;
		}
	}
	return m_Buffers[m_Current].SRVIndex;
}
