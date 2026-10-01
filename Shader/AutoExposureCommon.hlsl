#ifndef AUTO_EXPOSURE_COMMON_HLSL
#define AUTO_EXPOSURE_COMMON_HLSL

// ============================================================
//  AutoExposureCommon.hlsl
//
//  Shared by the two auto-exposure compute passes
//  (AutoExposureHistogram_CS / AutoExposureAverage_CS), which use
//  their own compute root signature (independent of the graphics RS).
//
//  EXPOSURE_PARAMS must mirror the C++ AutoExposure::EXPOSURE_PARAMS
//  (AutoExposure.h), and HISTOGRAM_BINS must match
//  AutoExposure::HISTOGRAM_BINS.
// ============================================================

#define HISTOGRAM_BINS 256

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

#endif
