// ============================================================
//  LightGridInjection_CS.hlsl
//
//  タイルドライトカリング (クラスタードライトグリッド) - Pass 1/2。
//  LightGridInjection.usf の TLightGridInjectionCS
//  (bLightGridUsesLinkedList = true) に相当する。
//
//  画面を XY = 64px タイル、Z = ビュー深度の指数スライスに分割した
//  3D セル (froxel) ごとに 1 スレッドを割り当て、全ローカルライトを
//  ビュー空間 AABB と交差判定して、通過したライトをグローバル
//  リンクリストへ積む:
//    RWStartOffsetGrid[cell] = 先頭リンク (0xFFFFFFFF = 空)
//    RWCulledLightLinks[i]   = (LightIndex, NextLink)
//  リンクの確保は RWByteAddressBuffer の InterlockedAdd (byte 0)。
//  ※ 各セルは担当スレッドのみが書くため StartOffsetGrid の事前
//    クリアは不要。アロケータ (8 bytes) のみ毎フレームゼロクリア。
//
//  交差判定 (LightGridInjection.usf と同型):
//    - 全ライト : 減衰球 vs セル AABB
//    - スポット : + SphereIntersectCone (セル外接球 vs コーン)
//    - レクト   : + 発光面背面の半空間カリング
//
//  Root signature (compute, グラフィックス RS から独立):
//    b0 : FLightGridParams      (cbuffer)
//    t0 : ForwardLocalLights    (StructuredBuffer<FLightShaderParameters>)
//    u0 : RWStartOffsetGrid     (RWStructuredBuffer<uint>)
//    u1 : RWCulledLightLinks    (RWStructuredBuffer<uint2>)
//    u2 : RWLightGridAllocator  (RWByteAddressBuffer)
//    u3 : RWNumCulledLightsGrid (RWStructuredBuffer<uint>)  ※ Pass 2 用
//    u4 : RWCulledLightDataGrid (RWStructuredBuffer<uint>)  ※ Pass 2 用
//
//  このファイルは b0 を独自所有するため、レジスタを宣言する
//  共有ヘッダ (ConstantBuffers/Resources) は include しない。
//  FLightShaderParameters / LIGHT_TYPE_* はレジスタ非依存の
//  Structs.hlsl から取り込む。
// ============================================================

#include "Structs.hlsl"

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

StructuredBuffer<FLightShaderParameters> ForwardLocalLights : register(t0);

RWStructuredBuffer<uint> RWStartOffsetGrid : register(u0); // セル -> 先頭リンク (0xFFFFFFFF = 空)
RWStructuredBuffer<uint2> RWCulledLightLinks : register(u1); // (x=LightIndex, y=NextLink)
RWByteAddressBuffer RWLightGridAllocator : register(u2); // [byte0]=NextLink, [byte4]=NextData
RWStructuredBuffer<uint> RWNumCulledLightsGrid : register(u3); // セルごとの [ライト数, データ開始]
RWStructuredBuffer<uint> RWCulledLightDataGrid : register(u4); // カリング済みライトインデックス列

// -------------------------------------------------------------
//  Z スライス s の開始ビュー深度
//  (ComputeCellNearViewDepthFromZSlice 相当)
//    Slice = log2(Depth * B + O) * S の逆関数
// -------------------------------------------------------------
float ComputeCellNearViewDepthFromZSlice(uint ZSlice)
{
    return (exp2((float) ZSlice / LightGridZParams.z) - LightGridZParams.y) / LightGridZParams.x;
}

