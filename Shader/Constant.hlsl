#ifndef CONSTANT_HLSL
#define CONSTANT_HLSL

static const float PI = 3.14159265358979323846f;
static const float TWO_PI = 6.28318530717958647692f;
static const float INV_PI = 0.31830988618379067154f;
static const float HALF_PI = 1.57079632679489661923f;
static const float EPSILON = 1e-7f;

// ---- 単位変換 (Substrate の MFP / Thickness は UE 同様 cm 単位で
//      オーサリングし、メートル単位のワールドへ変換して評価する) ----
#define CENTIMETER_TO_METER 0.01f
#define METER_TO_CENTIMETER 100.0f

// Substrate: Transmittance Color を実現する固定参照厚 [cm]。
// サンプルは MFP 導出距離にノードの Thickness を
// そのまま使うため、SSS 評価厚 (同じ Thickness) と厳密に相殺して
// τ = -log(T) 恒等となり、Thickness がマテリアルの見た目に影響しない。
// 本エンジンは MFP 導出距離のみをこの固定参照厚に変更し、
//   τ = Thickness x (-log T) / 参照厚
// として Thickness を濃度スケールに実効化する。
// -> Transmittance Color は「参照厚 (1cm) あたりの透過率」の意味。
//    Thickness = 1cm で T が厳密に実現され、2cm で T^2、0.5cm で √T。
// 呼出規約 (MFP -> SSSMFP / Thickness -> SSSMFPScale / Slab 厚 0.01
// 固定) はサンプルのまま維持している。
#define SUBSTRATE_TRANSMITTANCE_REFERENCE_CM 1.0f

#endif
