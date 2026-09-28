#ifndef VOLUMETRIC_FOG_COMMON_HLSL
#define VOLUMETRIC_FOG_COMMON_HLSL

// =============================================================
//  VolumetricFogCommon
//  VolumetricFogShared.ush / VolumetricFog.usf の共通部。
//  Volumetric Fog は視錐台を XY = GridPixelSize (8px) タイル、
//  Z = 指数分布 GridSizeZ (64) スライスの froxel ボリュームに切り、
//  3 つのコンピュートパスで「カメラから各 froxel までの累積
//  インスキャッタ + 透過率」(IntegratedLightScattering, t34) を作る:
//
//    1. VolumetricFogAttributes_CS      : 指数高さフォグの密度 ->
//                                         散乱 / 吸収 (VBufferA) + 発光 (VBufferB)
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
//    t0 : ForwardLocalLights        t1 : LocalShadowParams
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
//  Structs.hlsl、シャドウ投影は ShadowProjectionCommon.hlsl
//  (いずれもレジスタ非依存) から取り込む。ライトグリッドの
//  セル計算は LightGridCommon.hlsl と同式 (b3 の値を b0 経由で受ける)。
//
//  ---- UE からの意図的乖離 ----
//    - Y-up / メートル単位 (高さ = WorldPosition.y、密度 [1/m])
//    - 位相関数は標準 HG: cosθ = dot(ToLight, CameraVector)
//      (UE は g を反転して dot(L, -CameraVector) を渡す。同値)
//    - スカイライトの SH の代わりに IBL irradiance キューブ (t10) を
//      視線方向で採光し /π した値を等方インスキャッタとする
//    - DF シャドウ (bUseRayTracedDistanceFieldShadows) のライトは
//      ボリューム内では影なし (シャドウマップのライトのみ遮蔽)
//    - HistoryMissSupersampleCount (履歴外セルの追加サンプル) は未実装
// =============================================================

#include "Structs.hlsl"
#include "ShadowProjectionCommon.hlsl"

#define VOLUMETRIC_FOG_THREADGROUP_SIZE 4   // 4x4x4 (UE VolumetricFogGridInjectionGroupSize)
#define VOLUMETRIC_FOG_INTEGRATION_GROUP_SIZE 8   // 8x8x1 (FinalIntegration)

static const float VF_PI = 3.14159265358979323846f;

// C++ 側 (VolumetricFog.h) の FVolumetricFogParams と 1:1 ミラー必須 (720 bytes)
cbuffer FVolumetricFogParams : register(b0)
{
    float4x4 ViewToWorld; // ビュー -> ワールド (転置済み, mul(v,M) 規約)
    float4x4 PrevWorldToClip; // 前フレームの View x Projection (転置済み。履歴再投影用)
    float4x4 WorldToShadowCascade[4]; // CSM: ワールド -> シャドウクリップ (転置済み)

    float4 CascadeSplits; // 各カスケードのビュー深度遠端
    float4 CascadeDepthBias; // 受光側深度バイアス (NDC)
    float4 DirectionalShadowParams; // x=NumCascades, y=ShadowDistance, z=FadeStart, w=1/解像度

    float4 GridSize; // xyz = froxel 数 (X, Y, Z), w = GridPixelSize [px]
    float4 GridZParams; // xyz = (B, O, S): Slice = log2(ViewZ * B + O) * S, w = 1/GridSizeZ
    float4 ScreenSize; // xy = バックバッファ [px], zw = 1 / xy
    float4 ProjectionParams; // x = 1/Projection._11, y = 1/Projection._22, z = NearPlane [m], w = MaxDistance [m]
    float4 CameraOrigin; // xyz = カメラワールド位置 [m], w = 未使用
    float4 FrameJitter; // xyz = セル内サンプルオフセット (0..1), w = HistoryWeight
    float4 TemporalParams; // x = テンポラル再投影 (0/1), y = 履歴有効 (0/1), z = 逆二乗距離バイアススケール, w = 未使用

    // ---- Exponential Height Fog (FExponentialHeightFogSceneInfo) ----
    float4 FogDensityParams0; // x = Density0 [1/m], y = HeightFalloff0 [1/m], z = Height0 [m], w = ExtinctionScale
    float4 FogDensityParams1; // x = Density1 [1/m], y = HeightFalloff1 [1/m], z = Height1 [m], w = ScatteringDistribution (HG の g)
    float4 FogAlbedo; // rgb = VolumetricFogAlbedo, w = VolumetricFogStartDistance [m]
    float4 FogEmissive; // rgb = VolumetricFogEmissive [/m], w = VolumetricFogNearFadeInDistance [m]
    float4 FogInscatteringColor; // rgb = FogInscatteringLuminance (ライト色オーバーライド用), w = bOverrideLightColorsWithFogInscatteringColors
    float4 DirectionalInscatteringColor; // rgb = DirectionalInscatteringLuminance (同上), w = StaticLightingScatteringIntensity (スカイ項の強度)

    // ---- ライト ----
    float4 DirectionalLightDirection; // xyz = 受光点 -> ライト方向 (正規化), w = 有効 (0/1)
    float4 DirectionalLightColor; // rgb = 線形色 x lux x VolumetricScatteringIntensity, w = 未使用

    // ---- ライトグリッド (b3 ForwardLightData と同値) ----
    uint NumLocalLights;
    uint CulledGridSizeX;
    uint CulledGridSizeY;
    uint CulledGridSizeZ;

    uint LightGridPixelSizeShift;
    uint MaxCulledLightsPerCell;
    uint bUseLightGrid;
    uint VolumetricFogPadA;

    float3 LightGridZParams; // (B, O, S)
    uint VolumetricFogPadB;
};

