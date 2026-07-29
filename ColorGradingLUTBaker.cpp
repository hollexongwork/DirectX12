#include "Main.h"
#include "RenderManager.h"
#include "PostProcessSettings.h"
#include "ColorGradingLUTBaker.h"
#include "D3DX12.h"
#include <cstring>

// ============================================================
//  ColorGradingLUTBaker : Grading LUT bake (compute).
// ============================================================

ColorGradingLUTBaker::ColorGradingLUTBaker(RenderManager* owner): m_Owner(owner)
{
}

ColorGradingLUTBaker::~ColorGradingLUTBaker()
{
	if (m_ParamBuffer && m_ParamPtr)
		m_ParamBuffer->Unmap(0, nullptr);
}

ID3D12Device* ColorGradingLUTBaker::Device()
{
	return m_Owner->GetDevice();
}

ID3D12GraphicsCommandList* ColorGradingLUTBaker::CommandList()
{
	return m_Owner->GetGraphicsCommandList();
}

ComPtr<ID3D12PipelineState> ColorGradingLUTBaker::CreateComputePipeline(const char* csoFile)
{
	std::vector<char> cs;
	{
		std::ifstream file(csoFile, std::ios_base::in | std::ios_base::binary);
		assert(file);
		file.seekg(0, std::ios_base::end);
		int filesize = (int)file.tellg();
		file.seekg(0, std::ios_base::beg);
		cs.resize(filesize);
		file.read(&cs[0], filesize);
		file.close();
	}

	D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
	desc.pRootSignature = m_RootSignature.Get();
	desc.CS.pShaderBytecode = cs.data();
	desc.CS.BytecodeLength = cs.size();

	ComPtr<ID3D12PipelineState> pso;
	HRESULT hr = Device()->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pso));
	assert(SUCCEEDED(hr));
	return pso;
}

void ColorGradingLUTBaker::Init()
{
	const DXGI_FORMAT LUTFmt = DXGI_FORMAT_R16G16B16A16_FLOAT;

	// ---- compute root signature ----
	//  [0] CBV  b0 (GRADING_PARAMS)
	//  [1] UAV table u0 (LUT 3D output)
	//  [2] SRV table t0 (artist 2D strip LUT, optional)
	//  static sampler s0 (linear clamp)
	{
		D3D12_DESCRIPTOR_RANGE rangeUAV{};
		rangeUAV.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
		rangeUAV.NumDescriptors = 1;
		rangeUAV.BaseShaderRegister = 0; // u0
		rangeUAV.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

		D3D12_DESCRIPTOR_RANGE rangeSRV{};
		rangeSRV.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
		rangeSRV.NumDescriptors = 1;
		rangeSRV.BaseShaderRegister = 0; // t0
		rangeSRV.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

		D3D12_ROOT_PARAMETER params[3]{};
		params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
		params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
		params[0].Descriptor.ShaderRegister = 0;
		params[0].Descriptor.RegisterSpace = 0;

		params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
		params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
		params[1].DescriptorTable.NumDescriptorRanges = 1;
		params[1].DescriptorTable.pDescriptorRanges = &rangeUAV;

		params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
		params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
		params[2].DescriptorTable.NumDescriptorRanges = 1;
		params[2].DescriptorTable.pDescriptorRanges = &rangeSRV;

		D3D12_STATIC_SAMPLER_DESC samp{};
		samp.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
		samp.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
		samp.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
		samp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
		samp.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
		samp.BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
		samp.MaxLOD = D3D12_FLOAT32_MAX;
		samp.ShaderRegister = 0;
		samp.RegisterSpace = 0;
		samp.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

		D3D12_ROOT_SIGNATURE_DESC rs{};
		rs.NumParameters = _countof(params);
		rs.pParameters = params;
		rs.NumStaticSamplers = 1;
		rs.pStaticSamplers = &samp;
		rs.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

		ComPtr<ID3DBlob> blob, err;
		HRESULT hr = D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err);
		assert(SUCCEEDED(hr));
		hr = Device()->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
			IID_PPV_ARGS(&m_RootSignature));
		assert(SUCCEEDED(hr));
	}

	m_PSO = CreateComputePipeline("Shader/cso/ColorGradingLUT_CS.cso");

	// ---- 3D LUT resource (Texture3D 33^3) ----
	{
		D3D12_HEAP_PROPERTIES prop{};
		prop.Type = D3D12_HEAP_TYPE_DEFAULT;

		D3D12_RESOURCE_DESC desc{};
		desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
		desc.Width = LUT_SIZE;
		desc.Height = LUT_SIZE;
		desc.DepthOrArraySize = LUT_SIZE;          // depth slices
		desc.MipLevels = 1;
		desc.Format = LUTFmt;
		desc.SampleDesc.Count = 1;
		desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
		desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

		HRESULT hr = Device()->CreateCommittedResource(&prop, D3D12_HEAP_FLAG_NONE,
			&desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
			IID_PPV_ARGS(&m_LUT));
		assert(SUCCEEDED(hr));
		m_LUT->SetName(L"ColorGradingLUT3D");
	}

	// ---- UAV (Texture3D) ----
	{
		m_LUTUAVIndex = m_Owner->AllocateDescriptor();
		D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
		uav.Format = LUTFmt;
		uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
		uav.Texture3D.MipSlice = 0;
		uav.Texture3D.FirstWSlice = 0;
		uav.Texture3D.WSize = LUT_SIZE;
		Device()->CreateUnorderedAccessView(m_LUT.Get(), nullptr, &uav,
			m_Owner->GetCPUDescriptorHandle(m_LUTUAVIndex));
	}

	// ---- SRV (Texture3D) for the tonemap pass ----
	{
		m_LUTSRVIndex = m_Owner->AllocateDescriptor();
		D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
		srv.Format = LUTFmt;
		srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
		srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		srv.Texture3D.MostDetailedMip = 0;
		srv.Texture3D.MipLevels = 1;
		Device()->CreateShaderResourceView(m_LUT.Get(), &srv,
			m_Owner->GetCPUDescriptorHandle(m_LUTSRVIndex));
	}

	// ---- param upload buffer (b0) ----
	{
		D3D12_HEAP_PROPERTIES prop{};
		prop.Type = D3D12_HEAP_TYPE_UPLOAD;

		D3D12_RESOURCE_DESC d{};
		d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		d.Width = (sizeof(GRADING_PARAMS) + 255) & ~255u; // 256-aligned CBV
		d.Height = 1;
		d.DepthOrArraySize = 1;
		d.MipLevels = 1;
		d.Format = DXGI_FORMAT_UNKNOWN;
		d.SampleDesc.Count = 1;
		d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

		HRESULT hr = Device()->CreateCommittedResource(&prop, D3D12_HEAP_FLAG_NONE,
			&d, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
			IID_PPV_ARGS(&m_ParamBuffer));
		assert(SUCCEEDED(hr));
		m_ParamBuffer->Map(0, nullptr, &m_ParamPtr);
	}

	// 1x1 fallback so the artist-LUT SRV slot (t0) is always bound.
	CreateFallbackSRV();

	//LoadArtistLUT("Asset/Texture/LUTs/LUT_Adventure.DDS");

	// Bake once at startup with default settings so the LUT is valid
	// even before the first ImGui edit.
	m_Dirty = true;
	UpdateIfDirty(PP_SETTINGS{});
}

