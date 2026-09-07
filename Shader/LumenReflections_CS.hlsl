#include "LumenSceneLightingCommon.hlsl"
#include "LumenProbeCommon.hlsl"

// =============================================================
//  LumenReflections_CS
//  LumenReflections 相当のスペキュラ GI。ピクセルごとに反射レイを
//    1. スクリーンスペーストレース (前フレーム SceneColor)
//    2. SDF (SWRT) / RayQuery (HWRT)
//  の順でトレースし、ヒット先の Surface Cache FinalLighting を
//  採光する。ミスは prefilter 環境をラフネス対応ミップで採光
//  (= 既存 IBL と同一写像なのでシームレスにフォールバックする)。
//
//  出力はプレフィルタ「差し替え」ラディアンス:
//    DeferredPS が IBL スペキュラの prefiltered サンプルを
//    この値で lerp する (BRDF / フレネル重みは既存経路のまま)。
//    t15 = GBufferA / t16 = 深度 / t19 = GBufferB (ラフネス)
//    u4  = ReflectionTexture (RGBA16F): rgb = ラディアンス, a = 差し替え率
//  Dispatch: (ceil(W/8), ceil(H/8), 1)
// =============================================================

Texture2D<float4> ReflectionGBufferB : register(t19); // R=Metallic G=Specular B=Roughness A=AO

RWTexture2D<float4> RWReflections : register(u4);

[numthreads(8, 8, 1)]
void main(uint3 DTid : SV_DispatchThreadID)
{
    const uint2 screenSize = (uint2) PassProbeParams1.xy;
    if (DTid.x >= screenSize.x || DTid.y >= screenSize.y)
    {
        return;
    }

    const uint2 pixel = DTid.xy;

    float deviceDepth = LumenSceneDepth.Load(int3(pixel, 0));
    float4 normalSample = LumenGBufferNormal.Load(int3(pixel, 0));
    float roughness = ReflectionGBufferB.Load(int3(pixel, 0)).b;

    const float maxRoughness = PassReflectionParams.x;
    const float fadeStart = PassReflectionParams.y;

    [branch]
    if (deviceDepth >= 0.9999f || normalSample.w < 0.5f || roughness > maxRoughness)
    {
        RWReflections[pixel] = float4(0.0f, 0.0f, 0.0f, 0.0f);
        return;
    }

    float3 worldPos = LumenReconstructWorldPosition(pixel, deviceDepth);
    float3 N = normalize(normalSample.xyz);
    float3 V = normalize(PassCameraOrigin.xyz - worldPos);
    float3 R = reflect(-V, N);

    // ラフネス -> 反射コーン半角 tan (GGX ローブ幅の近似)
    float coneTan = max(roughness * roughness, 0.02f);

    const float maxTrace = PassTraceParams.x;
    const float bias = PassTraceParams.y;
    float3 rayStart = worldPos + N * bias;

    // ラフネスフェード (fadeStart..maxRoughness で IBL へ戻す)。
    // 差し替え率は [0,1] を保ち、強度はラディアンス側に乗算する
    // (lerp の外挿でスペキュラが負になるのを防ぐ)
    const float replaceWeight = saturate(1.0f - saturate(
        (roughness - fadeStart) / max(maxRoughness - fadeStart, 0.001f)));
    const float intensity = PassReflectionParams.z;

    // ---- 1. スクリーンスペーストレース ----
    float3 radiance;
    [branch]
    if (LumenScreenSpaceTrace(rayStart, R, maxTrace, bias, radiance))
    {
        RWReflections[pixel] = float4(radiance * intensity, replaceWeight);
        return;
    }

    // ---- 2. SDF / HWRT トレース ----
    FLumenTraceResult trace = TraceLumenRay(
        rayStart, R, maxTrace, coneTan, PassNumLumenObjects);

    [branch]
    if (trace.bHit)
    {
        FLumenSceneObject hitObj = LumenSceneObjects[trace.HitObject];
        float3 hitPos = rayStart + R * trace.HitT;
        float3 hitNormal = ComputeLumenHitNormal(hitObj, hitPos, rayStart);
        radiance = SampleLumenSurfaceCache(hitObj, hitPos, hitNormal);
    }
    else
    {
        // ミス: prefilter 環境をラフネス対応ミップで (IBL と同一写像 =
        // 差し替えても見た目が既存 IBL と一致する)
        // ミス: prefilter 環境をラフネス対応ミップで (IBL と同一写像 =
        // 差し替えても見た目が既存 IBL と一致する)。強度は掛けない
        // (掛けると素の空反射だけが明るさ変化してしまう)。
        const float PREFILTER_MAX_MIP_F = 4.0f;
        RWReflections[pixel] = float4(LumenSkyPrefilter.SampleLevel(
            LumenTraceSampler, R, roughness * PREFILTER_MAX_MIP_F).rgb, replaceWeight);
        return;
    }

    RWReflections[pixel] = float4(radiance * intensity, replaceWeight);
}
