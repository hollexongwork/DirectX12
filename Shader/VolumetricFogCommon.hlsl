#ifndef VOLUMETRIC_FOG_COMMON_HLSL
#define VOLUMETRIC_FOG_COMMON_HLSL

// =============================================================
//  VolumetricFogCommon
//  Volumetric Fog のコンピュートパス共通部。
//  Volumetric Fog は視錐台を XY = GridPixelSize (8px) タイル、
//  Z = 指数分布 GridSizeZ (64) スライスの froxel ボリュームに切り、
//  3 つのコンピュートパスで「カメラから各 froxel までの累積
//  インスキャッタ + 透過率」(IntegratedLightScattering, t34) を作る:
//
//    1. VolumetricFogAttributes_CS      : 指数高さフォグの密度 ->
//                                         散乱 / 吸収 (VBufferA) + 発光 (VBufferB)
//    1b. VolumetricFogVoxelization VS/GS/PS (グラフィックス):
//                                         Material Domain = Volume のプリミティブの
//                                         境界を VBufferA / VBufferB へ加算
//                                         (VBufferB.a = Σσt(1-AO) スカイ遮蔽重み)
//    2. VolumetricFogLightScattering_CS : 各 froxel のインスキャッタ
//                                         (ディレクショナル + CSM 影,
//                                          ローカルライト + シャドウマップ,
//                                          スカイ (IBL irradiance)) x 位相関数
//                                         + テンポラル再投影
//    3. VolumetricFogIntegration_CS     : Z 方向に前から後ろへ積分
//                                         (Frostbite のエネルギー保存積分)
//
//  Root signature (compute, グラフィックス RS から独立):
//    b0 : FVolumetricFogParams (cbuffer)
//    t0 : ForwardLightBuffer        t1 : LocalShadowParams
//    t2 : NumCulledLightsGrid       t3 : CulledLightDataGrid
//    t4 : DirectionalShadowCascades t5 : LocalLightShadows
//    t6 : VBufferA                  t7 : VBufferB
//    t8 : LightScatteringHistory    t9 : LightScatteringTexture
//    t10: SkyIrradiance (IBL irradiance キューブ)
//    u0 : RWVBufferA                u1 : RWVBufferB
//    u2 : RWLightScattering         u3 : RWIntegratedLightScattering
//    s0 : 線形クランプ              s1 : シャドウ比較 (LESS_EQUAL, ボーダー白)
//
//  このファイルは b0 / t0.. を独自所有するため、レジスタを宣言する
//  共有ヘッダ (Common.hlsl 系) は include しない。構造体は
//  Structs.hlsl / LightData.hlsl、ライトの減衰と面光源の積分は
//  DeferredLightingCommon.hlsl の前半 (NON_DIRECTIONAL_DIRECT_LIGHTING)、
//  シャドウ投影は ShadowProjectionCommon.hlsl、
//  定数 (PI) は Constant.hlsl、輝度 (Luminance) は ColorSpace.hlsl
//  (いずれもレジスタ非依存) から取り込む。ライトグリッドの
//  セル計算は LightGridCommon.hlsl と同式 (b3 の値を b0 経由で受ける)。
//
//  ---- 実装上の規約 ----
//    - Y-up / メートル単位 (高さ = WorldPosition.y、密度 [1/m])
//    - 位相関数は標準 HG: cosθ = dot(ToLight, CameraVector)
//    - スカイライトの SH の代わりに IBL irradiance キューブ (t10) を
//      視線方向で採光し /π した値を等方インスキャッタとする
//    - DF シャドウ (bUseRayTracedDistanceFieldShadows) のライトは
//      ボリューム内では影なし (シャドウマップのライトのみ遮蔽)
//    - 影付きローカルライトの注入は別パスにせず、
//      同じループ内にインライン評価する
//    - HistoryMissSupersampleCount (履歴外セルの追加サンプル) は未実装
// =============================================================

#define NON_DIRECTIONAL_DIRECT_LIGHTING 1

