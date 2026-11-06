#include "VolumetricFogVoxelizationCommon.hlsl"

// =============================================================
//  VolumetricFogVoxelizationVS
//  VoxelizeVolumePrimitiveVS 相当。頂点バッファは無く、1 プリミティブ
//  = DrawInstanced(1, NumPasses) の点 1 個。SV_InstanceID がボクセル化
//  パス番号 (担当する Z スライスの先頭 = PassIndex x 8) になる。
//  境界の計算は GS がまとめて行う。
// =============================================================

FVoxelizeVolumeVSToGS main(uint InstanceId : SV_InstanceID)
{
    FVoxelizeVolumeVSToGS output;
    output.PassIndex = InstanceId;
    return output;
}