// -------------------------------------------------------------
//  球 vs コーン (SphereIntersectCone 相当)
//    球 (中心 xyz + 半径 w) が、頂点 ConeVertex / 軸 ConeAxis /
//    半頂角 (cos, sin) の無限コーンと交差するか
// -------------------------------------------------------------
bool SphereIntersectCone(float4 SphereCenterAndRadius, float3 ConeVertex, float3 ConeAxis, float ConeAngleCos, float ConeAngleSin)
{
    float3 U = ConeVertex - (SphereCenterAndRadius.w / ConeAngleSin) * ConeAxis;
    float3 D = SphereCenterAndRadius.xyz - U;
    float DSizeSq = dot(D, D);
    float E = dot(ConeAxis, D);

    if (E > 0.0f && E * E >= DSizeSq * ConeAngleCos * ConeAngleCos)
    {
        D = SphereCenterAndRadius.xyz - ConeVertex;
        DSizeSq = dot(D, D);
        E = -dot(ConeAxis, D);

        if (E > 0.0f && E * E >= DSizeSq * ConeAngleSin * ConeAngleSin)
        {
            return DSizeSq <= SphereCenterAndRadius.w * SphereCenterAndRadius.w;
        }
        return true;
    }
    return false;
}

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
    //  セル (froxel) のビュー空間 AABB
    // ------------------------------------------------------------
    // 画面タイルのピクセル範囲 (画面端は端で打ち切る)
    float2 TileMinPx = float2(GridCoordinate.xy) * (float) LightGridPixelSize;
    float2 TileMaxPx = min(TileMinPx + (float) LightGridPixelSize, float2(ScreenWidth, ScreenHeight));

    // NDC 範囲 (y はピクセル座標と上下反転)
    float NdcMinX = TileMinPx.x / ScreenWidth * 2.0f - 1.0f;
    float NdcMaxX = TileMaxPx.x / ScreenWidth * 2.0f - 1.0f;
    float NdcMaxY = 1.0f - TileMinPx.y / ScreenHeight * 2.0f;
    float NdcMinY = 1.0f - TileMaxPx.y / ScreenHeight * 2.0f;

    // z=1 におけるビュー空間レイ範囲 (対称透視: view = ndc / Proj対角)
    float RayMinX = NdcMinX * InvProjScaleX;
    float RayMaxX = NdcMaxX * InvProjScaleX;
    float RayMinY = NdcMinY * InvProjScaleY;
    float RayMaxY = NdcMaxY * InvProjScaleY;

    // スライスの深度範囲。指数分布はスライス 0 が Near より少し奥
    // (NearOffset ぶん) から始まるため、取りこぼし防止で Near まで広げる
    float MinTileZ = (GridCoordinate.z == 0u)
        ? NearPlane
        : ComputeCellNearViewDepthFromZSlice(GridCoordinate.z);
    float MaxTileZ = ComputeCellNearViewDepthFromZSlice(GridCoordinate.z + 1u);
    MaxTileZ = max(MaxTileZ, MinTileZ + 1e-4f);

    // レイは z に比例して広がるので、min/max 深度の両端で AABB を張る
    float3 ViewMin;
    float3 ViewMax;
    ViewMin.x = min(RayMinX * MinTileZ, RayMinX * MaxTileZ);
    ViewMax.x = max(RayMaxX * MinTileZ, RayMaxX * MaxTileZ);
    ViewMin.y = min(RayMinY * MinTileZ, RayMinY * MaxTileZ);
    ViewMax.y = max(RayMaxY * MinTileZ, RayMaxY * MaxTileZ);
    ViewMin.z = MinTileZ;
    ViewMax.z = MaxTileZ;

    // コーン / 半空間テスト用の外接球
    float3 CellCenter = (ViewMin + ViewMax) * 0.5f;
    float CellRadius = length(ViewMax - CellCenter);

    // ------------------------------------------------------------
    //  全ローカルライトを巡回してリンクリストへ積む
    // ------------------------------------------------------------
    uint StartLinkOffset = 0xFFFFFFFFu;

    for (uint LightIndex = 0; LightIndex < NumLocalLights; ++LightIndex)
    {
        FLightShaderParameters Light = ForwardLocalLights[LightIndex];

        float Radius = (Light.InvRadius > 0.0f) ? rcp(Light.InvRadius) : 1e10f;
        float3 ViewPosition = mul(float4(Light.Position, 1.0f), ViewMatrix).xyz;

        // ---- 減衰球 vs セル AABB ----
        float3 ClosestPoint = clamp(ViewPosition, ViewMin, ViewMax);
        float3 ToCenter = ViewPosition - ClosestPoint;
        bool bIntersect = dot(ToCenter, ToCenter) <= Radius * Radius;

        // ---- スポット: コーン vs セル外接球で絞り込む ----
        if (bIntersect && Light.Type == LIGHT_TYPE_SPOT && Light.SpotAngles.x > -1.0f)
        {
            float CosOuterCone = Light.SpotAngles.x;
            float SinOuterCone = sqrt(saturate(1.0f - CosOuterCone * CosOuterCone));

            [branch]
            if (SinOuterCone > 1e-4f)
            {
                float3 ViewDirection = normalize(mul(float4(Light.Direction, 0.0f), ViewMatrix).xyz);
                bIntersect = SphereIntersectCone(
                    float4(CellCenter, CellRadius),
                    ViewPosition, ViewDirection,
                    CosOuterCone, SinOuterCone);
            }
        }

        // ---- レクト: 発光面の背面半空間を捨てる ----
        // (受光側は dot(Direction, -L) <= 0 で無光になるため、
        //  セル外接球が丸ごと背面ならスキップできる)
        if (bIntersect && Light.Type == LIGHT_TYPE_RECT)
        {
            float3 ViewDirection = normalize(mul(float4(Light.Direction, 0.0f), ViewMatrix).xyz);
            float PlaneDistance = dot(CellCenter - ViewPosition, ViewDirection);
            bIntersect = (PlaneDistance > -CellRadius);
        }

        if (bIntersect)
        {
            // グローバルリンクを確保して先頭に繋ぐ
            // (RWByteAddressBuffer の InterlockedAdd は out 引数必須)
            uint LinkOffset;
            RWLightGridAllocator.InterlockedAdd(0, 1u, LinkOffset);

            if (LinkOffset < MaxCulledLightLinks)
            {
                RWCulledLightLinks[LinkOffset] = uint2(LightIndex, StartLinkOffset);
                StartLinkOffset = LinkOffset;
            }
        }
    }

    RWStartOffsetGrid[GridIndex] = StartLinkOffset;
}