#include "Structs.hlsl"
#include "LightData.hlsl"
#include "DeferredLightingCommon.hlsl"
#include "ShadowProjectionCommon.hlsl"
#include "Constant.hlsl"
#include "ColorSpace.hlsl"

#define VOLUMETRIC_FOG_THREADGROUP_SIZE 4   // 4x4x4
#define VOLUMETRIC_FOG_INTEGRATION_GROUP_SIZE 8   // 8x8x1 (FinalIntegration)

#include "VolumetricFogGridCommon.hlsl"

StructuredBuffer<FLocalLightData> ForwardLightBuffer : register(t0); // t13 と同じライトバッファ
StructuredBuffer<FLocalShadowParameters> LocalShadowParams : register(t1);
StructuredBuffer<uint> NumCulledLightsGrid : register(t2);
StructuredBuffer<uint> CulledLightDataGrid : register(t3);
Texture2DArray<float> DirectionalShadowCascades : register(t4);
Texture2DArray<float> LocalLightShadows : register(t5);
Texture3D<float4> VBufferA : register(t6); // rgb = 散乱係数 [1/m], a = 吸収係数 [1/m]
Texture3D<float4> VBufferB : register(t7); // rgb = 発光 [/m]
Texture3D<float4> LightScatteringHistory : register(t8); // 前フレームの LightScattering
Texture3D<float4> LightScatteringTexture : register(t9); // 今フレームの LightScattering (積分パス入力)
TextureCube<float4> SkyIrradiance : register(t10); // IBL irradiance (スカイ項)

RWTexture3D<float4> RWVBufferA : register(u0);
RWTexture3D<float4> RWVBufferB : register(u1);
RWTexture3D<float4> RWLightScattering : register(u2); // rgb = インスキャッタ [/m], a = 消散係数 [1/m]
RWTexture3D<float4> RWIntegratedLightScattering : register(u3); // rgb = 累積インスキャッタ, a = 透過率

SamplerState LinearClampSampler : register(s0);
SamplerComparisonState ShadowCmpSampler : register(s1);

// -------------------------------------------------------------
//  froxel Z スライス <-> ビュー深度
//    Slice = log2(Depth * B + O) * S  /  Depth = (2^(Slice / S) - O) / B
// -------------------------------------------------------------
float HenyeyGreensteinPhase(float g, float CosTheta)
{
    float g2 = g * g;
    float denom = max(1.0f + g2 - 2.0f * g * CosTheta, 1e-4f);
    return (1.0f - g2) / (4.0f * PI * pow(denom, 1.5f));
}

// -------------------------------------------------------------
//  ライトグリッドのセル参照 (LightGridCommon.hlsl と同式)
// -------------------------------------------------------------
uint VF_ComputeLightGridZSlice(float SceneDepth)
{
    return (uint) max(0.0f, log2(SceneDepth * LightGridZParams.x + LightGridZParams.y) * LightGridZParams.z);
}

uint VF_ComputeLightGridCellIndex(uint2 PixelPos, float SceneDepth)
{
    uint zSlice = min(VF_ComputeLightGridZSlice(SceneDepth), CulledGridSizeZ - 1u);
    uint2 tile = PixelPos >> LightGridPixelSizeShift;
    // froxel グリッドは画面より広い場合がある (ceil) ので範囲へクランプ
    tile = min(tile, uint2(CulledGridSizeX - 1u, CulledGridSizeY - 1u));
    return (zSlice * CulledGridSizeY + tile.y) * CulledGridSizeX + tile.x;
}

