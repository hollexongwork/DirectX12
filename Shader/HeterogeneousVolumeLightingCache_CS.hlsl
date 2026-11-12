#include "HeterogeneousVolumeCommon.hlsl"

// =============================================================
//  HeterogeneousVolumeLightingCache_CS
//  Lighting Cache (RenderLightingCacheWithLiveShading 相当) の
//  本エンジン版。キャッシュの各ボクセルから密度ボリュームをレイマーチし、
//  ボックスを出るまでの光学的長さ ∫density ds [m] を書く:
//    R : 太陽方向 (選択されたフォワードディレクショナルライト)
//    G : 上空コーン (真上 + 55 度傾けた 4 方向) の平均 = 天空光の遮蔽
//    B : 真下 = 地面からの照り返しの遮蔽
//  透過率は描画側が exp(-Extinction x 長さ) で求める。消散係数を掛ける前の
//  長さで持つので、マテリアルの Extinction / Albedo を変えてもキャッシュは
//  作り直さない (密度 / ライト方向 / 変換が変わった時だけ更新する)。
//
//  [PORT] UE はライトごとにキャッシュを作り、全ライト種に対応する。
//  本エンジンは太陽 1 灯 + 天空光のみ。
//
//  出力: RWTexture3D<float4> (u0, RGBA16F)
//  Dispatch: ceil(CacheResolution / 4) の 3D
// =============================================================

Texture3D<float> DensityVolume : register(t0);
RWTexture3D<float4> RWLightingCache : register(u0);

// ボックス空間の点 Box から方向 DirBox (ボックス空間 / m) へ進んで
// 単位ボックス [-1,1]^3 を出るまでの距離 [m]
float ComputeExitDistance(float3 Box, float3 DirBox)
{
    // 0 成分は「その軸では出ない」(十分大きい値)
    float3 safeDir = float3(
        abs(DirBox.x) > 1e-6f ? DirBox.x : 1e-6f,
        abs(DirBox.y) > 1e-6f ? DirBox.y : 1e-6f,
        abs(DirBox.z) > 1e-6f ? DirBox.z : 1e-6f);
    float3 t1 = (-1.0f - Box) / safeDir;
    float3 t2 = (1.0f - Box) / safeDir;
    float3 tFar = max(t1, t2);
    return max(min(tFar.x, min(tFar.y, tFar.z)), 0.0f);
}

// 光学的長さ ∫density ds [m]。StepSize [m] ごとに採り、MaxSteps を超える分は
// ステップを伸ばして出口まで必ず届かせる
float MarchOpticalLength(float3 Box, float3 DirBox, float StepSize, uint MaxSteps, float Bias)
{
    const float exitDistance = ComputeExitDistance(Box, DirBox);
    const float marchLength = exitDistance - Bias;
    if (marchLength <= 0.0f)
    {
        return 0.0f;
    }

    const uint numSteps = clamp((uint) ceil(marchLength / max(StepSize, 1e-4f)), 1u, max(MaxSteps, 1u));
    const float dt = marchLength / (float) numSteps;

    float sum = 0.0f;
    [loop]
    for (uint i = 0; i < numSteps; ++i)
    {
        float3 p = Box + DirBox * (Bias + ((float) i + 0.5f) * dt);
        sum += DensityVolume.SampleLevel(LinearClampSampler, p * 0.5f + 0.5f, 0.0f);
    }
    return sum * dt;
}

[numthreads(HETEROGENEOUS_VOLUME_THREADGROUP_SIZE, HETEROGENEOUS_VOLUME_THREADGROUP_SIZE, HETEROGENEOUS_VOLUME_THREADGROUP_SIZE)]
void main(uint3 DispatchThreadId : SV_DispatchThreadID)
{
    const uint3 voxel = DispatchThreadId;
    if (any(voxel >= (uint3) CacheResolution.xyz))
    {
        return;
    }

    const float3 box = VoxelToBox(voxel, CacheResolution.xyz);

    const float stepSize = ShadowParams.x;
    const uint maxSteps = (uint) ShadowParams.y;
    const float bias = ShadowParams.z;
    const uint ambientSteps = max((uint) ShadowParams.w, 1u);

    // ---- R: 太陽 ----
    float sunLength = 0.0f;
    [branch]
    if (SunDirectionBox.w > 0.5f)
    {
        sunLength = MarchOpticalLength(box, SunDirectionBox.xyz, stepSize, maxSteps, bias);
    }

    // ---- G: 上空コーンの平均 (粗いステップで十分) ----
    float skyLength = 0.0f;
    [unroll]
    for (uint d = 0; d < HETEROGENEOUS_VOLUME_NUM_AMBIENT_DIRECTIONS; ++d)
    {
        skyLength += MarchOpticalLength(box, AmbientDirectionBox[d].xyz, stepSize * 2.0f, ambientSteps, bias);
    }
    skyLength /= (float) HETEROGENEOUS_VOLUME_NUM_AMBIENT_DIRECTIONS;

    // ---- B: 真下 ----
    float groundLength = MarchOpticalLength(box, GroundDirectionBox.xyz, stepSize * 2.0f, ambientSteps, bias);

    RWLightingCache[voxel] = float4(sunLength, skyLength, groundLength, 0.0f);
}
