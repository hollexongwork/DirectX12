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
//  Talks to RenderManager through its public accessors (device,
//  command list, descriptor allocation), like IBLBaker /
//  ColorGradingLUTBaker. Unlike IBLBaker (one-shot, flushes) this
//  dispatches every frame and does NOT flush -- its work is
//  recorded into the frame's command list from
//  FSceneRenderer::RenderPostProcessing.
// ============================================================

class RenderManager;

class AutoExposure
{
public:
    // ---- Tunable parameters (ImGui / SettingsManager) ----
    // SettingsManager が Default スナップショットを保持するため public 型にしている。
    struct Params
    {
        float MinLogLuminance = -10.0f;  // histogram low  end (log2)
        float MaxLogLuminance = 1.0f;    // histogram high end (log2)
        float LowPercent = 0.25f;   // clip darkest
        float HighPercent = 0.75f;   // clip above
        float MinBrightness = 0.03f;   // exposure clamp (lo)
        float MaxBrightness = 0.25f;   // exposure clamp (hi)
        float SpeedUp = 10.0f;   // adapt speed (scene brighter)
        float SpeedDown = 10.0f;   // adapt speed (scene darker)
        float ExposureCompensation = 0.0f;    // EV bias on the auto result
    };

private:
    // Matches the EXPOSURE_PARAMS cbuffer (b0) in Shader/AutoExposureCommon.hlsl.
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

    // Persistent 2-float result: [0]=exposure scale, [1]=avg luminance.
    ComPtr<ID3D12Resource>      m_Result;
    unsigned int                m_ResultUAVIndex = 0;
    unsigned int                m_ResultSRVIndex = 0;

    // Non-shader-visible heap for ClearUnorderedAccessViewUint (it needs a
    // CPU handle from a non-shader-visible heap plus the shader-visible one).
    ComPtr<ID3D12DescriptorHeap> m_ClearHeap;

    // b0 upload buffer.
    // 2フレーム・イン・フライトのため、CPU 書き込みが in-flight フレームの
    // GPU 読みと競合しないようフレーム毎にダブルバッファする
    // (m_LightBuffer 等と同じパターン)。
    ComPtr<ID3D12Resource>      m_ParamBuffer[2];
    void* m_ParamPtr[2] = {};

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
    //  sceneColorResource : HDR SceneColor resource. Must be in
    //                       PIXEL_SHADER_RESOURCE on entry; Dispatch
    //                       transitions it to NON_PIXEL_SHADER_RESOURCE
    //                       for the histogram compute pass and restores
    //                       it before returning.
    //  sceneColorSRVIndex : SRV of the HDR SceneColor.
    //  width/height       : SceneColor dimensions.
    //  deltaTime          : seconds since last frame (adaptation rate).
    void Dispatch(ID3D12Resource* sceneColorResource,
        unsigned int sceneColorSRVIndex,
        unsigned int width, unsigned int height,
        float deltaTime);

    // SRV index of the 2-element ([0]=exposure scale, [1]=avg luminance)
    // exposure result buffer, bound to the tonemap pass on t11
    // (TEXTURE_TYPE::AUTO_EXPOSURE).
    unsigned int                GetExposureSRVIndex()  const { return m_ResultSRVIndex; }

    // ---- 結果バッファの状態管理 ----
    // m_Result はバッファなので、レガシーバリアでは ExecuteCommandLists 完了ごとに
    // COMMON へ減衰する (フレームを跨いだ「PSR 常駐」は成立しない)。
    // RenderPostProcessing の先頭 (結果を読む全パスより前) で呼び、
    // COMMON -> 読み取り (PIXEL | NON_PIXEL) を明示遷移する。
    // 初回 Dispatch 前 (IsResultValid() = false) は何もしない。
    // フレーム内の遷移: COMMON -> RD -> UAV -> COPY_SOURCE -> RD (-> 減衰)
    void PrepareResultForRead();

    // 結果バッファに有効な露出値があるか (初回 Dispatch 以降 true)。
    // false の間は前フレーム露出として読んではならない (TAA は手動露出へフォールバック)
    bool IsResultValid() const { return m_ResultInitialised; }

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

    // ---- Tunable parameters (driven by ImGui / SettingsManager) ----

    Params& GetParams() { return m_Params; }
    const Params& GetParams() const { return m_Params; }

    static const unsigned int HISTOGRAM_BINS = 256;
};