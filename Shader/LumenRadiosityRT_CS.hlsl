// =============================================================
//  LumenRadiosityRT_CS
//  LumenRadiosity_CS の DXR (RayQuery, SM 6.5) バリアント。
//  LUMEN_HWRT=1 で TraceLumenRay が RayQuery インライントレース
//  (LumenTracingHardware.hlsl) に切り替わる。TLAS はルート SRV
//  t28 (LumenHardwareRayTracing.h) にバインドされる。
// =============================================================
#define LUMEN_HWRT 1
#include "LumenRadiosity_CS.hlsl"
