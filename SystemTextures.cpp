#include "Main.h"
#include "RenderManager.h"
#include "SystemTextures.h"
#include "LTC.h"
#include "D3DX12.h"
#include <DirectXPackedVector.h>
#include <vector>

using DirectX::PackedVector::HALF;
using DirectX::PackedVector::XMConvertFloatToHalf;


FSystemTextures::FSystemTextures(RenderManager* RHI)
	: m_RHI(RHI)
{
}


FSystemTextures::~FSystemTextures()
{
	// in-flight のコマンドリストが読んでいる可能性があるので遅延解放
	if (m_LTCMat)
	{
		m_RHI->DeferredRelease(m_LTCMat, (int)m_LTCMatSRVIndex, -1);
	}
	if (m_LTCAmp)
	{
		m_RHI->DeferredRelease(m_LTCAmp, (int)m_LTCAmpSRVIndex, -1);
	}
}


// ============================================================
//  CreateTexture2D: CPU データからミップ無しの 2D テクスチャを作る
//  (DDSTextureLoader と同じ CUSTOM / WRITE_BACK ヒープ + WriteToSubresource)
// ============================================================
ComPtr<ID3D12Resource> FSystemTextures::CreateTexture2D(unsigned int Size, DXGI_FORMAT Format, unsigned int BytesPerTexel,
	const void* Data, const wchar_t* Name)
{
	ID3D12Device* device = m_RHI->GetDevice();

	D3D12_HEAP_PROPERTIES heapProperties{};
	heapProperties.Type = D3D12_HEAP_TYPE_CUSTOM;
	heapProperties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_WRITE_BACK;
	heapProperties.MemoryPoolPreference = D3D12_MEMORY_POOL_L0;

	D3D12_RESOURCE_DESC desc{};
	desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	desc.Width = Size;
	desc.Height = Size;
	desc.DepthOrArraySize = 1;
	desc.MipLevels = 1;
	desc.Format = Format;
	desc.SampleDesc.Count = 1;
	desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
	desc.Flags = D3D12_RESOURCE_FLAG_NONE;

	ComPtr<ID3D12Resource> texture;
	HRESULT hr = device->CreateCommittedResource(
		&heapProperties,
		D3D12_HEAP_FLAG_NONE,
		&desc,
		D3D12_RESOURCE_STATE_COPY_DEST,
		nullptr,
		IID_PPV_ARGS(&texture));
	assert(SUCCEEDED(hr));
	texture->SetName(Name);

	D3D12_BOX box = { 0, 0, 0, Size, Size, 1 };
	hr = texture->WriteToSubresource(0, &box, Data, Size * BytesPerTexel, Size * Size * BytesPerTexel);
	assert(SUCCEEDED(hr));

	m_RHI->GetGraphicsCommandList()->ResourceBarrier(1,
		&CD3DX12_RESOURCE_BARRIER::Transition(
			texture.Get(),
			D3D12_RESOURCE_STATE_COPY_DEST,
			D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE));

	return texture;
}


// ミップ無し 2D テクスチャの SRV を共有ヒープに作る
unsigned int FSystemTextures::CreateSRV(ID3D12Resource* Resource, DXGI_FORMAT Format)
{
	const unsigned int index = m_RHI->AllocateDescriptor();

	D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
	srv.Format = Format;
	srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	srv.Texture2D.MostDetailedMip = 0;
	srv.Texture2D.MipLevels = 1;

	m_RHI->GetDevice()->CreateShaderResourceView(Resource, &srv, m_RHI->GetCPUDescriptorHandle(index));
	return index;
}


// ============================================================
//  Init: LTC テーブル (LTC.cpp) を半精度テクスチャへ
// ============================================================
void FSystemTextures::Init()
{
	const unsigned int size = (unsigned int)LTC_Size;
	const unsigned int numTexels = size * size;

	// t37: 逆行列 4 成分 (RGBA16F)
	{
		std::vector<HALF> data(numTexels * 4);
		for (unsigned int i = 0; i < numTexels; ++i)
		{
			for (unsigned int c = 0; c < 4; ++c)
			{
				data[i * 4 + c] = XMConvertFloatToHalf(LTCMat[i][c]);
			}
		}
		m_LTCMat = CreateTexture2D(size, DXGI_FORMAT_R16G16B16A16_FLOAT, 8, data.data(), L"LTCMat");
		m_LTCMatSRVIndex = CreateSRV(m_LTCMat.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT);
	}

	// t38: 振幅 + フレネル (RG16F)
	{
		std::vector<HALF> data(numTexels * 2);
		for (unsigned int i = 0; i < numTexels; ++i)
		{
			data[i * 2 + 0] = XMConvertFloatToHalf(LTCAmp[i][0]);
			data[i * 2 + 1] = XMConvertFloatToHalf(LTCAmp[i][1]);
		}
		m_LTCAmp = CreateTexture2D(size, DXGI_FORMAT_R16G16_FLOAT, 4, data.data(), L"LTCAmp");
		m_LTCAmpSRVIndex = CreateSRV(m_LTCAmp.Get(), DXGI_FORMAT_R16G16_FLOAT);
	}
}


void FSystemTextures::BindTextures()
{
	m_RHI->BindRootTableBySRVIndex((unsigned int)RenderManager::TEXTURE_TYPE::LTC_MAT, m_LTCMatSRVIndex);
	m_RHI->BindRootTableBySRVIndex((unsigned int)RenderManager::TEXTURE_TYPE::LTC_AMP, m_LTCAmpSRVIndex);
}
