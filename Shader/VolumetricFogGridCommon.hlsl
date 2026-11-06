#ifndef VOLUMETRIC_FOG_GRID_COMMON_HLSL
#define VOLUMETRIC_FOG_GRID_COMMON_HLSL

// =============================================================
//  VolumetricFogGridCommon
//  Volumetric Fog の froxel グリッド共通部 (VolumetricFogCommon.hlsl から分離)。
//  b0 (FVolumetricFogParams) と froxel 座標系の変換だけを持ち、
//  コンピュート 3 パス (VolumetricFogCommon.hlsl 経由) と
//  ボリュームマテリアルのボクセル化 (VolumetricFogVoxelization*.hlsl,
//  グラフィックス) の両方が取り込む。レジスタは b0 のみ。
// =============================================================

// C++ 側 (VolumetricFog.h) の FVolumetricFogParams と 1:1 ミラー必須 (784 bytes)
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
    float4 TemporalParams; // x = テンポラル再投影 (0/1), y = 履歴有効 (0/1), z = 逆二乗距離バイアススケール, w = 経過時間 [s] (ボリュームマテリアルの Panner)

    // ---- Exponential Height Fog (FExponentialHeightFogSceneInfo) ----
    float4 FogDensityParams0; // x = Density0 [1/m], y = HeightFalloff0 [1/m], z = Height0 [m], w = ExtinctionScale
    float4 FogDensityParams1; // x = Density1 [1/m], y = HeightFalloff1 [1/m], z = Height1 [m], w = ScatteringDistribution (HG の g)
    float4 FogAlbedo; // rgb = VolumetricFogAlbedo, w = VolumetricFogStartDistance [m]
    float4 FogEmissive; // rgb = VolumetricFogEmissive [/m], w = VolumetricFogNearFadeInDistance [m]
    float4 FogInscatteringColor; // rgb = FogInscatteringLuminance (ライト色オーバーライド用), w = bOverrideLightColorsWithFogInscatteringColors
    float4 DirectionalInscatteringColor; // rgb = DirectionalInscatteringLuminance (同上), w = StaticLightingScatteringIntensity (スカイ項の強度)

    // ---- ライト ----
    // 選択されたフォワードディレクショナルライト (b3 と同じ 1 灯)。
    // bCastVolumetricShadow が偽なら C++ 側が DirectionalShadowParams.x (NumCascades) を 0 にする
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

    // ---- ボリュームマテリアルのボクセル化 (VolumetricFogVoxelization*.hlsl) ----
    float4x4 WorldToView; // ワールド -> ビュー (転置済み。境界の投影 / スライス範囲)
};

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

#endif
