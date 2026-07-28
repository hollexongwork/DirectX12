#include "Main.h"
#include "RenderManager.h"
#include "AutoExposure.h"
#include "D3DX12.h"
#include <fstream>
#include <vector>
#include <cstring>
#include <cmath>

// ============================================================
//  AutoExposure : Histogram eye adaptation (compute).
// ============================================================

AutoExposure::AutoExposure(RenderManager* owner)
    : m_Owner(owner)
{
}

AutoExposure::~AutoExposure()
{
    if (m_ParamBuffer && m_ParamPtr)
        m_ParamBuffer->Unmap(0, nullptr);
    if (m_Readback && m_ReadbackPtr)
        m_Readback->Unmap(0, nullptr);
}

ID3D12Device* AutoExposure::Device()
{
    return m_Owner->GetDevice();
}

ID3D12GraphicsCommandList* AutoExposure::CommandList()
{
    return m_Owner->GetGraphicsCommandList();
}

ComPtr<ID3D12PipelineState> AutoExposure::CreateComputePipeline(const char* csoFile)
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

void AutoExposure::Init()
{
    // ------------------------------------------------------------
    //  Compute root signature (independent of the graphics RS):
    //   [0] CBV  b0 (EXPOSURE_PARAMS)
    //   [1] SRV table t0 (SceneColor)
    //   [2] UAV table u0 (Histogram)
    //   [3] UAV table u1 (Result)
    // ------------------------------------------------------------
    {
        D3D12_DESCRIPTOR_RANGE rangeSRV{};
        rangeSRV.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        rangeSRV.NumDescriptors = 1;
        rangeSRV.BaseShaderRegister = 0; // t0
        rangeSRV.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        D3D12_DESCRIPTOR_RANGE rangeUAV0{};
        rangeUAV0.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        rangeUAV0.NumDescriptors = 1;
        rangeUAV0.BaseShaderRegister = 0; // u0
        rangeUAV0.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        D3D12_DESCRIPTOR_RANGE rangeUAV1{};
        rangeUAV1.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        rangeUAV1.NumDescriptors = 1;
        rangeUAV1.BaseShaderRegister = 1; // u1
        rangeUAV1.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        D3D12_ROOT_PARAMETER params[4]{};
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
        params[2].DescriptorTable.pDescriptorRanges = &rangeUAV0;

        params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params[3].DescriptorTable.NumDescriptorRanges = 1;
        params[3].DescriptorTable.pDescriptorRanges = &rangeUAV1;

        D3D12_ROOT_SIGNATURE_DESC rs{};
        rs.NumParameters = _countof(params);
        rs.pParameters = params;
        rs.NumStaticSamplers = 0;
        rs.pStaticSamplers = nullptr;
        rs.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

        ComPtr<ID3DBlob> blob, err;
        HRESULT hr = D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err);
        assert(SUCCEEDED(hr));
        hr = Device()->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
            IID_PPV_ARGS(&m_RootSignature));
        assert(SUCCEEDED(hr));
    }

    m_PSOHistogram = CreateComputePipeline("Shader/cso/AutoExposureHistogram_CS.cso");
    m_PSOAverage = CreateComputePipeline("Shader/cso/AutoExposureAverage_CS.cso");

    // ------------------------------------------------------------
    //  Histogram buffer (256 * uint), UAV.
    // ------------------------------------------------------------
    {
        D3D12_HEAP_PROPERTIES prop{};
        prop.Type = D3D12_HEAP_TYPE_DEFAULT;

        D3D12_RESOURCE_DESC d{};
        d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        d.Width = HISTOGRAM_BINS * sizeof(unsigned int);
        d.Height = 1;
        d.DepthOrArraySize = 1;
        d.MipLevels = 1;
        d.Format = DXGI_FORMAT_UNKNOWN;
        d.SampleDesc.Count = 1;
        d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

        HRESULT hr = Device()->CreateCommittedResource(&prop, D3D12_HEAP_FLAG_NONE,
            &d, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
            IID_PPV_ARGS(&m_Histogram));
        assert(SUCCEEDED(hr));
        m_Histogram->SetName(L"AutoExposureHistogram");
    }

    // ------------------------------------------------------------
    //  Result buffer (2 * float : [0]=exposure scale, [1]=avg lum).
    // ------------------------------------------------------------
    {
        D3D12_HEAP_PROPERTIES prop{};
        prop.Type = D3D12_HEAP_TYPE_DEFAULT;

        D3D12_RESOURCE_DESC d{};
        d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        d.Width = 2 * sizeof(float);
        d.Height = 1;
        d.DepthOrArraySize = 1;
        d.MipLevels = 1;
        d.Format = DXGI_FORMAT_UNKNOWN;
        d.SampleDesc.Count = 1;
        d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

        HRESULT hr = Device()->CreateCommittedResource(&prop, D3D12_HEAP_FLAG_NONE,
            &d, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
            IID_PPV_ARGS(&m_Result));
        assert(SUCCEEDED(hr));
        m_Result->SetName(L"AutoExposureResult");
    }

    // ------------------------------------------------------------
    //  UAV : Histogram (Raw byte-address buffer, R32_TYPELESS).
    // ------------------------------------------------------------
    {
        m_HistogramUAVIndex = m_Owner->AllocateDescriptor();
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
        uav.Format = DXGI_FORMAT_R32_TYPELESS;
        uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        uav.Buffer.FirstElement = 0;
        uav.Buffer.NumElements = HISTOGRAM_BINS;          // 256 raw uints
        uav.Buffer.StructureByteStride = 0;
        uav.Buffer.CounterOffsetInBytes = 0;
        uav.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
        Device()->CreateUnorderedAccessView(m_Histogram.Get(), nullptr, &uav,
            m_Owner->GetCPUDescriptorHandle(m_HistogramUAVIndex));
    }

    // ------------------------------------------------------------
    //  UAV : Result (Raw byte-address buffer).
    // ------------------------------------------------------------
    {
        m_ResultUAVIndex = m_Owner->AllocateDescriptor();
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
        uav.Format = DXGI_FORMAT_R32_TYPELESS;
        uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        uav.Buffer.FirstElement = 0;
        uav.Buffer.NumElements = 2;
        uav.Buffer.StructureByteStride = 0;
        uav.Buffer.CounterOffsetInBytes = 0;
        uav.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
        Device()->CreateUnorderedAccessView(m_Result.Get(), nullptr, &uav,
            m_Owner->GetCPUDescriptorHandle(m_ResultUAVIndex));
    }

    // ------------------------------------------------------------
    //  SRV : Result for the tonemap pass (t11). Buffer of 2 floats.
    // ------------------------------------------------------------
    {
        m_ResultSRVIndex = m_Owner->AllocateDescriptor();
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = DXGI_FORMAT_R32_FLOAT;
        srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Buffer.FirstElement = 0;
        srv.Buffer.NumElements = 2;
        srv.Buffer.StructureByteStride = 0;
        srv.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
        Device()->CreateShaderResourceView(m_Result.Get(), &srv,
            m_Owner->GetCPUDescriptorHandle(m_ResultSRVIndex));
        m_ResultSRVHandle = m_Owner->GetGPUDescriptorHandle(m_ResultSRVIndex);
    }

    // ------------------------------------------------------------
    //  Non-shader-visible heap for ClearUnorderedAccessViewUint.
    //  Clear needs BOTH a GPU (shader-visible) and CPU (non-shader-
    //  visible) handle of the same UAV; the shared heap is shader-
    //  visible, so allocate a tiny CPU-only heap for the clear handle.
    // ------------------------------------------------------------
    {
        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.NumDescriptors = 2;                       // slot0 histogram, slot1 result
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;  // CPU-visible only
        HRESULT hr = Device()->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&m_ClearHeap));
        assert(SUCCEEDED(hr));

        UINT inc = Device()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        D3D12_CPU_DESCRIPTOR_HANDLE base = m_ClearHeap->GetCPUDescriptorHandleForHeapStart();

        // slot 0 : histogram raw UAV
        D3D12_UNORDERED_ACCESS_VIEW_DESC hUav{};
        hUav.Format = DXGI_FORMAT_R32_TYPELESS;
        hUav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        hUav.Buffer.FirstElement = 0;
        hUav.Buffer.NumElements = HISTOGRAM_BINS;
        hUav.Buffer.StructureByteStride = 0;
        hUav.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
        Device()->CreateUnorderedAccessView(m_Histogram.Get(), nullptr, &hUav, base);

        // slot 1 : result raw UAV (for the one-time zero init)
        D3D12_CPU_DESCRIPTOR_HANDLE rHandle = base;
        rHandle.ptr += inc;
        D3D12_UNORDERED_ACCESS_VIEW_DESC rUav{};
        rUav.Format = DXGI_FORMAT_R32_TYPELESS;
        rUav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        rUav.Buffer.FirstElement = 0;
        rUav.Buffer.NumElements = 2;
        rUav.Buffer.StructureByteStride = 0;
        rUav.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
        Device()->CreateUnorderedAccessView(m_Result.Get(), nullptr, &rUav, rHandle);
    }

    // ------------------------------------------------------------
    //  b0 upload buffer (EXPOSURE_PARAMS, 256-aligned).
    // ------------------------------------------------------------
    {
        D3D12_HEAP_PROPERTIES prop{};
        prop.Type = D3D12_HEAP_TYPE_UPLOAD;

        D3D12_RESOURCE_DESC d{};
        d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        d.Width = (sizeof(EXPOSURE_PARAMS) + 255) & ~255u;
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

    // ------------------------------------------------------------
    //  Readback buffer (2 floats) : a READBACK-heap copy of m_Result,
    //  filled each frame via CopyBufferRegion and persistently mapped so
    //  the CPU (ImGui) can read the current exposure + average luminance.
    // ------------------------------------------------------------
    {
        D3D12_HEAP_PROPERTIES prop{};
        prop.Type = D3D12_HEAP_TYPE_READBACK;

        D3D12_RESOURCE_DESC d{};
        d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        d.Width = 2 * sizeof(float);
        d.Height = 1;
        d.DepthOrArraySize = 1;
        d.MipLevels = 1;
        d.Format = DXGI_FORMAT_UNKNOWN;
        d.SampleDesc.Count = 1;
        d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        HRESULT hr = Device()->CreateCommittedResource(&prop, D3D12_HEAP_FLAG_NONE,
            &d, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
            IID_PPV_ARGS(&m_Readback));
        assert(SUCCEEDED(hr));
        m_Readback->SetName(L"AutoExposureReadback");
        // Persistent map; CPU read range is the whole 2-float buffer.
        m_Readback->Map(0, nullptr, &m_ReadbackPtr);
    }
}