StructuredBuffer<FLightShaderParameters> ForwardLocalLights : register(t0);
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
//  froxel Z スライス <-> ビュー深度 (VolumetricFogShared.ush)
//    Slice = log2(Depth * B + O) * S  /  Depth = (2^(Slice / S) - O) / B
// -------------------------------------------------------------
float ComputeDepthFromZSlice(float ZSlice)
{
    return (exp2(ZSlice / GridZParams.z) - GridZParams.y) / GridZParams.x;
}

float ComputeZSliceFromDepth(float SceneDepth)
{
    return log2(SceneDepth * GridZParams.x + GridZParams.y) * GridZParams.z;
}

// -------------------------------------------------------------
//  froxel 座標 -> ワールド座標 (ComputeCellWorldPosition)
//    CellOffset : セル内オフセット (0..1)。0.5 でセル中心、
//                 テンポラルジッタ時は FrameJitter.xyz
//    SceneDepth : そのセルのビュー Z [m]
//  XY はスクリーンピクセル (GridCoordinate * GridPixelSize) を
//  NDC へ写し、ビュー空間で深度に沿って引き伸ばす。
// -------------------------------------------------------------
float3 ComputeCellWorldPosition(uint3 GridCoordinate, float3 CellOffset, out float SceneDepth)
{
    float2 pixelPos = (float2(GridCoordinate.xy) + CellOffset.xy) * GridSize.w;
    float2 uv = pixelPos * ScreenSize.zw;
    float2 ndc = float2(uv.x * 2.0f - 1.0f, (1.0f - uv.y) * 2.0f - 1.0f);

    SceneDepth = ComputeDepthFromZSlice((float) GridCoordinate.z + CellOffset.z);

    float3 viewPos = float3(
        ndc.x * ProjectionParams.x * SceneDepth,
        ndc.y * ProjectionParams.y * SceneDepth,
        SceneDepth);

    return mul(float4(viewPos, 1.0f), ViewToWorld).xyz;
}

// -------------------------------------------------------------
//  ワールド座標 -> ボリューム UV (ComputeVolumeUV)
//  履歴再投影用 (前フレームの WorldToClip を渡す)。
//  戻り値 w > 0 のとき有効 (カメラ前方)。
// -------------------------------------------------------------
float4 ComputeVolumeUVFromWorld(float3 WorldPos, float4x4 WorldToClip)
{
    float4 clip = mul(float4(WorldPos, 1.0f), WorldToClip);
    if (clip.w <= 1e-4f)
    {
        return float4(0.0f, 0.0f, 0.0f, 0.0f);
    }

    float2 ndc = clip.xy / clip.w;
    float2 screenUV = float2(ndc.x * 0.5f + 0.5f, 0.5f - ndc.y * 0.5f);

    // スクリーン UV -> グリッド UV (グリッドは GridSize.xy * PixelSize [px] を覆う)
    float2 gridUV = screenUV * ScreenSize.xy / (GridSize.xy * GridSize.w);
    float sliceUV = ComputeZSliceFromDepth(clip.w) * GridZParams.w;

    return float4(gridUV, sliceUV, 1.0f);
}

