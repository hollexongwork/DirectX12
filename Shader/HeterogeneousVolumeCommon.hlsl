#ifndef HETEROGENEOUS_VOLUME_COMMON_HLSL
#define HETEROGENEOUS_VOLUME_COMMON_HLSL

// =============================================================
//  HeterogeneousVolumeCommon
//  Heterogeneous Volumes (HeterogeneousVolumes.cpp 相当) のコンピュートパス共通部。
//
//    1. HeterogeneousVolumeCloudBake_CS     : プロシージャル雲の密度を
//                                             Texture3D (R16F) へベイク
//                                             (UE の Sparse Volume Texture の代わり)
//    1'. HeterogeneousVolumeFireBake_CS     : プロシージャルな炎の密度 + 熱を
//                                             Texture3D (RG16F) へ毎フレームベイク
//                                             (SVT のフレーム列の代わり)
//    2. HeterogeneousVolumeLightingCache_CS : 各ボクセルから太陽 / 上空 / 地面方向へ
//                                             密度をレイマーチし、光学的長さ
//                                             ∫density ds [m] を Texture3D (RGBA16F) へ
//                                             (UE の Lighting Cache = Transmittance Volume)
//    3. HeterogeneousVolumePS (グラフィックス, 既定 RS) : OBB をレイマーチして
//                                             SceneColor へ合成
//
//  Root signature (compute, FHeterogeneousVolumeRenderer 専用):
//    b0 : FHeterogeneousVolumeComputeParams (ルート CBV)
//    t0 : DensityVolume (Texture3D<float>。ライティングキャッシュの入力)
//    u0 : 出力 (ベイク = RWTexture3D<float>, キャッシュ = RWTexture3D<float4>)
//    s0 : 線形クランプ
//
//  ---- 座標系 ----
//    ボックス空間 : ボリュームの単位ボックス [-1, 1]^3 (コンポーネントの
//                   LocalToWorld がワールドの OBB へ写す。スケール = 半幅 [m])
//    UVW          : ボックス空間 * 0.5 + 0.5 (テクスチャ座標)
//    形状空間     : ボックス空間 x AspectScale (= 半幅 / 最大半幅)。
//                   ノイズと球が非一様スケールで潰れないよう、雲の形は
//                   ここで定義する (最大半幅の軸が ±1)
//    レイのパラメータ t は常にワールドのメートル。方向ベクトルは
//    「ワールド 1 m あたりのボックス空間の移動量」で渡す
// =============================================================

#define HETEROGENEOUS_VOLUME_THREADGROUP_SIZE 4   // 4x4x4
#define HETEROGENEOUS_VOLUME_MAX_PUFFS 32
#define HETEROGENEOUS_VOLUME_NUM_AMBIENT_DIRECTIONS 5

// C++ 側 (HeterogeneousVolumes.h) の FHeterogeneousVolumeComputeParams と 1:1 ミラー必須 (832 bytes)
cbuffer FHeterogeneousVolumeComputeParams : register(b0)
{
    float4 VolumeResolution; // xyz = 密度ボリュームの解像度, w = 未使用
    float4 CacheResolution; // xyz = ライティングキャッシュの解像度, w = 未使用
    float4 AspectScale; // xyz = 半幅 / 最大半幅 (形状空間のスケール), w = NumPuffs

    // ---- 雲の形 (FProceduralCloudParams) ----
    float4 ShapeParams; // x = SmoothUnion k, y = 底面の高さ (形状空間 Y), z = 底面のなめらかさ, w = EdgeSoftness
    float4 BillowParams; // x = Frequency, y = Strength, z = Octaves, w = PerlinMix (0 = Worley のみ)
    float4 DetailParams; // x = Frequency, y = Strength, z = Octaves, w = DensityVariation (内部のムラ 0..1)
    float4 NoiseOffset; // xyz = シード由来のノイズ空間オフセット, w = BorderFade (ボックス端のフェード幅)

    // ---- ライティングキャッシュ ----
    float4 SunDirectionBox; // xyz = 太陽方向 (受光点 -> ライト) のボックス空間移動量 / m, w = 有効 (0/1)
    float4 ShadowParams; // x = ShadowStepSize [m], y = MaxShadowStepCount, z = ShadowBias [m], w = AmbientStepCount
    float4 AmbientDirectionBox[HETEROGENEOUS_VOLUME_NUM_AMBIENT_DIRECTIONS]; // 上空コーン 5 方向 (ボックス空間 / m)
    float4 GroundDirectionBox; // xyz = 真下 (ボックス空間 / m)

    float4 Puffs[HETEROGENEOUS_VOLUME_MAX_PUFFS]; // xyz = 中心 (形状空間), w = 半径

    // ---- 炎の形 (FProceduralFireParams。HeterogeneousVolumeFireBake_CS) ----
    float4 FireShape; // x = BaseRadius (ボックス空間 XZ), y = TipRadiusScale, z = TaperPower, w = FlameHeight (ボックスの高さ比)
    float4 FireNoise; // x = NoiseFrequency [1/m], y = NoiseStrength, z = HeightErosion, w = Octaves
    float4 FireMotion; // x = RiseSpeed [m/s], y = EvolutionSpeed [1/s], z = Turbulence (横揺れ, ボックス空間), w = Time [s]
    float4 FireSmoke; // x = FlameOpacity, y = SmokeDensity, z = SmokeStart (高さ比), w = HeatGain
    float4 FireExtent; // xyz = 半幅 [m], w = シード由来のノイズ空間オフセット
};

SamplerState LinearClampSampler : register(s0);

// ボクセル中心のボックス空間座標 ([-1, 1]^3)
float3 VoxelToBox(uint3 Voxel, float3 Resolution)
{
    return ((float3(Voxel) + 0.5f) / Resolution) * 2.0f - 1.0f;
}

#endif
