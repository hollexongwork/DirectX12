#include "VolumetricFogVoxelizationCommon.hlsl"

// =============================================================
//  VolumetricFogVoxelizationPS
//  VoxelizeVolumePrimitivePS 相当。1 ピクセル = 1 froxel。
//  属性パス (VolumetricFogAttributes_CS) と同じジッタ付きサンプル位置で
//  Volume ドメインのマテリアルを評価し、媒質属性へ変換して加算する:
//    Extinction σt = Extinction x Hardness x Noise x 境界フェード [1/m]
//    散乱 (rgb)    = σt x Albedo
//    吸収          = σt x (1 - mean(Albedo))
//    発光 (rgb)    = Emissive x 境界フェード [/m]
//    遮蔽重み      = σt x (1 - AmbientOcclusion)
//                    [PORT] UE は AO を Volumetric Fog では使わない。本エンジンは
//                    散乱パスのスカイ項を Σσt(1-AO) / Σσt で減衰させる (VBufferB.a)
//  サンプル位置が境界 (OBB / 球) の外なら discard。
//  加算ブレンド (One / One) なので複数の媒質が重なると係数が足し合わさる
//  (UE と同じ)。
//
//  ノイズ: Noise(AbsoluteWorldPosition + Panner(Speed x Time)) x Scale。
//  FilterWidth は froxel の対角長 (属性の分布に合わせて細かい
//  オクターブを落とす)。
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
    const uint shape = (uint) (mat.ExtinctionParams.w + 0.5f);
    const float edgeFalloff = saturate(mat.ExtinctionParams.z);

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

    // ---- ノイズ (Noise ノード + Panner) ----
    float noise = 1.0f;

    [branch]
    if (mat.NoiseFlags.x > 0.5f)
    {
        const float scale = mat.NoiseParams.x;
        const float time = TemporalParams.w;
        float3 noisePos = (worldPos + mat.NoisePanner.xyz * time) * scale;

        float filterWidth = 0.0f;
        [flatten]
        if (mat.NoiseFlags.z > 0.5f)
        {
            // froxel の対角長 [m] (散乱パスの cellRadius と同じ) をノイズ空間へ
            float neighborDepth;
            float3 neighborPos = ComputeCellWorldPosition(gridCoordinate + uint3(1, 1, 1), FrameJitter.xyz, neighborDepth);
            filterWidth = length(worldPos - neighborPos) * scale;
        }

        noise = MaterialExpressionNoise(noisePos,
            (uint) (mat.NoiseParams.y + 0.5f), mat.NoisePanner.w, mat.NoiseFlags.y > 0.5f,
            mat.NoiseParams.z, mat.NoiseParams.w, filterWidth);
    }

    // ---- 媒質属性 ----
    const float extinction = max(mat.ExtinctionParams.x * mat.ExtinctionParams.y * noise * shapeMask, 0.0f);
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