// -------------------------------------------------------------
//  Henyey-Greenstein 位相関数 (標準形)
//    CosTheta = dot(受光点 -> ライト, カメラ -> 受光点)
//    g > 0 で前方散乱 (ライトを向いたときに明るい)
// -------------------------------------------------------------
float HenyeyGreensteinPhase(float g, float CosTheta)
{
    float g2 = g * g;
    float denom = max(1.0f + g2 - 2.0f * g * CosTheta, 1e-4f);
    return (1.0f - g2) / (4.0f * VF_PI * pow(denom, 1.5f));
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
//  (VolumetricFog.usf の GetLocalLightAttenuation + 位相関数)
//    WorldPos     : froxel サンプル位置
//    CameraVector : カメラ -> froxel (正規化)
//    CellRadius   : froxel の大きさ [m] (逆二乗の特異点回避バイアス)
//    Shadow       : ライトと同インデックスのシャドウパラメータ
// -------------------------------------------------------------
float3 ComputeLocalLightVolumetricScattering(
    FLightShaderParameters Light, FLocalShadowParameters Shadow,
    float3 WorldPos, float3 CameraVector, float CellRadius, float PhaseG)
{
    float3 toLight = Light.Position - WorldPos;
    float distSqr = dot(toLight, toLight);
    float3 L = toLight * rsqrt(max(distSqr, 1e-8f));

    // ---- 減衰マスク (DeferredLightingCommon.hlsl の SetupAreaLight と同式) ----
    float lightMask;
    if (Light.Flags & LIGHT_FLAG_INVERSE_SQUARED)
    {
        float t = saturate(1.0f - (distSqr * Light.InvRadius * Light.InvRadius) * (distSqr * Light.InvRadius * Light.InvRadius));
        lightMask = t * t;
    }
    else
    {
        lightMask = pow(1.0f - saturate(distSqr * Light.InvRadius * Light.InvRadius), Light.FalloffExponent);
    }

    if (Light.Type == LIGHT_TYPE_SPOT)
    {
        float cone = saturate((dot(-L, Light.Direction) - Light.SpotAngles.x) * Light.SpotAngles.y);
        lightMask *= cone * cone;
    }
    else if (Light.Type == LIGHT_TYPE_RECT)
    {
        // 発光面の裏側は照らさない + ランバート発光
        lightMask *= saturate(dot(Light.Direction, -L));
    }

    [branch]
    if (lightMask <= 0.0f)
    {
        return float3(0.0f, 0.0f, 0.0f);
    }

    // ---- 距離フォールオフ (逆二乗 + セルサイズ由来のバイアスで発火防止) ----
    float falloff = 1.0f;
    if (Light.Flags & LIGHT_FLAG_INVERSE_SQUARED)
    {
        float distanceBias = max(CellRadius * TemporalParams.z, Light.SourceRadius);
        falloff = rcp(distSqr + max(distanceBias * distanceBias, 1e-4f));
    }

    // ---- シャドウマップ (DF シャドウ指定のライトは影なし) ----
    float shadow = 1.0f;
    [branch]
    if (Shadow.ShadowSliceIndex >= 0 && Shadow.DFShadow < 0.5f)
    {
        shadow = ProjectLocalLightShadowMap(LocalLightShadows, ShadowCmpSampler, Light, Shadow, WorldPos);
    }

    // ---- ライト色 (オーバーライド時はフォグのインスキャッタ色 x 輝度) ----
    float3 lightColor = Light.Color;
    [flatten]
    if (FogInscatteringColor.w > 0.5f)
    {
        float luminance = dot(Light.Color, float3(0.2126f, 0.7152f, 0.0722f));
        lightColor = FogInscatteringColor.rgb * luminance;
    }

    float phase = HenyeyGreensteinPhase(PhaseG, dot(L, CameraVector));

    return lightColor * (lightMask * falloff * shadow * phase * Light.VolumetricScatteringIntensity);
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