void AutoExposure::Dispatch(unsigned int sceneColorSRVIndex,
    unsigned int width, unsigned int height,
    float deltaTime)
{
    ID3D12GraphicsCommandList* cl = CommandList();
    ID3D12DescriptorHeap* heap = m_Owner->GetSRVDescriptorHeap();
    cl->SetDescriptorHeaps(1, &heap);

    // ---- Upload params for this frame ----
    EXPOSURE_PARAMS p{};
    p.SceneWidth = width;
    p.SceneHeight = height;
    p.MinLogLuminance = m_Params.MinLogLuminance;
    p.MaxLogLuminance = m_Params.MaxLogLuminance;
    p.LowPercent = m_Params.LowPercent;
    p.HighPercent = m_Params.HighPercent;
    p.MinBrightness = m_Params.MinBrightness;
    p.MaxBrightness = m_Params.MaxBrightness;
    p.SpeedUp = m_Params.SpeedUp;
    p.SpeedDown = m_Params.SpeedDown;
    p.ExposureCompensation = m_Params.ExposureCompensation;
    // Guard against huge first-frame / paused dt (avoids an exposure pop).
    p.DeltaTime = (deltaTime > 0.0f && deltaTime < 0.5f) ? deltaTime : 1.0f / 60.0f;
    std::memcpy(m_ParamPtr, &p, sizeof(p));

    cl->SetComputeRootSignature(m_RootSignature.Get());
    cl->SetComputeRootConstantBufferView(0, m_ParamBuffer->GetGPUVirtualAddress());

    // One-time zero init of the persistent result buffer. It is created in
    // UAV state, so on the first frame we can clear it directly (the average
    // pass treats a zero/previous<=0 value as "first valid frame" and seeds
    // from the target exposure). Slot 1 of m_ClearHeap is the result UAV.
    if (!m_ResultInitialised)
    {
        UINT inc = Device()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        D3D12_CPU_DESCRIPTOR_HANDLE rClearCPU = m_ClearHeap->GetCPUDescriptorHandleForHeapStart();
        rClearCPU.ptr += inc; // slot 1

        const float zeroF[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        cl->ClearUnorderedAccessViewFloat(
            m_Owner->GetGPUDescriptorHandle(m_ResultUAVIndex), // shader-visible
            rClearCPU,                                         // non-shader-visible
            m_Result.Get(), zeroF, 0, nullptr);
        cl->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::UAV(m_Result.Get()));
    }

    // ---- Clear the histogram to 0 (per frame) ----
    {
        const UINT zero[4] = { 0, 0, 0, 0 };
        cl->ClearUnorderedAccessViewUint(
            m_Owner->GetGPUDescriptorHandle(m_HistogramUAVIndex), // shader-visible
            m_ClearHeap->GetCPUDescriptorHandleForHeapStart(),    // non-shader-visible
            m_Histogram.Get(), zero, 0, nullptr);

        cl->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::UAV(m_Histogram.Get()));
    }

    // ---- Pass 1 : build histogram ----
    {
        cl->SetPipelineState(m_PSOHistogram.Get());
        cl->SetComputeRootDescriptorTable(1, m_Owner->GetGPUDescriptorHandle(sceneColorSRVIndex)); // t0
        cl->SetComputeRootDescriptorTable(2, m_Owner->GetGPUDescriptorHandle(m_HistogramUAVIndex)); // u0

        const UINT TG = 16;
        UINT gx = (width + TG - 1) / TG;
        UINT gy = (height + TG - 1) / TG;
        cl->Dispatch(gx, gy, 1);

        cl->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::UAV(m_Histogram.Get()));
    }

    // ---- Pass 2 : average + temporal adaptation ----
    {
        // The result buffer was left in PIXEL_SHADER_RESOURCE after the
        // previous frame's tonemap read; flip it back to UAV so the
        // average pass can read the previous exposure and write the new
        // one. On the very first frame it is already UAV (creation state).
        if (m_ResultInitialised)
        {
            cl->ResourceBarrier(1,
                &CD3DX12_RESOURCE_BARRIER::Transition(m_Result.Get(),
                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS));
        }

        cl->SetPipelineState(m_PSOAverage.Get());
        cl->SetComputeRootDescriptorTable(2, m_Owner->GetGPUDescriptorHandle(m_HistogramUAVIndex)); // u0
        cl->SetComputeRootDescriptorTable(3, m_Owner->GetGPUDescriptorHandle(m_ResultUAVIndex));    // u1

        cl->Dispatch(1, 1, 1);

        cl->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::UAV(m_Result.Get()));
    }

    // ---- Copy result -> READBACK buffer (for ImGui display) ----
    // Transition UAV -> COPY_SOURCE, copy the 2 floats, then continue to
    // PIXEL_SHADER_RESOURCE for the tonemap read. The CPU reads m_Readback
    // a frame or two later (asynchronous, fine for a UI readout).
    cl->ResourceBarrier(1,
        &CD3DX12_RESOURCE_BARRIER::Transition(m_Result.Get(),
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_COPY_SOURCE));

    cl->CopyBufferRegion(m_Readback.Get(), 0, m_Result.Get(), 0, 2 * sizeof(float));

    // ---- Result : COPY_SOURCE -> SRV for the tonemap read (t11) ----
    cl->ResourceBarrier(1,
        &CD3DX12_RESOURCE_BARRIER::Transition(m_Result.Get(),
            D3D12_RESOURCE_STATE_COPY_SOURCE,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE));

    m_ResultInitialised = true;
}

void AutoExposure::UpdateReadback()
{
    if (!m_ReadbackPtr)
        return;
    const float* p = reinterpret_cast<const float*>(m_ReadbackPtr);
    m_ReadbackExposure = p[0]; // adapted exposure scale
    m_ReadbackAvgLum = p[1]; // measured average luminance
}

float AutoExposure::GetCurrentExposureEV() const
{
    return (m_ReadbackExposure > 0.0f) ? log2f(m_ReadbackExposure) : 0.0f;
}