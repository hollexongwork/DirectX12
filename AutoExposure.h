#pragma once

// ============================================================
//  AutoExposure
//
//  Histogram-based automatic exposure, run every frame as two
//  compute dispatches:
//
//    Pass 1 (AutoExposureHistogram_CS): build a 256-bin log-
//            luminance histogram of the HDR SceneColor.
//    Pass 2 (AutoExposureAverage_CS):  percentile-clip + weighted
//            average the histogram, then exponentially adapt the
//            exposure scale toward the target (Speed Up/Down).
//
//  The adapted exposure scale lives in a small GPU buffer that
//  persists across frames (the eye-adaptation state). The tonemap
//  pass samples it (t11) and multiplies it into SceneColor, so the
//  whole loop stays GPU-side with no CPU readback stall -- exactly
//  how EyeAdaptation works.
//
//  Mirrors IBLBaker / ColorGradingLUTBaker's dependency model:
//  talks to RenderManager through its public accessors (device,
//  command list, descriptor allocation). Unlike those one-shot
//  bakers this dispatches per frame, so it does NOT flush the
//  command list -- its work is recorded inside DrawEnd's list.
// ============================================================

class RenderManager;

class AutoExposure
{
public:
    // ---- Tunable parameters (ImGui / SettingsManager) ----
    // SettingsManager が Default スナップショットを保持するため public 型にしている。
    struct Params
    {
        float MinLogLuminance      = -10.0f;  // histogram low  end (log2)
        float MaxLogLuminance      = 1.0f;    // histogram high end (log2)
        float LowPercent           = 0.25f;   // clip darkest
        float HighPercent          = 0.75f;   // clip above
        float MinBrightness        = 0.03f;   // exposure clamp (lo)
        float MaxBrightness        = 0.25f;   // exposure clamp (hi)
        float SpeedUp              = 10.0f;   // adapt speed (scene brighter)
        float SpeedDown            = 10.0f;   // adapt speed (scene darker)
        float ExposureCompensation = 0.0f;    // EV bias on the auto result
    };

private:
    // Matches EXPOSURE_PARAMS cbuffer in both compute shaders (b0).
    struct EXPOSURE_PARAMS
    {
        unsigned int SceneWidth;
        unsigned int SceneHeight;
        float        MinLogLuminance;
        float        MaxLogLuminance;

        float        LowPercent;
        float        HighPercent;
        float        MinBrightness;
        float        MaxBrightness;

        float        SpeedUp;
        float        SpeedDown;
        float        ExposureCompensation;
        float        DeltaTime;
    };

    RenderManager* m_Owner = nullptr;
    Params         m_Params;

    // Independent compute root signature shared by both passes:
    //  [0] CBV  b0 (EXPOSURE_PARAMS)
    //  [1] SRV table t0 (SceneColor)           -- histogram pass
    //  [2] UAV table u0 (Histogram)            -- both passes
    //  [3] UAV table u1 (Result)               -- average pass
    ComPtr<ID3D12RootSignature> m_RootSignature;
    ComPtr<ID3D12PipelineState> m_PSOHistogram;
    ComPtr<ID3D12PipelineState> m_PSOAverage;

    // 256-bin uint histogram (cleared each frame before pass 1).
    ComPtr<ID3D12Resource>      m_Histogram;
    unsigned int                m_HistogramUAVIndex = 0;      // shader-visible UAV
    unsigned int                m_HistogramClearUAVIndex = 0; // CPU-visible UAV (ClearUAV)

    // Persistent 2-float result: [0]=exposure scale, [1]=avg luminance.
    ComPtr<ID3D12Resource>      m_Result;
    unsigned int                m_ResultUAVIndex = 0;
    unsigned int                m_ResultSRVIndex = 0;
    D3D12_GPU_DESCRIPTOR_HANDLE m_ResultSRVHandle{};

    // Non-shader-visible heap for ClearUnorderedAccessViewUint (it needs a
    // CPU handle from a non-shader-visible heap plus the shader-visible one).
    ComPtr<ID3D12DescriptorHeap> m_ClearHeap;

    // b0 upload buffer.
    ComPtr<ID3D12Resource>      m_ParamBuffer;
    void* m_ParamPtr = nullptr;

    bool                        m_ResultInitialised = false;

    // ---- CPU readback of the GPU result (for ImGui display) ----
    // READBACK-heap copy of m_Result, written each frame via CopyBufferRegion
    // and persistently mapped. Reading it gives the value the GPU produced a
    // frame or two earlier (asynchronous), which is fine for a UI readout.
    ComPtr<ID3D12Resource>      m_Readback;
    void* m_ReadbackPtr = nullptr;
    float                       m_ReadbackExposure = 0.0f;
    float                       m_ReadbackAvgLum = 0.0f;

    ID3D12Device* Device();
    ID3D12GraphicsCommandList* CommandList();
    ComPtr<ID3D12PipelineState> CreateComputePipeline(const char* csoFile);

public:
    explicit AutoExposure(RenderManager* owner);
    ~AutoExposure();

    void Init();                       // root sig / PSOs / buffers / SRV

    // Record the histogram + adaptation dispatches for this frame.
    //  sceneColorSRVIndex : SRV of the HDR SceneColor (already in a
    //                       shader-readable state by the caller).
    //  width/height       : SceneColor dimensions.
    //  deltaTime          : seconds since last frame (adaptation rate).
    void Dispatch(unsigned int sceneColorSRVIndex,
                  unsigned int width, unsigned int height,
                  float deltaTime);

    // SRV index/handle of the 1-element exposure result buffer, bound
    // to the tonemap pass on t11 (TEXTURE_TYPE::AUTO_EXPOSURE).
    unsigned int                GetExposureSRVIndex()  const { return m_ResultSRVIndex; }
    D3D12_GPU_DESCRIPTOR_HANDLE GetExposureSRVHandle() const { return m_ResultSRVHandle; }

    // Latest GPU-computed values read back to the CPU (for ImGui display).
    // These lag the GPU by a frame or two (readback is asynchronous) but are
    // fine for an on-screen readout. Valid only after the first frames once
    // the readback buffer has been populated; both are 0 until then.
    float GetCurrentExposure()      const { return m_ReadbackExposure; }
    float GetCurrentAvgLuminance()  const { return m_ReadbackAvgLum; }
    // EV stops the auto exposure currently corresponds to: log2(exposure).
    float GetCurrentExposureEV()    const;

    // Pull the latest readback values into the CPU-side cache. Call once per
    // frame (e.g. from ImGui) before reading the getters above.
    void  UpdateReadback();

    // ---- Tunable parameters (driven by ImGui / PostProcessVolume) ----

    Params&       GetParams()       { return m_Params; }
    const Params& GetParams() const { return m_Params; }

    static const unsigned int HISTOGRAM_BINS = 256;
};