#include "HeterogeneousVolumeCommon.hlsl"
#include "Noise.hlsl"

// =============================================================
//  HeterogeneousVolumeFireBake_CS
//  プロシージャルな炎 (焚き火) の密度と温度を Texture3D へ毎フレームベイクする。
//  [PORT] UE の Heterogeneous Volume は流体シミュレーション (OpenVDB) の
//  density / temperature 属性を持つ Sparse Volume Texture のフレーム列を
//  再生する。本エンジンは VDB / SVT を持たないので、同じ 2 属性を
//  時間の関数として手続き的に生成する。
//
//  出力: RWTexture3D<float2> (u0, R16G16_FLOAT)
//    r = 密度 (炎のすす + 煙。x Extinction [1/m] で消散係数になる)
//    g = 熱 0..1 (マテリアルが黒体温度へ写して発光させる)
//
//  形 (ボックス空間。Y = 高さ、底面が火床):
//    1. 柱   : 高さとともに細くなる円柱 (BaseRadius -> BaseRadius x TipRadiusScale)。
//              中心軸は高さと時間の低周波ノイズで横に揺れ (根元は動かない)、
//              断面は上へ流れるノイズで歪む (ドメインワープ = 炎の舌のうねり)
//    2. 侵食 : 上へ流れる 4D Perlin FBM (xyz = メートル, w = Evolution) で削る。
//              侵食量は高さとともに増えるので、根元はつながった炎、
//              上は舌状に裂けてちぎれた塊になる
//    3. 熱   : (柱の半径方向の落ち込み - 侵食) x HeatGain を、上ほど冷まし、
//              別のノイズで芯に明暗の筋を付ける。炎の先端 / 底 / ボックスの端で 0
//    4. 煙   : 炎の上半分から立ちのぼる薄い柱 (同じノイズで濃淡)
//
//  Dispatch: ceil(VolumeResolution / 4) の 3D
// =============================================================

RWTexture3D<float2> RWFireVolume : register(u0);

[numthreads(HETEROGENEOUS_VOLUME_THREADGROUP_SIZE, HETEROGENEOUS_VOLUME_THREADGROUP_SIZE, HETEROGENEOUS_VOLUME_THREADGROUP_SIZE)]
void main(uint3 DispatchThreadId : SV_DispatchThreadID)
{
    const uint3 voxel = DispatchThreadId;
    if (any(voxel >= (uint3) VolumeResolution.xyz))
    {
        return;
    }

    const float3 box = VoxelToBox(voxel, VolumeResolution.xyz);
    const float height = box.y * 0.5f + 0.5f; // 0 = 火床, 1 = ボックス上端

    const float baseRadius = FireShape.x;
    const float tipRadiusScale = FireShape.y;
    const float taperPower = FireShape.z;
    const float flameHeight = max(FireShape.w, 1e-3f);

    const float noiseFrequency = FireNoise.x;
    const float noiseStrength = FireNoise.y;
    const float heightErosion = FireNoise.z;
    const int noiseOctaves = clamp((int) (FireNoise.w + 0.5f), 1, 5);

    const float riseSpeed = FireMotion.x;
    const float evolution = FireMotion.w * FireMotion.y;
    const float turbulence = FireMotion.z;
    const float time = FireMotion.w;
    const float seedOffset = FireExtent.w;

    FBMNoise N;

    // ---- 1. 柱: 中心軸の横揺れ + 高さで細くなる半径 ----
    // 揺れは断面全体で共通 (高さと時間だけの関数) なので 2D ノイズで足りる
    const float2 swayCoord = float2(height * 1.6f - time * riseSpeed * 0.35f, time * 0.9f + seedOffset);
    const float2 sway = float2(
        N.Perlin2D(swayCoord),
        N.Perlin2D(swayCoord + float2(37.2f, 11.9f)));
    const float2 center = sway * turbulence * height;

    const float flameT = saturate(height / flameHeight); // 0 = 火床, 1 = 炎の先端
    const float radius = baseRadius * lerp(1.0f, tipRadiusScale, pow(flameT, max(taperPower, 0.05f)));

    // 断面のドメインワープ: 上へ流れる中周波の 3D ノイズ 2 本で XZ をずらす (舌のうねり)。
    // 根元は小さく、上ほど大きい
    float3 warpPos = box * FireExtent.xyz;
    warpPos.y -= time * riseSpeed;
    warpPos = warpPos * (noiseFrequency * 0.55f) + seedOffset * 1.3f;
    const float2 warp = float2(
        N.Perlin3D(warpPos),
        N.Perlin3D(warpPos + float3(31.4f, 7.7f, 13.1f))) * (turbulence * (0.25f + flameT));

    const float radialDistance = length(box.xz - center - warp);
    const float core = saturate(1.0f - radialDistance / max(radius, 1e-3f));

    // 煙の柱 (炎より少し細く、揺れは大きい)
    const float smokeRadius = baseRadius * 0.75f;
    const float smokeCore = saturate(1.0f - length(box.xz - center * 1.6f) / max(smokeRadius, 1e-3f));

    // 柱の外はノイズを評価しない
    [branch]
    if (core <= 0.0f && smokeCore <= 0.0f)
    {
        RWFireVolume[voxel] = float2(0.0f, 0.0f);
        return;
    }

    // ---- 2. 侵食: 上へ流れる 4D FBM (座標はメートル。ノイズを下へ流す = 炎が昇る) ----
    float3 noisePos = box * FireExtent.xyz;
    noisePos.y -= time * riseSpeed;
    noisePos = noisePos * noiseFrequency + seedOffset;
    // FBM は 0.5 付近に集まるのでコントラストを上げてから使う
    float noise = N.fbm(float4(noisePos, evolution), noiseOctaves, 2.0f, 0.55f) * 0.5f + 0.5f; // 0..1
    noise = saturate((noise - 0.5f) * 1.8f + 0.5f);

    // ---- 3. 熱 ----
    const float erosion = noiseStrength * (0.35f + heightErosion * flameT);
    float heat = core - (1.0f - noise) * erosion;
    heat *= 1.0f - smoothstep(0.8f, 1.0f, flameT); // 先端
    heat = saturate(heat * FireSmoke.w);

    // 上ほど冷め (先端は赤く暗い)、芯には別のノイズで明暗の筋を入れる
    float veins = N.fbm(float4(noisePos * 1.9f + 5.3f, evolution * 1.6f), 2, 2.0f, 0.5f) * 0.5f + 0.5f;
    veins = saturate((veins - 0.5f) * 2.0f + 0.5f);
    heat *= lerp(1.0f, 0.45f, flameT) * lerp(0.6f, 1.0f, veins);

    // ---- 4. 煙 ----
    const float smokeStart = FireSmoke.z;
    const float smokeMask = smoothstep(smokeStart, min(smokeStart + 0.25f, 1.0f), height);
    const float smoke = smokeCore * smokeMask * saturate(noise * 1.6f - 0.35f) * FireSmoke.y;

    // ---- 端のフェード (底は火床なので鋭く、側面と上はなだらかに) ----
    const float bottomFade = smoothstep(0.0f, 0.04f, height);
    const float sideFade = saturate(min(1.0f - abs(box.x), 1.0f - abs(box.z)) / 0.08f);
    const float topFade = saturate((1.0f - height) / 0.1f);
    const float fade = bottomFade * sideFade * topFade;

    heat *= fade;
    const float density = (heat * FireSmoke.x + smoke) * fade;

    RWFireVolume[voxel] = float2(density, heat);
}