bool ColorGradingLUTBaker::ParamsChanged(const GRADING_PARAMS& p) const
{
	return std::memcmp(&p, &m_LastParams, sizeof(GRADING_PARAMS)) != 0;
}

// ---- 1x1 white fallback for t0 (always-bound SRV) ----
void ColorGradingLUTBaker::CreateFallbackSRV()
{
	D3D12_HEAP_PROPERTIES prop{};
	prop.Type = D3D12_HEAP_TYPE_DEFAULT;

	D3D12_RESOURCE_DESC d{};
	d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	d.Width = 1; d.Height = 1; d.DepthOrArraySize = 1; d.MipLevels = 1;
	d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	d.SampleDesc.Count = 1;
	d.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

	HRESULT hr = Device()->CreateCommittedResource(&prop, D3D12_HEAP_FLAG_NONE,
		&d, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr,
		IID_PPV_ARGS(&m_FallbackTex));
	assert(SUCCEEDED(hr));
	m_FallbackTex->SetName(L"ArtistLUTFallback");

	m_FallbackSRVIndex = m_Owner->AllocateDescriptor();
	D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
	srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	srv.Texture2D.MipLevels = 1;
	Device()->CreateShaderResourceView(m_FallbackTex.Get(), &srv,
		m_Owner->GetCPUDescriptorHandle(m_FallbackSRVIndex));
	// contents are undefined (never sampled when PP_FLAG_ARTIST_LUT is off),
	// so no upload needed.
}

void ColorGradingLUTBaker::LoadArtistLUT(const char* ddsFile)
{
	if (!ddsFile || !ddsFile[0]) { ClearArtistLUT(); return; }

	// Reuse RenderManager's DDS loader (SRV ends up in the shared heap).
	// LUT data is linear, not sRGB.
	m_ArtistLUT = m_Owner->LoadTexture(ddsFile, false);
	assert(m_ArtistLUT && m_ArtistLUT->Resource);

	m_ArtistLUTPath = ddsFile;

	D3D12_RESOURCE_DESC desc = m_ArtistLUT->Resource->GetDesc();
	m_ArtistPixelsX = (unsigned int)desc.Width;
	m_ArtistPixelsY = (unsigned int)desc.Height;

	// Standard unwrapped strip: width = tile*tile, height = tile.
	// So tile edge = height, and width should equal height*height.
	m_ArtistTileSize = m_ArtistPixelsY;
	// sanity: a 256x16 strip -> tile 16, 16*16=256 == width. If the strip is
	// authored differently the tile size still follows height, which matches
	// the Unreal convention used by SampleArtistLUT in the shader.

	m_Dirty = true;   // force re-bake with the new artist LUT
}

