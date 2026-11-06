#ifndef VOLUMETRIC_FOG_VOXELIZATION_COMMON_HLSL
#define VOLUMETRIC_FOG_VOXELIZATION_COMMON_HLSL

// =============================================================
//  VolumetricFogVoxelizationCommon
//  VolumetricFogVoxelization.usf 相当 - Volume ドメインの
//  マテリアルを持つプリミティブを Volumetric Fog の froxel
//  ボリューム (VBufferA / VBufferB) へボクセル化するグラフィックス
//  パスの共通部 (FVolumetricFog::VoxelizeFogVolumePrimitives)。
//
//  パイプライン:
//    VS (VolumetricFogVoxelizationVS) : 頂点無し。SV_InstanceID = ボクセル化
//                                       パス番号 (8 スライスごと) を GS へ渡す
//    GS (VolumetricFogVoxelizationGS) : プリミティブの境界 (OBB / 球) を
//                                       ビュー空間へ写し、覆う froxel の
//                                       XY 矩形と Z スライス範囲を求めて
//                                       スライスごとに 1 クアッドを
//                                       SV_RenderTargetArrayIndex = スライス
//                                       で出力する (UE と同じく GS 1 回あたり
//                                       MAX_SLICES_PER_VOXELIZATION_PASS 枚)
//    PS (VolumetricFogVoxelizationPS) : froxel のサンプル位置 (属性パスと
//                                       同じジッタ) でボリュームマテリアルを
//                                       評価し、散乱 / 吸収 / 発光を加算
//                                       ブレンド (One / One) で VBuffer へ足す
//
//  Root signature (graphics, コンピュート RS / 既定 RS から独立):
//    b0 : FVolumetricFogParams (コンピュート 3 パスと同じ定数)
//    b1 : FVoxelizeVolumeElementParams (プリミティブ + マテリアル)
//  レンダーターゲット: RT0 = VBufferA (散乱 rgb, 吸収),
//                      RT1 = VBufferB (発光 rgb, 遮蔽重み)
//  ビューポート = GridSize.xy (1 px = 1 froxel)。深度無し。
//
//  [PORT] UE はメッシュバッチの三角形を GS でスライス複製して
//  ラスタライズするが、ボクセル化されるのはプリミティブの境界
//  (UE のドキュメント「Volume マテリアルはメッシュの境界に適用される」)
//  なので、本エンジンは境界 (OBB / 球) を直接ラスタライズする。
// =============================================================

#include "VolumetricFogGridCommon.hlsl"
#include "Noise.hlsl"

// GS 1 回 (= 1 インスタンス) が出力するスライス数。
// 4 頂点 x 8 枚 x (float4 + uint) = 160 スカラー (GS の上限 1024 以内)
#define MAX_SLICES_PER_VOXELIZATION_PASS 8

#define VOLUME_SHAPE_BOX 0
#define VOLUME_SHAPE_SPHERE 1

// C++ 側 (Material.h) の FVolumeMaterialParameters と 1:1 ミラー (96 bytes)
struct FVolumeMaterialParameters
{
    float4 Albedo; // rgb = 散乱アルベド, w = AmbientOcclusion
    float4 Emissive; // rgb = 発光 [/m], w = 未使用
    float4 ExtinctionParams; // x = Extinction [1/m], y = Hardness, z = EdgeFalloff (0..1), w = EVolumeShape
    float4 NoiseParams; // x = Scale [1/m], y = Levels, z = OutputMin, w = OutputMax
    float4 NoisePanner; // xyz = Panner Speed [m/s], w = LevelScale
    float4 NoiseFlags; // x = bUseNoise, y = bTurbulence, z = bUseFilterWidth, w = 未使用
};

// C++ 側 (VolumetricFog.h) の FVoxelizeVolumeElementParams と 1:1 ミラー (240 bytes)
cbuffer FVoxelizeVolumeElementParams : register(b1)
{
    float4x4 BoxToWorld; // 単位ボックス [-1,1]^3 -> ワールド (転置済み)
    float4x4 WorldToBox; // ワールド -> 単位ボックス (転置済み)
    float4 BoundsSphere; // xyz = 境界球の中心 [m], w = 半径 [m]
    FVolumeMaterialParameters VolumeMaterial;
};

struct FVoxelizeVolumeVSToGS
{
    uint PassIndex : PASSINDEX; // SV_InstanceID (スライス 8 枚ごとのパス番号)
};

struct FVoxelizeVolumeGSToPS
{
    float4 Position : SV_POSITION; // froxel グリッドのクリップ座標 (ビューポート = GridSize.xy)
    uint SliceIndex : SV_RenderTargetArrayIndex; // 書き込み先 Z スライス
};

#endif
