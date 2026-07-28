// ============================================================
//  AutoExposureAverage_CS.hlsl
//
//  Eye adaptation - Pass 2 of 2 (resolve + adapt).
//
//  Single-thread-group reduction over the 256-bin luminance
//  histogram produced by AutoExposureHistogram_CS. Steps:
//
//   1. Sum the histogram to get the total weighted pixel count.
//   2. Walk the bins discarding a low/high percentile of pixels
//       so a few very dark or very bright pixels don't dominate.
//   3. Compute the weighted-average log luminance of the kept
//      range and convert back to a linear average luminance.
//   4. Convert that to a target exposure scale, clamp to the
//      Min/Max brightness window, apply exposure compensation.
//   5. Exponentially blend from the previous frame's exposure
//      toward the target using SpeedUp / SpeedDown , giving 
//      smooth eye adaptation.
//
//  Output: a single float exposure scale at Result[0], which the
//  tonemap pass multiplies into SceneColor. Result[0] persists
//  across frames (read previous value, write new) so adaptation
//  is stateful exactly like EyeAdaptation texture.
//
//  Root signature (compute, independent):
//    b0 : EXPOSURE_PARAMS    (same layout as the histogram pass)
//    u0 : Histogram          (RWByteAddressBuffer, 256 * uint)   [read]
//    u1 : Result             (RWByteAddressBuffer, >=2 * float)  [read+write]
//         Result[0] = adapted exposure scale (used by tonemap)
//         Result[1] = adapted average luminance (for ImGui readout)
// ============================================================

#define HISTOGRAM_BINS 256

cbuffer EXPOSURE_PARAMS : register(b0)
{
    uint SceneWidth;
    uint SceneHeight;
    float MinLogLuminance;
    float MaxLogLuminance;

    float LowPercent;
    float HighPercent;
    float MinBrightness;
    float MaxBrightness;

    float SpeedUp;
    float SpeedDown;
    float ExposureCompensation;
    float DeltaTime;
};

RWByteAddressBuffer Histogram : register(u0);
RWByteAddressBuffer Result : register(u1);

groupshared float gs_Weighted[HISTOGRAM_BINS];

// Inverse of the histogram bin mapping (bins 1..254 carry signal,
// bin 0 is the reserved black bucket, bin 255 is the saturated bucket).
float BinToLogLuminance(uint bin)
{
    float t = ((float) bin - 1.0f) / (float) (HISTOGRAM_BINS - 2);
    return lerp(MinLogLuminance, MaxLogLuminance, t);
}

[numthreads(HISTOGRAM_BINS, 1, 1)]
void main(uint GIdx : SV_GroupIndex)
{
    // Load this thread's bin count and stash a per-bin weighted log term.
    uint binCount = Histogram.Load(GIdx * 4u);

    // Bin 0 (near-black) contributes to the total count for percentile
    // accounting but carries no luminance weight.
    float logLum = (GIdx == 0u) ? 0.0f : BinToLogLuminance(GIdx);
    gs_Weighted[GIdx] = (GIdx == 0u) ? 0.0f : logLum * (float) binCount;

    GroupMemoryBarrierWithGroupSync();

    // Single-thread serial reduction (256 bins is tiny). Thread 0 does the
    // percentile clipping + averaging; all the data it needs is either in
    // the histogram buffer or gs_Weighted.
    if (GIdx == 0u)
    {
        // Total pixel count across all bins.
        uint total = 0u;
        for (uint i = 0u; i < HISTOGRAM_BINS; ++i)
            total += Histogram.Load(i * 4u);

        float exposureScale;
        float avgLum;

        if (total == 0u)
        {
            // Nothing to measure (e.g. first frame / black frame): hold.
            // RWByteAddressBuffer.Load() returns raw uint bits; the stored
            // values are floats written via asuint(), so reinterpret back
            // with asfloat() instead of doing a numeric uint->float convert.
            exposureScale = asfloat(Result.Load(0u)); // keep previous
            avgLum = asfloat(Result.Load(4u));
            if (exposureScale <= 0.0f)
                exposureScale = 1.0f;
        }
        else
        {
            // Percentile window in pixel counts.
            float fTotal = (float) total;
            float loCount = LowPercent * fTotal;
            float hiCount = HighPercent * fTotal;

            float sumWeightedLog = 0.0f;
            float sumWeight = 0.0f;
            float running = 0.0f;

            // Walk bins 1..255 (skip the black bucket), trimming the
            // low/high percentile tails before averaging.
            for (uint b = 1u; b < HISTOGRAM_BINS; ++b)
            {
                float c = (float) Histogram.Load(b * 4u);
                if (c <= 0.0f)
                    continue;

                float binStart = running;
                float binEnd = running + c;
                running = binEnd;

                // Clip against [loCount, hiCount].
                float lo = max(binStart, loCount);
                float hi = min(binEnd, hiCount);
                float kept = max(hi - lo, 0.0f);
                if (kept <= 0.0f)
                    continue;

                float ll = BinToLogLuminance(b);
                sumWeightedLog += ll * kept;
                sumWeight += kept;
            }

            float avgLogLum = (sumWeight > 0.0f)
                ? (sumWeightedLog / sumWeight)
                : 0.5f * (MinLogLuminance + MaxLogLuminance);

            avgLum = exp2(avgLogLum);
            avgLum = clamp(avgLum, MinBrightness, MaxBrightness);

            // Key-value exposure: scale the average luminance to a mid-grey
            // target (0.18) the way basic auto-exposure does, then
            // apply exposure compensation in EV stops.
            float targetExposure = 0.18f / max(avgLum, 1e-4f);
            targetExposure *= exp2(ExposureCompensation);

            // Temporal eye adaptation: exponential approach from the
            // previous exposure toward the target. SpeedUp is used when the
            // scene gets brighter (exposure must DECREASE), SpeedDown when it
            // gets darker (exposure must INCREASE)
            float prev = asfloat(Result.Load(0u));
            if (prev <= 0.0f)
                prev = targetExposure; // first valid frame

            float speed = (targetExposure < prev) ? SpeedUp : SpeedDown;
            float blend = 1.0f - exp(-DeltaTime * speed);
            exposureScale = lerp(prev, targetExposure, saturate(blend));
        }

        Result.Store(0u, asuint(exposureScale));
        Result.Store(4u, asuint(avgLum));
    }
}
