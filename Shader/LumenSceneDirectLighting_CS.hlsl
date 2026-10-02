#define NON_DIRECTIONAL_DIRECT_LIGHTING 1

#include "LumenSceneLightingCommon.hlsl"
#include "DeferredLightingCommon.hlsl"

// =============================================================
//  LumenSceneDirectLighting_CS
//  LumenSceneDirectLighting.usf 相当。Surface Cache の各カード
//  テクセルに対する直接光イラディアンス [lux] を計算して
//  DirectLightingAtlas (u0) へ書く (アルベド乗算は Combine が行う)。
//
//  ライトは Lumen 用ライトバッファ (t3) を全灯巡回する
//  (ディレクショナル + ローカル。C++ ComputeLightGrid が
//   bAffectGlobalIllumination のライトだけを IndirectLightingIntensity
//   込みで積む)。1 灯の評価は UE の GetIrradianceForLight と同じく
//    GetLocalLightAttenuation (半径窓 / コーン / レクト背面)
//    x IntegrateLight (面光源の形状を考慮したフォールオフ)
//    x N.L x 遮蔽トレース (メッシュ SDF / Global SDF / HWRT)
//  スペキュラはカードでは扱わない。
//
//  Dispatch: (CARD_RES/8, CARD_RES/8, NumCardsToProcess)
// =============================================================

// 遮蔽トレースをスキップする寄与しきい値 (負荷対策)
static const float SHADOW_TRACE_THRESHOLD = 0.005f;

// ライトのソフトネス (コーン半角 tan)。ディレクショナルは太陽の
// 見かけ角相当の小さめ、ローカルは点光源の近距離を考慮して広め。
static const float DIRECTIONAL_SHADOW_CONE_TAN = 0.05f;
static const float LOCAL_SHADOW_CONE_TAN = 0.1f;

// -------------------------------------------------------------
//  ライト 1 灯のイラディアンス (UE GetIrradianceForLight)
//  戻り値: 遮蔽前の寄与。OutL / OutTraceDistance は遮蔽トレース用
// -------------------------------------------------------------
float3 GetIrradianceForLight(
    FDeferredLightData LightData, float3 WorldPosition, float3 WorldNormal,
    out float3 OutL, out float OutTraceDistance)
{
    float3 L = LightData.Direction;
    float3 ToLight = L;
    float LightMask = 1.0f;
    OutTraceDistance = PassTraceParams.x;

    [branch]
    if (LightData.bRadialLight)
    {
        LightMask = GetLocalLightAttenuation(WorldPosition, LightData, ToLight, L);
        OutTraceDistance = length(ToLight);
    }

    OutL = L;

    float NoL = saturate(dot(WorldNormal, L));
    float3 Irradiance = float3(0.0f, 0.0f, 0.0f);

    [branch]
    if (LightMask > 0.0f && NoL > 0.0f)
    {
        float Attenuation;

        [branch]
        if (LightData.bRectLight)
        {
            FRect Rect = GetRect(ToLight, LightData);
            Attenuation = IntegrateLight(Rect);
        }
        else
        {
            FCapsuleLight Capsule = GetCapsule(ToLight, LightData);
            Capsule.DistBiasSqr = 0.0f;
            Attenuation = IntegrateLight(Capsule, LightData.bInverseSquared);
        }

        // DiffuseScale はこのライトの拡散光の倍率 (カードの直接光は拡散のみ)
        Irradiance = LightData.Color * (LightData.DiffuseScale * Attenuation * LightMask * NoL);
    }

    return Irradiance;
}

[numthreads(8, 8, 1)]
void main(uint3 GroupID : SV_GroupID, uint3 GroupThreadID : SV_GroupThreadID)
{
    const uint cardIndex = CardStartIndex + GroupID.z;
    FLumenCardData card = LumenCardBuffer[cardIndex];

    const uint2 texelInCard = GroupID.xy * 8u + GroupThreadID.xy;

    if (card.CardExtentAndValid.w < 0.5f)
    {
        // 無効カード: タイルをゼロで確定させる (未初期化値の混入防止)
        RWDirectLighting[GetLumenCardTileOrigin(cardIndex) + texelInCard] =
            float4(0.0f, 0.0f, 0.0f, 0.0f);
        return;
    }

    FLumenCardTexel texel = ReconstructLumenCardTexel(card, cardIndex, texelInCard);

    if (!texel.bValid)
    {
        RWDirectLighting[texel.AtlasTexel] = float4(0.0f, 0.0f, 0.0f, 0.0f);
        return;
    }

    const float3 worldPos = texel.WorldPosition;
    const float3 worldNormal = texel.WorldNormal;
    const float surfaceBias = PassTraceParams.y;
    const float3 rayStart = worldPos + worldNormal * surfaceBias;

    float3 directLighting = float3(0.0f, 0.0f, 0.0f);

    [loop]
    for (uint lightIndex = 0; lightIndex < PassNumLumenLights; ++lightIndex)
    {
        FDeferredLightData lightData = ConvertToDeferredLight(LumenLightBuffer[lightIndex]);

        float3 L;
        float traceDistance;
        float3 contribution = GetIrradianceForLight(lightData, worldPos, worldNormal, L, traceDistance);

        if (max(contribution.r, max(contribution.g, contribution.b)) < SHADOW_TRACE_THRESHOLD)
        {
            continue; // 寄与が小さすぎる: 遮蔽トレースを省略して破棄
        }

        // ---- 遮蔽トレース (ディレクショナル: 最大距離 / ローカル: ライトまで) ----
        // ハイブリッド (SWRT) / RayQuery (HWRT)
        float coneTan = lightData.bRadialLight ? LOCAL_SHADOW_CONE_TAN : DIRECTIONAL_SHADOW_CONE_TAN;
        float maxT = lightData.bRadialLight ? max(traceDistance - surfaceBias, 0.0f) : traceDistance;

        float shadow = TraceLumenRay(rayStart, L, maxT, coneTan, PassNumLumenObjects).Visibility;

        directLighting += contribution * shadow;
    }

    RWDirectLighting[texel.AtlasTexel] = float4(directLighting, 1.0f);
}
