#ifndef LIGHT_GRID_COMMON_HLSL
#define LIGHT_GRID_COMMON_HLSL

#include "ConstantBuffers.hlsl"
#include "Resources.hlsl"

// =============================================================
//  LightGridCommon
//  LightGridCommon.ush 相当。タイルドライトカリング
//  (クラスタードライトグリッド) の受光側ヘルパ。
//
//  グリッドは XY = 画面タイル (LightGridPixelSize = 64px)、
//  Z = ビュー深度の指数スライス (LightGridSizeZ = 32) の 3D セル。
//  LightGridInjection_CS がセルごとの可視ライトをリンクリストに集め、
//  LightGridCompact_CS が連続領域へ圧縮した結果を
//    NumCulledLightsGrid (t19) : セルごとの [ライト数, データ開始]
//    CulledLightDataGrid (t20) : ライトインデックス列
//  として読む。インデックスはライトバッファ (t13, ForwardLocalLights) /
//  ローカルシャドウパラメータ (t16, LocalShadowParams) と共通。
//
//  グリッドパラメータは b3 (ForwardLightData) を参照するため、
//  このファイルはレジスタを宣言する共有ヘッダに依存する
//  (レジスタフリーな純関数ファイルではない点に注意)。
// =============================================================

// NumCulledLightsGrid のセルあたり uint 数 ([0]=NumLights, [1]=DataStart)
#define NUM_CULLED_LIGHTS_GRID_STRIDE 2

// -------------------------------------------------------------
//  ビュー深度 -> Z スライス
//  LightGridZParams = (B, O, S) による指数分布:
//    Slice = log2(Depth * B + O) * S
//  (GetLightGridZParams / ComputeZSliceFromDepth 相当)
// -------------------------------------------------------------
uint ComputeZSliceFromDepth(float SceneDepth)
{
    return (uint) max(0.0f, log2(SceneDepth * LightGridZParams.x + LightGridZParams.y) * LightGridZParams.z);
}

// -------------------------------------------------------------
//  ピクセル座標 + ビュー深度 -> グリッドセル座標
//  (ComputeLightGridCellCoordinate 相当)
// -------------------------------------------------------------
uint3 ComputeLightGridCellCoordinate(uint2 PixelPos, float SceneDepth)
{
    uint ZSlice = min(ComputeZSliceFromDepth(SceneDepth), CulledGridSizeZ - 1u);
    return uint3(PixelPos >> LightGridPixelSizeShift, ZSlice);
}

// -------------------------------------------------------------
//  グリッドセル座標 -> セル線形インデックス
//  (ComputeLightGridCellIndex 相当)
// -------------------------------------------------------------
uint ComputeLightGridCellIndex(uint3 GridCoordinate)
{
    return (GridCoordinate.z * CulledGridSizeY + GridCoordinate.y) * CulledGridSizeX + GridCoordinate.x;
}

// -------------------------------------------------------------
//  セルのカリング済みライトヘッダ (FCulledLightsGridHeader 相当)
// -------------------------------------------------------------
struct FCulledLightsGridHeader
{
    uint NumLights; // このセルに影響するローカルライト数
    uint DataStartIndex; // CulledLightDataGrid 内の開始位置
};

FCulledLightsGridHeader GetCulledLightsGridHeader(uint GridIndex)
{
    FCulledLightsGridHeader Header;
    Header.NumLights = NumCulledLightsGrid[GridIndex * NUM_CULLED_LIGHTS_GRID_STRIDE + 0];
    Header.DataStartIndex = NumCulledLightsGrid[GridIndex * NUM_CULLED_LIGHTS_GRID_STRIDE + 1];

    // 安全クランプ (圧縮パスのセルあたり上限と同じ)
    Header.NumLights = min(Header.NumLights, MaxCulledLightsPerCell);
    return Header;
}

// CulledLightDataGrid からライトインデックスを取り出す
uint GetCulledLightDataGrid(uint GridElementIndex)
{
    return CulledLightDataGrid[GridElementIndex];
}

// -------------------------------------------------------------
//  デバッグ可視化 (b3 の LightGridDebugMode)
//    Mode 1 : ライト複雑度ヒートマップ (セルのライト数)
//    Mode 2 : Z スライス可視化
// -------------------------------------------------------------
float3 GetLightGridComplexityColor(uint NumLights)
{
    if (NumLights == 0u)
    {
        return float3(0.02f, 0.02f, 0.05f); // 空セル = ほぼ黒
    }

    // 緑 -> 黄 -> 赤 のランプ (MaxCulledLightsPerCell で飽和)
    float t = saturate((float) NumLights / (float) max(MaxCulledLightsPerCell, 1u));
    float3 Green = float3(0.0f, 1.0f, 0.0f);
    float3 Yellow = float3(1.0f, 1.0f, 0.0f);
    float3 Red = float3(1.0f, 0.0f, 0.0f);
    return (t < 0.5f) ? lerp(Green, Yellow, t * 2.0f) : lerp(Yellow, Red, t * 2.0f - 1.0f);
}

float3 GetLightGridZSliceColor(uint ZSlice)
{
    // スライスごとに色相を黄金比でずらした簡易カラーホイール
    float h = frac((float) ZSlice * 0.618034f);
    float3 c = saturate(float3(abs(h * 6.0f - 3.0f) - 1.0f,
                               2.0f - abs(h * 6.0f - 2.0f),
                               2.0f - abs(h * 6.0f - 4.0f)));
    return c;
}

#endif
