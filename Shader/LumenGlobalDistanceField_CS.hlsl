#include "LumenSceneLightingCommon.hlsl"

// =============================================================
//  LumenGlobalDistanceField_CS
//  GlobalDistanceField.usf 相当。シーンの全メッシュ SDF を
//  カメラ追従のクリップマップ (128^3, R16F, ワールド距離 [m]) へ
//  合成する。ボリューム外のボクセルは
//    d ~ |p - clamp(p)| + SDF(clamp(p))
//  で近似する。
//
//  CardStartIndex = ビルド対象のクリップマップ番号 (0/1)。
//  Dispatch: (128/8, 128/8, 128/8)
// =============================================================

[numthreads(8, 8, 8)]
void main(uint3 DTid : SV_DispatchThreadID)
{
    const float4 clipmap = (CardStartIndex == 0u) ? PassGlobalSDF0 : PassGlobalSDF1;
    const float halfExtent = clipmap.w;

    // ボクセル中心のワールド座標
    float3 uvw = ((float3) DTid + 0.5f) / LUMEN_GLOBAL_SDF_RESOLUTION;
    float3 worldPos = clipmap.xyz + (uvw - 0.5f) * (2.0f * halfExtent);

    // 遮蔽なしの既定値 (クリップマップ直径 = 十分遠い)
    float minDistance = 2.0f * halfExtent;

    [loop]
    for (uint i = 0; i < PassNumLumenObjects; ++i)
    {
        FLumenSceneObject obj = LumenSceneObjects[i];
        if (obj.bValid == 0u)
        {
            continue;
        }

        float d = GetLumenObjectDistanceApprox(obj, worldPos);

        minDistance = min(minDistance, d);
    }

    RWGlobalSDF[DTid] = minDistance;
}
