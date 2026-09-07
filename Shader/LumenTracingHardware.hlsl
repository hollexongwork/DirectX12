#ifndef LUMEN_TRACING_HARDWARE_HLSL
#define LUMEN_TRACING_HARDWARE_HLSL

// =============================================================
//  LumenTracingHardware
//  LumenHardwareRayTracing 相当。DXR 1.1 のインライン
//  レイトレーシング (RayQuery, SM 6.5) で Lumen シーンをトレース
//  する。TLAS のインスタンス ID = Lumen オブジェクトスロット
//  なので、ヒットの Surface Cache 採光は SWRT と同じ経路を使う
//  (ヒット法線はそのオブジェクトのメッシュ SDF 勾配で求める =
//  頂点バッファのバインド不要なハイブリッド構成)。
//
//  LUMEN_HWRT=1 の RT バリアント (*RT_CS.hlsl) だけがインクルード
//  する。ルートシグネチャの [37] (ルート SRV t28) に TLAS を
//  バインドすること (LumenScene.cpp / LumenHardwareRayTracing.h)。
// =============================================================

RaytracingAccelerationStructure LumenSceneTLAS : register(t28);

// -------------------------------------------------------------
//  最近ヒットトレース (FLumenTraceResult は LumenTracingCommon)
//  HWRT はバイナリ可視率 (ソフトコーンなし。UE の HWRT シャドウと
//  同じ挙動)。ヒットオブジェクト = InstanceID (Lumen スロット)。
// -------------------------------------------------------------
FLumenTraceResult TraceLumenSceneHardware(float3 RayStart, float3 RayDir, float MaxT,
    uint NumObjects)
{
    FLumenTraceResult result;
    result.bHit = false;
    result.HitT = MaxT;
    result.HitObject = 0u;
    result.Visibility = 1.0f;

    RayDesc ray;
    ray.Origin = RayStart;
    ray.Direction = RayDir;
    ray.TMin = 0.0f;
    ray.TMax = MaxT;

    RayQuery < RAY_FLAG_FORCE_OPAQUE > query;
    query.TraceRayInline(LumenSceneTLAS, RAY_FLAG_NONE, 0xFFu, ray);

    // FORCE_OPAQUE なので固定機能トラバーサルのみ (候補処理不要)
    while (query.Proceed())
    {
    }

    [branch]
    if (query.CommittedStatus() == COMMITTED_TRIANGLE_HIT)
    {
        result.bHit = true;
        result.HitT = query.CommittedRayT();
        result.HitObject = min(query.CommittedInstanceID(), max(NumObjects, 1u) - 1u);
        result.Visibility = 0.0f;
    }

    return result;
}

#endif
