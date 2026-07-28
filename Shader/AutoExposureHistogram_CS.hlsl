// ============================================================
//  AutoExposureHistogram_CS.hlsl
//
//  Eye adaptation - Pass 1 of 2 (histogram build).
//
//  Reads the HDR SceneColor, computes per-pixel log2 luminance,
//  maps it into one of HISTOGRAM_BINS buckets between a min/max
//  log-luminance range, and accumulates a 256-bin luminance
//  histogram into an RWByteAddressBuffer using groupshared
//  atomics (one shared bin array per thread group, flushed to
//  the global histogram once per group).
//
//  FRDGBuilder histogram + eye-adaptation passes. Pass 2
//  (AutoExposureAverage_CS) consumes this histogram and produces
//  the temporally-adapted exposure scale.
//
//  Root signature (compute, independent of the graphics RS):
//    b0 : EXPOSURE_PARAMS   (cbuffer)
//    t0 : SceneColor        (Texture2D, R16G16B16A16_FLOAT)
//    u0 : Histogram         (RWByteAddressBuffer, 256 * uint)
//
//  Note: this file owns its own b0 register (NOT ViewConstantBuffer).
//  It does not #include any register-declaring shared header; only
//  pure helper math is inlined here so no register leakage occurs.
// ============================================================

#define HISTOGRAM_BINS      256
#define THREADS_X           16
#define THREADS_Y           16
#define THREADS_PER_GROUP   (THREADS_X * THREADS_Y)   // 256 == HISTOGRAM_BINS

cbuffer EXPOSURE_PARAMS : register(b0)
{
    uint SceneWidth; // SceneColor width  in texels
    uint SceneHeight; // SceneColor height in texels
    float MinLogLuminance; // histogram low  end (log2 cd/m2-ish)
    float MaxLogLuminance; // histogram high end

    float LowPercent; // 0..1  dark   pixels to clip (e.g. 0.5  -> 50%)
    float HighPercent; // 0..1  bright pixels to clip (e.g. 0.95 -> 95%)
    float MinBrightness; // exposure clamp (linear avg luminance)
    float MaxBrightness;

    float SpeedUp; // adaptation speed when scene gets brighter
    float SpeedDown; // adaptation speed when scene gets darker
    float ExposureCompensation; // EV bias added on top of the auto result
    float DeltaTime; // seconds since last frame (adaptation)
};

Texture2D<float4> SceneColor : register(t0);
RWByteAddressBuffer Histogram : register(u0);

// Per-group shared histogram, flushed once at the end of the group.
groupshared uint gs_Histogram[HISTOGRAM_BINS];

// Rec.709 luminance.
float Luminance(float3 c)
{
    return dot(c, float3(0.2126f, 0.7152f, 0.0722f));
}

// Map a linear luminance to a [0,1] histogram coordinate using the
// log range, then to an integer bin. Bin 0 is reserved for ~black so
// fully dark pixels don't skew the average.
uint LuminanceToBin(float lum)
{
    if (lum < 1e-4f)
        return 0u;

    float logLum = log2(lum);
    // Normalise into [0,1] across the configured log range.
    float t = saturate((logLum - MinLogLuminance) / (MaxLogLuminance - MinLogLuminance));
    // Bins 1..255 carry the real distribution; 0 is the black bucket.
    return (uint) (t * (float) (HISTOGRAM_BINS - 2) + 1.0f);
}

[numthreads(THREADS_X, THREADS_Y, 1)]
void main(uint3 DTid : SV_DispatchThreadID,
          uint GIdx : SV_GroupIndex)
{
    // Clear this group's shared histogram (1 thread per bin).
    gs_Histogram[GIdx] = 0u;
    GroupMemoryBarrierWithGroupSync();

    // Accumulate this pixel into the shared histogram.
    if (DTid.x < SceneWidth && DTid.y < SceneHeight)
    {
        float3 hdr = SceneColor.Load(int3(DTid.xy, 0)).rgb;
        float lum = Luminance(hdr);
        uint bin = LuminanceToBin(lum);
        InterlockedAdd(gs_Histogram[bin], 1u);
    }
    GroupMemoryBarrierWithGroupSync();

    // Flush the shared bin owned by this thread into the global buffer.
    // RWByteAddressBuffer::InterlockedAdd requires an out "original value"
    // parameter (there is no 2-argument overload), so pass a dummy.
    uint count = gs_Histogram[GIdx];
    if (count > 0u)
    {
        uint prev;
        Histogram.InterlockedAdd(GIdx * 4u, count, prev);
    }
}
