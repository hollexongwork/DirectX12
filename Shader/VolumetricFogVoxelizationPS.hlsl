#include "VolumetricFogVoxelizationCommon.hlsl"

// =============================================================
//  VolumetricFogVoxelizationPS
//  VoxelizeVolumePrimitivePS 相当。1 ピクセル = 1 froxel。
//  属性パス (VolumetricFogAttributes_CS) と同じジッタ付きサンプル位置で
//  Volume ドメインのマテリアルを評価し、媒質属性へ変換して加算する:
//    Extinction σt = Extinction x pow(Noise x Multiply, Power) x 境界フェード [1/m]
//    散乱 (rgb)    = σt x Albedo
//    吸収          = σt x (1 - mean(Albedo))
//    発光 (rgb)    = Emissive x 境界フェード [/m]
//    遮蔽重み      = σt x (1 - AmbientOcclusion)
//                    [PORT] UE は AO を Volumetric Fog では使わない。本エンジンは
//                    散乱パスのスカイ項を Σσt(1-AO) / Σσt で減衰させる (VBufferB.a)
//  サンプル位置が境界 (OBB / 球) の外なら discard。
//  加算ブレンド (One / One) なので複数の媒質が重なると係数が足し合わさる
//
//  ノイズ (マテリアル関数 "Fbm Perlin noise", Noise.hlsl):
//    Position  = (AbsoluteWorldPosition + OffsetSpeed x Time) x Scale
//    Evolution = frac(Time x EvolutionSpeed x 0.001) x 1000   (4D の 4 軸目)
//    Noise     = FbmPerlinNoise(Position, Evolution, Octaves, Lacunarity, Gain, Dimension)  0..1
//  Position がそのままノイズ空間 (格子 1 セル = 1.0)。Dimension (NoiseParams.z) で
//  2D (XZ) / 3D / 4D を選ぶ。
// =============================================================

struct FVoxelizeVolumePSOutput
{
    float4 VBufferA : SV_Target0; // rgb = 散乱係数 [1/m], a = 吸収係数 [1/m]
    float4 VBufferB : SV_Target1; // rgb = 発光 [/m], a = 遮蔽重み σt(1-AO)
};

FVoxelizeVolumePSOutput main(FVoxelizeVolumeGSToPS Input)
{
    const uint3 gridCoordinate = uint3((uint2) Input.Position.xy, Input.SliceIndex);

    float sceneDepth;
    float3 worldPos = ComputeCellWorldPosition(gridCoordinate, FrameJitter.xyz, sceneDepth);

    const FVolumeMaterialParameters mat = VolumeMaterial;
    const uint shape = (uint) (mat.Emissive.w + 0.5f);
    const float edgeFalloff = saturate(mat.ExtinctionParams.w);

    // ---- 境界の内外判定 + 境界フェード ----
    // dist = 形状の中心 0 .. 境界 1 の正規化距離
    float dist;

    [branch]
    if (shape == VOLUME_SHAPE_SPHERE)
    {
        dist = length(worldPos - BoundsSphere.xyz) / max(BoundsSphere.w, 1e-4f);
    }
    else
    {
        float3 boxPos = mul(float4(worldPos, 1.0f), WorldToBox).xyz;
        float3 a = abs(boxPos);
        dist = max(a.x, max(a.y, a.z));
    }

    if (dist > 1.0f)
    {
        discard;
    }

    float shapeMask = 1.0f;
    [flatten]
    if (edgeFalloff > 0.0f)
    {
        shapeMask = 1.0f - smoothstep(1.0f - edgeFalloff, 1.0f, dist);
    }

    // ---- ノイズ (Fbm Perlin noise) ----
    float density = 1.0f;

    [branch]
    if (mat.NoiseParams.w > 0.5f)
    {
        const float time = TemporalParams.w;
        float3 noisePos = (worldPos + mat.NoiseOffsetSpeed.xyz * time) * mat.NoiseScale.xyz;
        float evolution = frac(time * mat.NoiseOffsetSpeed.w * 0.001f) * 1000.0f;

        float noise = FbmPerlinNoise(noisePos, evolution,
            (int) (mat.NoiseParams.x + 0.5f), mat.NoiseParams.y, mat.NoiseScale.w,
            (int) (mat.NoiseParams.z + 0.5f));

        // Multiply -> Power (グラフの x1.5 -> Power 5)
        density = pow(max(noise * mat.ExtinctionParams.y, 0.0f), max(mat.ExtinctionParams.z, 1e-3f));
    }

    // ---- 媒質属性 ----
    const float extinction = max(mat.ExtinctionParams.x * density * shapeMask, 0.0f);
    const float3 albedo = saturate(mat.Albedo.rgb);
    const float ao = saturate(mat.Albedo.w);

    float3 scattering = extinction * albedo;
    float absorption = extinction * (1.0f - dot(albedo, float3(1.0f, 1.0f, 1.0f) / 3.0f));
    float3 emissive = max(mat.Emissive.rgb, 0.0f) * shapeMask;

    FVoxelizeVolumePSOutput output;
    output.VBufferA = float4(scattering, absorption);
    output.VBufferB = float4(emissive, extinction * (1.0f - ao));
    return output;
}
