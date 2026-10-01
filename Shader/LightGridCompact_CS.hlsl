// ============================================================
//  LightGridCompact_CS.hlsl
//
//  タイルドライトカリング - Pass 2/2 (圧縮)。
//  LightGridInjection.usf の FLightGridCompactCS に相当する。
//
//  Pass 1 (LightGridInjection_CS) が構築したセルごとの
//  リンクリストを巡回して数え、グローバルアロケータ (byte 4)
//  から連続領域を確保して
//    RWNumCulledLightsGrid[cell*2+0] = ライト数
//    RWNumCulledLightsGrid[cell*2+1] = データ開始位置
//    RWCulledLightDataGrid[start..]  = ライトインデックス列
//  へ確定する。リンクリストは LIFO (ライトインデックス降順) なので
//  逆順に書き込み、昇順 (= ライトバッファ t13 の順) を復元する。
//
//  Root signature は Pass 1 と共通 (LightGridInjection_CS.hlsl 参照)。
//  本パスもグラフィックス用の共有ヘッダは include せず、
//  b0/u0-u4 は LightGridInjectionCommon.hlsl を Pass 1 と共有する。
// ============================================================

#include "LightGridInjectionCommon.hlsl"

[numthreads(THREADGROUP_SIZE, THREADGROUP_SIZE, THREADGROUP_SIZE)]
void main(uint3 GridCoordinate : SV_DispatchThreadID)
{
    if (GridCoordinate.x >= CulledGridSizeX ||
        GridCoordinate.y >= CulledGridSizeY ||
        GridCoordinate.z >= CulledGridSizeZ)
    {
        return;
    }

    uint GridIndex = (GridCoordinate.z * CulledGridSizeY + GridCoordinate.y) * CulledGridSizeX + GridCoordinate.x;

    // ------------------------------------------------------------
    //  リンクリストを数える (各ライトは 1 セルに最大 1 回なので
    //  NumLocalLights が巡回の安全上限になる)
    // ------------------------------------------------------------
    uint NumCulledLights = 0;
    uint LinkOffset = RWStartOffsetGrid[GridIndex];

    while (LinkOffset != 0xFFFFFFFFu && NumCulledLights < NumLocalLights)
    {
        ++NumCulledLights;
        LinkOffset = RWCulledLightLinks[LinkOffset].y;
    }

    // セルあたり上限で打ち切り (r.Forward.MaxCulledLightsPerCell 相当)
    uint NumToStore = min(NumCulledLights, MaxCulledLightsPerCell);

    // ------------------------------------------------------------
    //  連続領域を確保 (byte 4 = NextCulledLightData)
    // ------------------------------------------------------------
    uint DataStart = 0;
    if (NumToStore > 0)
    {
        RWLightGridAllocator.InterlockedAdd(4, NumToStore, DataStart);

        // 容量打ち切り (容量 = NumCells * MaxCulledLightsPerCell なので通常は発生しない)
        if (DataStart >= CulledLightDataCapacity)
        {
            NumToStore = 0;
            DataStart = 0;
        }
        else
        {
            NumToStore = min(NumToStore, CulledLightDataCapacity - DataStart);
        }
    }

    RWNumCulledLightsGrid[GridIndex * 2u + 0u] = NumToStore;
    RWNumCulledLightsGrid[GridIndex * 2u + 1u] = DataStart;

    // ------------------------------------------------------------
    //  逆順書き込みでライトインデックス昇順を復元
    // ------------------------------------------------------------
    LinkOffset = RWStartOffsetGrid[GridIndex];
    for (uint i = 0; i < NumToStore; ++i)
    {
        RWCulledLightDataGrid[DataStart + (NumToStore - 1u - i)] = RWCulledLightLinks[LinkOffset].x;
        LinkOffset = RWCulledLightLinks[LinkOffset].y;
    }
}
