#ifndef LIGHT_GRID_INJECTION_COMMON_HLSL
#define LIGHT_GRID_INJECTION_COMMON_HLSL

// =============================================================
//  LightGridInjectionCommon
//  タイルドライトカリングの構築側 2 パス
//  (LightGridInjection_CS = Pass 1 / LightGridCompact_CS = Pass 2)
//  が共有する定数バッファ / UAV の宣言。
//
//  レジスタ (b0 / u0-u4) はグラフィックス RS から独立した
//  コンピュート RS のパスローカルなもの。
//
//  ConstantBuffers.hlsl / LightGridCommon.hlsl とは併用しないこと。
//  b3 (ForwardLightData) のフィールド (CulledGridSizeX/Y/Z,
//  NumLocalLights, LightGridZParams など) と名前が衝突する。
// =============================================================

#define THREADGROUP_SIZE 4 // LightGridInjectionGroupSize (4x4x4)

// C++ 側 (LightGridInjection.h) の FLightGridParams と 1:1 ミラー必須
cbuffer FLightGridParams : register(b0)
{
    float4x4 ViewMatrix; // ワールド -> ビュー (転置済み, mul(v,M) 規約)

    uint CulledGridSizeX; // 画面タイル数 X (= ceil(W / LightGridPixelSize))
    uint CulledGridSizeY; // 画面タイル数 Y
    uint CulledGridSizeZ; // Z スライス数 (LIGHT_GRID_SIZE_Z)
    uint NumLocalLights; // 有効ローカルライト数

    float3 LightGridZParams; // (B, O, S): Slice = log2(Depth*B + O) * S
    uint LightGridPixelSize; // 64

    float InvProjScaleX; // 1 / Projection._11 (NDC.x -> view.x @ z=1)
    float InvProjScaleY; // 1 / Projection._22 (NDC.y -> view.y @ z=1)
    float ScreenWidth; // バックバッファ幅 [px]
    float ScreenHeight; // バックバッファ高 [px]

    uint MaxCulledLightsPerCell; // セルあたり保持上限 (32)
    uint MaxCulledLightLinks; // リンクバッファ容量 (NumCells * MaxCulledLightsPerCell)
    uint CulledLightDataCapacity; // データグリッド容量 (NumCells * MaxCulledLightsPerCell)
    float NearPlane; // カメラ近クリップ [m]
};

RWStructuredBuffer<uint> RWStartOffsetGrid : register(u0); // セル -> 先頭リンク (0xFFFFFFFF = 空)
RWStructuredBuffer<uint2> RWCulledLightLinks : register(u1); // (x=LightIndex, y=NextLink)
RWByteAddressBuffer RWLightGridAllocator : register(u2); // [byte0]=NextLink, [byte4]=NextData
RWStructuredBuffer<uint> RWNumCulledLightsGrid : register(u3); // セルごとの [ライト数, データ開始]
RWStructuredBuffer<uint> RWCulledLightDataGrid : register(u4); // カリング済みライトインデックス列

#endif