void ColorGradingLUTBaker::ClearArtistLUT()
{
	if (!m_ArtistLUT) return;
	m_ArtistLUT.reset();
	m_ArtistLUTPath.clear();
	m_ArtistTileSize = 0;
	m_ArtistPixelsX = m_ArtistPixelsY = 0;
	m_Dirty = true;
}


void ColorGradingLUTBaker::UpdateIfDirty(const PP_SETTINGS& Settings)
{
	// Phase 2: レンダラが解決済み設定 (FFinalPostProcessSettings 相当) を渡す。
	const PP_SETTINGS& s = Settings;

	// Build current params and compare against the last baked set.
	GRADING_PARAMS p{};
	p.ColorSaturation = s.ColorSaturation;
	p.ColorContrast = s.ColorContrast;
	p.ColorGamma = s.ColorGamma;
	p.ColorGain = s.ColorGain;
	p.ColorOffset = s.ColorOffset;
	p.WhiteTemp = s.WhiteTemp;
	p.WhiteTint = s.WhiteTint;
	p.LUTSize = LUT_SIZE;
	// White-balance + artist-LUT bits affect the bake; mask the rest so
	// toggling unrelated effects doesn't force a needless re-bake.
	unsigned int bakeFlags = s.Flags & PP_FLAG_WHITE_BALANCE;
	if (m_ArtistLUT) bakeFlags |= PP_FLAG_ARTIST_LUT;
	p.Flags = bakeFlags;

	// Artist LUT combine params.
	p.ArtistLUTWeight = m_ArtistLUT ? m_ArtistWeight : 0.0f;
	p.ArtistLUTTileSize = (float)(m_ArtistTileSize ? m_ArtistTileSize : 16);
	p.ArtistLUTPixelsX = (float)(m_ArtistPixelsX ? m_ArtistPixelsX : 256);
	p.ArtistLUTPixelsY = (float)(m_ArtistPixelsY ? m_ArtistPixelsY : 16);

	if (!m_Dirty && !ParamsChanged(p))
		return; // nothing to do

	m_LastParams = p;
	m_Dirty = false;

	std::memcpy(m_ParamPtr, &p, sizeof(GRADING_PARAMS));

	ID3D12GraphicsCommandList* cl = CommandList();
	ID3D12DescriptorHeap* heap = m_Owner->GetSRVDescriptorHeap();
	cl->SetDescriptorHeaps(1, &heap);

	cl->SetComputeRootSignature(m_RootSignature.Get());
	cl->SetPipelineState(m_PSO.Get());
	cl->SetComputeRootConstantBufferView(0, m_ParamBuffer->GetGPUVirtualAddress());
	cl->SetComputeRootDescriptorTable(1, m_Owner->GetGPUDescriptorHandle(m_LUTUAVIndex));
	// t0 : artist LUT if present, else the 1x1 fallback (always bound).
	unsigned int artistSRV = m_ArtistLUT ? m_ArtistLUT->SRVIndex : m_FallbackSRVIndex;
	cl->SetComputeRootDescriptorTable(2, m_Owner->GetGPUDescriptorHandle(artistSRV));

	// The LUT is normally left in PIXEL_SHADER_RESOURCE state (for the
	// tonemap sample). Transition it back to UAV for this bake, then back
	// to SRV when done. On the very first bake it starts as UAV already,
	// so guard against a redundant transition.
	if (m_BakedOnce)
	{
		cl->ResourceBarrier(1,
			&CD3DX12_RESOURCE_BARRIER::Transition(m_LUT.Get(),
				D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
				D3D12_RESOURCE_STATE_UNORDERED_ACCESS));
	}

	// 33 / 4 -> 9 groups per axis (covers 36, guarded in shader).
	const UINT groups = (LUT_SIZE + 3) / 4;
	cl->Dispatch(groups, groups, groups);

	// UAV barrier (ordering) then transition to SRV for the tonemap read.
	cl->ResourceBarrier(1,
		&CD3DX12_RESOURCE_BARRIER::UAV(m_LUT.Get()));
	cl->ResourceBarrier(1,
		&CD3DX12_RESOURCE_BARRIER::Transition(m_LUT.Get(),
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
			D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE));

	m_BakedOnce = true;
}
