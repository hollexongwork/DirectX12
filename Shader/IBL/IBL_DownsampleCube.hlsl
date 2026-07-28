// ============================================================
//  IBL_DownsampleCube.hlsl
//  キューブmip(N) -> mip(N+1) を 2x2 box downsample で生成
//  リソース状態を UNORDERED_ACCESS のまま保つため、
//  親mipも UAV(RWTexture2DArray, 読み取り) として参照する。
//  これにより per-subresource バリアの複雑さを回避。
// ============================================================

RWTexture2DArray<float4>  SrcMip : register(u1);   // 親mip(mip N) 読み取り
RWTexture2DArray<float4>  DstMip : register(u0);   // 子mip(mip N+1) 書き込み

cbuffer BakeParams : register(b0)
{
    uint  FaceSize;     // 出力(子)mipの一辺
    uint  MipLevel;
    float Roughness;
    float _pad;
};

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= FaceSize || id.y >= FaceSize)
        return;

    uint face = id.z;

    // 子の (x,y) -> 親の 2x2 ブロック平均
    uint2 p = id.xy * 2;

    float4 c0 = SrcMip[uint3(p.x,     p.y,     face)];
    float4 c1 = SrcMip[uint3(p.x + 1, p.y,     face)];
    float4 c2 = SrcMip[uint3(p.x,     p.y + 1, face)];
    float4 c3 = SrcMip[uint3(p.x + 1, p.y + 1, face)];

    float4 avg = (c0 + c1 + c2 + c3) * 0.25f;

    DstMip[uint3(id.xy, face)] = avg;
}
