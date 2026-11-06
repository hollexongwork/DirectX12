#include "VolumetricFogVoxelizationCommon.hlsl"

// =============================================================
//  VolumetricFogVoxelizationGS
//  VoxelizeVolumePrimitiveGS 相当。プリミティブの境界 (OBB の 8 頂点、
//  球なら中心 ± 半径の AABB) をビュー空間へ写し、
//    - ビュー深度の範囲 -> froxel の Z スライス範囲 [SliceMin, SliceMax]
//    - 透視投影した XY の範囲 -> froxel グリッドのクリップ矩形
//  を求め、このパスが担当するスライス (PassIndex x 8 .. +7) のうち
//  範囲内のものへスライスごとに 1 クアッド (トライアングルストリップ 4 頂点)
//  を SV_RenderTargetArrayIndex = スライス で出力する。
//  境界の内外判定は PS がサンプル位置で行う (矩形は保守的でよい)。
//
//  ニアより手前に境界が食い込む場合、投影は発散するので矩形を
//  画面全体にする (カメラが媒質の中に居る)。
// =============================================================

[maxvertexcount(4 * MAX_SLICES_PER_VOXELIZATION_PASS)]
void main(point FVoxelizeVolumeVSToGS Input[1], inout TriangleStream<FVoxelizeVolumeGSToPS> OutStream)
{
    const float nearPlane = ProjectionParams.z;
    const uint shape = (uint) (VolumeMaterial.ExtinctionParams.w + 0.5f);

    // ---- 境界の 8 頂点をビュー空間へ ----
    float3 viewCorners[8];

    [branch]
    if (shape == VOLUME_SHAPE_SPHERE)
    {
        float3 centerView = mul(float4(BoundsSphere.xyz, 1.0f), WorldToView).xyz;
        float r = BoundsSphere.w;
        [unroll]
        for (uint i = 0; i < 8; ++i)
        {
            float3 s = float3((i & 1u) ? 1.0f : -1.0f, (i & 2u) ? 1.0f : -1.0f, (i & 4u) ? 1.0f : -1.0f);
            viewCorners[i] = centerView + s * r;
        }
    }
    else
    {
        [unroll]
        for (uint i = 0; i < 8; ++i)
        {
            float3 unitCorner = float3((i & 1u) ? 1.0f : -1.0f, (i & 2u) ? 1.0f : -1.0f, (i & 4u) ? 1.0f : -1.0f);
            float3 worldCorner = mul(float4(unitCorner, 1.0f), BoxToWorld).xyz;
            viewCorners[i] = mul(float4(worldCorner, 1.0f), WorldToView).xyz;
        }
    }

    // ---- ビュー深度の範囲 ----
    float minZ = viewCorners[0].z;
    float maxZ = viewCorners[0].z;
    [unroll]
    for (uint i = 1; i < 8; ++i)
    {
        minZ = min(minZ, viewCorners[i].z);
        maxZ = max(maxZ, viewCorners[i].z);
    }

    // 全体がカメラの後ろ、または Volumetric Fog の最大距離より遠い
    if (maxZ <= nearPlane || minZ >= ProjectionParams.w)
    {
        return;
    }

    // ---- Z スライス範囲 ----
    const uint gridSizeZ = (uint) GridSize.z;
    const float sliceMinF = ComputeZSliceFromDepth(max(minZ, nearPlane));
    const float sliceMaxF = ComputeZSliceFromDepth(maxZ);
    const int sliceMin = max((int) floor(sliceMinF), 0);
    const int sliceMax = min((int) floor(sliceMaxF), (int) gridSizeZ - 1);
    if (sliceMin > sliceMax)
    {
        return;
    }

    // このパスの担当スライス範囲と交差しなければ何も出さない
    const int passFirst = (int) (Input[0].PassIndex * MAX_SLICES_PER_VOXELIZATION_PASS);
    const int passLast = passFirst + MAX_SLICES_PER_VOXELIZATION_PASS - 1;
    if (passLast < sliceMin || passFirst > sliceMax)
    {
        return;
    }

    // ---- 投影した XY の範囲 -> froxel グリッド UV ----
    float2 gridUVMin;
    float2 gridUVMax;

    [branch]
    if (minZ <= nearPlane)
    {
        // ニアに食い込む: 保守的に画面全体
        gridUVMin = float2(0.0f, 0.0f);
        gridUVMax = float2(1.0f, 1.0f);
    }
    else
    {
        gridUVMin = float2(1e9f, 1e9f);
        gridUVMax = float2(-1e9f, -1e9f);

        // スクリーン UV -> グリッド UV の比 (グリッドは画面より広い場合がある)
        const float2 screenToGrid = ScreenSize.xy / (GridSize.xy * GridSize.w);

        [unroll]
        for (uint i = 0; i < 8; ++i)
        {
            float3 v = viewCorners[i];
            // ComputeCellWorldPosition の逆: ndc = view.xy / (view.z * (1/P11, 1/P22))
            float2 ndc = v.xy / (v.z * ProjectionParams.xy);
            float2 screenUV = float2(ndc.x * 0.5f + 0.5f, 0.5f - ndc.y * 0.5f);
            float2 gridUV = screenUV * screenToGrid;
            gridUVMin = min(gridUVMin, gridUV);
            gridUVMax = max(gridUVMax, gridUV);
        }

        gridUVMin = saturate(gridUVMin);
        gridUVMax = saturate(gridUVMax);
        if (any(gridUVMin >= gridUVMax))
        {
            return;	// 画面外
        }
    }

    // グリッド UV -> クリップ座標 (ビューポート = GridSize.xy)
    const float2 clipMin = float2(gridUVMin.x * 2.0f - 1.0f, 1.0f - gridUVMax.y * 2.0f);
    const float2 clipMax = float2(gridUVMax.x * 2.0f - 1.0f, 1.0f - gridUVMin.y * 2.0f);

    // ---- スライスごとに 1 クアッド ----
    [unroll]
    for (int s = 0; s < MAX_SLICES_PER_VOXELIZATION_PASS; ++s)
    {
        const int slice = passFirst + s;
        if (slice < sliceMin || slice > sliceMax)
        {
            continue;
        }

        FVoxelizeVolumeGSToPS v;
        v.SliceIndex = (uint) slice;

        v.Position = float4(clipMin.x, clipMax.y, 0.5f, 1.0f);
        OutStream.Append(v);
        v.Position = float4(clipMax.x, clipMax.y, 0.5f, 1.0f);
        OutStream.Append(v);
        v.Position = float4(clipMin.x, clipMin.y, 0.5f, 1.0f);
        OutStream.Append(v);
        v.Position = float4(clipMax.x, clipMin.y, 0.5f, 1.0f);
        OutStream.Append(v);
        OutStream.RestartStrip();
    }
}
