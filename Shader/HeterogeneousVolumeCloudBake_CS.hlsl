#include "HeterogeneousVolumeCommon.hlsl"
#include "Noise.hlsl"

// =============================================================
//  HeterogeneousVolumeCloudBake_CS
//  プロシージャルな単体の積雲の密度 (0..1) を Texture3D へベイクする。
//  [PORT] UE の Heterogeneous Volume は OpenVDB から取り込んだ Sparse Volume
//  Texture を入力にする。本エンジンは VDB / SVT を持たないので、
//  同じ役割の密度ボリュームをパラメータから生成する。
//
//  形 (形状空間):
//    1. 基本形 : 球 (パフ) 群のスムースユニオン。中心と半径は CPU が
//               シードから決める (FProceduralCloudParams::GeneratePuffs)。
//    2. 底面   : 水平面とのスムース交差で平らな雲底を作る
//    3. ビロー : 反転 Worley FBM (丸い塊) と Perlin FBM で SDF を押し出し、
//               カリフラワー状のこぶを作る
//    4. ディテール : 高周波の反転 Worley FBM で縁を削る
//    5. 密度   : -SDF / EdgeSoftness を 0..1 に。内部は Perlin で少しムラを付け、
//               ボックスの端は必ず 0 へフェードする (クランプサンプルで漏れない)
//
//  出力: RWTexture3D<float> (u0, R16_FLOAT)
//  Dispatch: ceil(VolumeResolution / 4) の 3D
// =============================================================

RWTexture3D<float> RWDensityVolume : register(u0);

// 多項式スムース最小 / 最大 (k = なめらかさの幅)
float SmoothMin(float a, float b, float k)
{
    float h = saturate(0.5f + 0.5f * (b - a) / max(k, 1e-5f));
    return lerp(b, a, h) - k * h * (1.0f - h);
}

float SmoothMax(float a, float b, float k)
{
    return -SmoothMin(-a, -b, k);
}

[numthreads(HETEROGENEOUS_VOLUME_THREADGROUP_SIZE, HETEROGENEOUS_VOLUME_THREADGROUP_SIZE, HETEROGENEOUS_VOLUME_THREADGROUP_SIZE)]
void main(uint3 DispatchThreadId : SV_DispatchThreadID)
{
    const uint3 voxel = DispatchThreadId;
    if (any(voxel >= (uint3) VolumeResolution.xyz))
    {
        return;
    }

    const float3 box = VoxelToBox(voxel, VolumeResolution.xyz);
    const float3 q = box * AspectScale.xyz; // 形状空間

    // ---- 1. 基本形: パフのスムースユニオン ----
    const uint numPuffs = min((uint) AspectScale.w, (uint) HETEROGENEOUS_VOLUME_MAX_PUFFS);
    float sdf = 1.0e3f;
    [loop]
    for (uint i = 0; i < numPuffs; ++i)
    {
        float d = length(q - Puffs[i].xyz) - Puffs[i].w;
        sdf = SmoothMin(sdf, d, ShapeParams.x);
    }

    // ---- 2. 平らな雲底 (底面より下を切る) ----
    sdf = SmoothMax(sdf, ShapeParams.y - q.y, ShapeParams.z);

    // 形から十分遠いボクセルはノイズを評価しない (ベイク時間の節約)
    const float maxDisplacement = BillowParams.y + DetailParams.y + ShapeParams.w;
    [branch]
    if (sdf > maxDisplacement * 1.5f + 0.02f)
    {
        RWDensityVolume[voxel] = 0.0f;
        return;
    }

    FBMNoise N;

    // ---- 3. ビロー (低周波のこぶ) ----
    const float3 billowPos = q * BillowParams.x + NoiseOffset.xyz;
    const int billowOctaves = clamp((int) (BillowParams.z + 0.5f), 1, 5);
    float billowWorley = N.worleyFbm(billowPos, billowOctaves, 2.0f, 0.5f); // 0..1
    float billowPerlin = N.fbm(billowPos * 0.6f + 11.3f, 3, 2.0f, 0.5f) * 0.5f + 0.5f; // 0..1
    float billow = lerp(billowWorley, billowPerlin, saturate(BillowParams.w));
    sdf += (0.5f - billow) * 2.0f * BillowParams.y;

    // ---- 4. ディテール (高周波の侵食) ----
    const float3 detailPos = q * DetailParams.x + NoiseOffset.zxy * 1.7f;
    const int detailOctaves = clamp((int) (DetailParams.z + 0.5f), 1, 4);
    float detail = N.worleyFbm(detailPos, detailOctaves, 2.0f, 0.5f); // 0..1
    sdf += (0.5f - detail) * 2.0f * DetailParams.y;

    // ---- 5. 密度 ----
    float density = saturate(-sdf / max(ShapeParams.w, 1e-4f));

    // 内部のムラ (中心ほど濃く、Perlin で少し抜く)
    density *= lerp(1.0f, billowPerlin, saturate(DetailParams.w));

    // ボックスの端は 0 へ
    const float3 border = 1.0f - abs(box);
    density *= saturate(min(border.x, min(border.y, border.z)) / max(NoiseOffset.w, 1e-4f));

    RWDensityVolume[voxel] = density;
}
