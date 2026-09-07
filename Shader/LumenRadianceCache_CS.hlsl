#include "LumenSceneLightingCommon.hlsl"
#include "LumenProbeCommon.hlsl"

// =============================================================
//  LumenRadianceCache_CS
//  LumenRadianceCache 相当のワールド空間プローブ。カメラ周囲の
//  N^3 グリッド (トロイダルアドレッシング) の各プローブから
//  octahedral 8x8 の全球レイをトレースし、放射輝度アトラスへ書く。
//  更新はフレーム予算制 (PassRCParams1.y = 開始, z = 個数)。
//
//    u4 = RC ラディアンスアトラス ((N*N/8?) 固定: 512x512 =
//         64x64 タイル x 8x8 octa。タイル = テクスチャ空間
//         リニアプローブ番号)
//  Dispatch: (1, 1, 更新プローブ数) - 1 グループ = 1 プローブ (8x8)
// =============================================================

RWTexture2D<float4> RWRadianceCacheAtlas : register(u4);

[numthreads(8, 8, 1)]
void main(uint3 GroupID : SV_GroupID, uint3 GroupThreadID : SV_GroupThreadID)
{
    const int probesPerAxis = (int) PassRCParams1.x;
    const int numProbes = probesPerAxis * probesPerAxis * probesPerAxis;

    // テクスチャ空間リニアプローブ番号 (ラウンドロビン + ラップ)
    const uint probeLinear =
        ((uint) PassRCParams1.y + GroupID.z) % (uint) numProbes;

    // テクスチャ空間 3D インデックス
    int3 texIndex;
    texIndex.x = (int) (probeLinear % (uint) probesPerAxis);
    texIndex.y = (int) ((probeLinear / (uint) probesPerAxis) % (uint) probesPerAxis);
    texIndex.z = (int) (probeLinear / (uint) (probesPerAxis * probesPerAxis));

    // トロイダル: テクスチャインデックス -> ワールドセル
    const float spacing = PassRCParams0.w;
    int3 minWorldCell = (int3) floor(PassRCParams0.xyz / spacing + 0.5f);
    int3 worldCell = LumenRCWorldCellFromTexIndex(texIndex, minWorldCell, probesPerAxis);

    float3 probePos = ((float3) worldCell + 0.5f) * spacing;

    // ---- octahedral 全球レイ ----
    const uint2 octaTexel = GroupThreadID.xy;
    float2 octaUV = ((float2) octaTexel + 0.5f) / 8.0f;
    float3 rayDir = LumenOctahedronToDirection(octaUV);

    // 全球 64 分割のコーン半角 tan (立体角 4pi/64)
    const float coneTan = 0.25f;
    const float maxTrace = PassTraceParams.x;

    FLumenTraceResult trace = TraceLumenRay(
        probePos, rayDir, maxTrace, coneTan, PassNumLumenObjects);

    float3 radiance = ResolveLumenRayRadiance(trace, probePos, rayDir, PassRCParams1.w);

    // アトラスタイル: 64 タイル/行 (512 / 8)
    uint2 tileOrigin = uint2(probeLinear % 64u, probeLinear / 64u) * 8u;
    RWRadianceCacheAtlas[tileOrigin + octaTexel] = float4(radiance, trace.Visibility);
}