// -------------------------------------------------------------
//  ローカルライト 1 灯の froxel でのインスキャッタ
//  (LightScatteringCS の評価)
//    GetLocalLightAttenuation : 半径窓 / 指数フォールオフ / コーン / レクト背面
//    IntegrateLight           : 面光源の形状を考慮したフォールオフ
//                               (逆二乗の特異点は froxel サイズ由来の
//                                距離バイアスで避ける)
//    WorldPos     : froxel サンプル位置
//    CameraVector : カメラ -> froxel (正規化)
//    CellRadius   : froxel の大きさ [m]
//    Shadow       : ライトと同インデックスのシャドウパラメータ
//                   (bCastVolumetricShadow のライトだけ参照する)
// -------------------------------------------------------------
float3 ComputeLocalLightVolumetricScattering(
    FDeferredLightData LightData, FLocalShadowParameters Shadow,
    float3 WorldPos, float3 CameraVector, float CellRadius, float PhaseG)
{
    float3 ToLight = float3(0.0f, 0.0f, 0.0f);
    float3 L = float3(0.0f, 0.0f, 0.0f);
    float LightMask = GetLocalLightAttenuation(WorldPos, LightData, ToLight, L);

    [branch]
    if (LightMask <= 0.0f)
    {
        return float3(0.0f, 0.0f, 0.0f);
    }

    // ---- 面光源の形状を考慮したフォールオフ ----
    // DistanceBiasSqr = Pow2(max(CellRadius * InverseSquaredLightDistanceBiasScale, SourceRadius))
    float Attenuation;

    [branch]
    if (LightData.bRectLight)
    {
        FRect Rect = GetRect(ToLight, LightData);
        Attenuation = IntegrateLight(Rect);
    }
    else
    {
        float DistanceBias = max(CellRadius * TemporalParams.z, LightData.SourceRadius);
        FCapsuleLight Capsule = GetCapsule(ToLight, LightData);
        Capsule.DistBiasSqr = DistanceBias * DistanceBias;
        Attenuation = IntegrateLight(Capsule, LightData.bInverseSquared);
    }

    // ---- シャドウマップ (bCastVolumetricShadow のライトだけ。DF シャドウ指定のライトは影なし) ----
    float ShadowFactor = 1.0f;

    [branch]
    if (LightData.bCastVolumetricShadow && Shadow.ShadowSliceIndex >= 0 && Shadow.DFShadow < 0.5f)
    {
        ShadowFactor = ProjectLocalLightShadowMap(LocalLightShadows, ShadowCmpSampler, LightData, Shadow, WorldPos);
    }

    // ---- ライト色 (オーバーライド時はフォグのインスキャッタ色 x 輝度) ----
    float3 LightColor = LightData.Color;

    [flatten]
    if (FogInscatteringColor.w > 0.5f)
    {
        float LightLuminance = Luminance(LightData.Color);
        LightColor = FogInscatteringColor.rgb * LightLuminance;
    }

    float Phase = HenyeyGreensteinPhase(PhaseG, dot(L, CameraVector));

    return LightColor * (LightMask * Attenuation * ShadowFactor * Phase * LightData.VolumetricScatteringIntensity);
}

// -------------------------------------------------------------
//  ディレクショナルライトの CSM 遮蔽 (法線オフセットなし / DF なし)
//    ViewDepth : froxel のビュー Z (カスケード選択 + 距離フェード)
// -------------------------------------------------------------
float ComputeDirectionalLightVolumetricShadow(float3 WorldPos, float ViewDepth)
{
    const uint numCascades = (uint) DirectionalShadowParams.x;
    if (numCascades == 0)
    {
        return 1.0f;
    }

    const float shadowDistance = DirectionalShadowParams.y;
    const float fadeStart = DirectionalShadowParams.z;

    if (ViewDepth >= shadowDistance)
    {
        return 1.0f;
    }

    uint cascade = SelectShadowCascade(ViewDepth, CascadeSplits);
    if (cascade >= numCascades)
    {
        return 1.0f;
    }

    float csm = ProjectDirectionalCascadeShadow(DirectionalShadowCascades, ShadowCmpSampler,
        WorldToShadowCascade[cascade], CascadeDepthBias[cascade],
        DirectionalShadowParams.w, cascade, WorldPos);

    float fade = saturate((shadowDistance - ViewDepth) / max(shadowDistance - fadeStart, 1e-4f));
    return lerp(1.0f, csm, fade);
}

#endif
