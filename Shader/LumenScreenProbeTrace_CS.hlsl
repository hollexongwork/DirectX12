#include "LumenSceneLightingCommon.hlsl"
#include "LumenProbeCommon.hlsl"

// =============================================================
//  LumenScreenProbeTrace_CS
//  ScreenProbeTrace 相当。プローブごとに hemi-octahedral 8x8 の
//  レイを飛ばし、放射輝度をトレースラディアンスアトラスへ書く:
//    1. スクリーンスペーストレース (現深度 + 前フレーム SceneColor)
//    2. ミス時は SDF (SWRT: メッシュ + Global) / HWRT (RayQuery)
//    3. さらにミスなら prefilter 環境 (スカイ)
//
//    t19 = ProbeGeo / u4 = TraceRadiance ((PW*8) x (PH*8), RGBA16F)
//         rgb = 放射輝度, a = スカイ可視率 (ヒットで 0)
//  Dispatch: (PW, PH, 1) - 1 グループ = 1 プローブ (8x8 スレッド)
// =============================================================

Texture2D<float4> ProbeGeoTexture : register(t19);
RWTexture2D<float4> RWTraceRadiance : register(u4);

[numthreads(8, 8, 1)]
void main(uint3 GroupID : SV_GroupID, uint3 GroupThreadID : SV_GroupThreadID)
{
    const uint2 probe = GroupID.xy;
    const uint2 octaTexel = GroupThreadID.xy;
    const uint2 atlasTexel = probe * 8u + octaTexel;

    float4 probeGeo = ProbeGeoTexture.Load(int3(probe, 0));

    [branch]
    if (probeGeo.w <= 0.0f)
    {
        RWTraceRadiance[atlasTexel] = float4(0.0f, 0.0f, 0.0f, 1.0f);
        return;
    }

    // ---- プローブのワールド位置 (アンカーピクセルから再構築) ----
    const uint downsample = (uint) PassProbeParams0.z;
    const uint2 screenSize = (uint2) PassProbeParams1.xy;
    uint2 anchor = min(probe * downsample + downsample / 2u, screenSize - 1u);
    float deviceDepth = LumenSceneDepth.Load(int3(anchor, 0));
    float3 worldPos = LumenReconstructWorldPosition(anchor, deviceDepth);

    const float3 probeNormal = probeGeo.xyz;

    // ---- レイ方向 (hemi-octahedral + フレームジッタ) ----
    uint probeSeed = probe.x | (probe.y << 16);
    float2 jitter = LumenGetFrameJitter((uint) PassAtlasParams.w, probeSeed);
    float2 octaUV = ((float2) octaTexel + jitter) / 8.0f;

    float3 tangent, bitangent;
    LumenBuildTangentBasis(probeNormal, tangent, bitangent);
    float3 localDir = LumenHemiOctahedronToDirection(octaUV);
    float3 rayDir = normalize(
        tangent * localDir.x + bitangent * localDir.y + probeNormal * localDir.z);

    const float maxTrace = PassTraceParams.x;
    const float bias = PassTraceParams.y;
    float3 rayStart = worldPos + probeNormal * bias;

    // 8x8 半球分割のコーン半角 tan (立体角 2pi/64)
    const float coneTan = 0.18f;

    // ---- 1. スクリーンスペーストレース ----
    float3 radiance;
    [branch]
    if (LumenScreenSpaceTrace(rayStart, rayDir, maxTrace, bias, radiance))
    {
        RWTraceRadiance[atlasTexel] = float4(radiance, 0.0f);
        return;
    }

    // ---- 2. SDF / HWRT トレース -> 3. スカイ ----
    FLumenTraceResult trace = TraceLumenRay(
        rayStart, rayDir, maxTrace, coneTan, PassNumLumenObjects);

    radiance = ResolveLumenRayRadiance(trace, rayStart, rayDir, PassRCParams1.w);

    RWTraceRadiance[atlasTexel] = float4(radiance, trace.Visibility);
}
