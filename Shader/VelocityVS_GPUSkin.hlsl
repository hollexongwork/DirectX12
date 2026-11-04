// =============================================================
//  VelocityVS_GPUSkin
//  VelocityVS の GPU スキニング順列 (FGPUSkinVertexFactory)。
//  前フレーム位置は PreviousBoneMatrices (t40) でスキンする。
//  PSO: Velocity*_GPUSkin (RenderManager::InitPipelines)
// =============================================================
#define GPUSKIN_VERTEX_FACTORY 1
#include "VelocityVS.hlsl"
