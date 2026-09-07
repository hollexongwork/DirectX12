#include "LumenSceneLightingCommon.hlsl"

// =============================================================
//  LumenSceneDirectLighting_CS
//  LumenSceneDirectLighting.usf 相当。Surface Cache の各カード
//  テクセルに対する直接光イラディアンス [lux] を計算して
//  DirectLightingAtlas (u0) へ書く (アルベド乗算は Combine が行う)。
//    - ディレクショナル: メッシュ SDF の遮蔽トレース付き
//    - ローカル (Point/Spot/Rect): 距離減衰 + コーン減衰 +
//      SDF 遮蔽トレース (拡散のみ。スペキュラはカードでは扱わない)
//
//  Dispatch: (CARD_RES/8, CARD_RES/8, NumCardsToProcess)
// =============================================================

// 遮蔽トレースをスキップする寄与しきい値 (負荷対策)
static const float SHADOW_TRACE_THRESHOLD = 0.005f;

// ライトのソフトネス (コーン半角 tan)。ディレクショナルは太陽の
// 見かけ角相当の小さめ、ローカルは点光源の近距離を考慮して広め。
static const float DIRECTIONAL_SHADOW_CONE_TAN = 0.05f;
static const float LOCAL_SHADOW_CONE_TAN = 0.1f;

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
    const float maxTrace = PassTraceParams.x;
    const float3 rayStart = worldPos + worldNormal * surfaceBias;

    float3 directLighting = float3(0.0f, 0.0f, 0.0f);

    // ---- ディレクショナルライト ----
    [branch]
    if (PassDirectionalLightDirection.w > 0.5f)
    {
        float3 toLight = normalize(PassDirectionalLightDirection.xyz);
        float NdotL = saturate(dot(worldNormal, toLight));

        [branch]
        if (NdotL > 0.0f)
        {
            // ハイブリッド (SWRT) / RayQuery (HWRT) の遮蔽トレース
            float shadow = TraceLumenRay(
                rayStart, toLight, maxTrace,
                DIRECTIONAL_SHADOW_CONE_TAN, PassNumLumenObjects).Visibility;

            directLighting += PassDirectionalLightColor.rgb * (NdotL * shadow);
        }
    }

    // ---- ローカルライト (Point / Spot / Rect: 拡散のみ) ----
    [loop]
    for (uint i = 0; i < PassNumLocalLights; ++i)
    {
        FLightShaderParameters light = LumenLocalLights[i];

        float3 toLight = light.Position - worldPos;
        float distSq = dot(toLight, toLight);
        float dist = sqrt(max(distSq, 1e-8f));
        float normalizedDist = dist * light.InvRadius;

        if (normalizedDist >= 1.0f)
        {
            continue; // 減衰半径外
        }

        float3 L = toLight / dist;
        float NdotL = saturate(dot(worldNormal, L));
        if (NdotL <= 0.0f)
        {
            continue;
        }

        // ---- 距離減衰 ----
        float attenuation;
        [branch]
        if (light.Flags & LIGHT_FLAG_INVERSE_SQUARED)
        {
            // 逆二乗 + 半径ウィンドウ (1 - (d/r)^4)^2
            float window = saturate(1.0f - normalizedDist * normalizedDist *
                normalizedDist * normalizedDist);
            attenuation = (window * window) / max(distSq, 0.0001f);
        }
        else
        {
            attenuation = pow(saturate(1.0f - normalizedDist), light.FalloffExponent);
        }

        // ---- コーン減衰 (Spot) / 半球制限 (Rect) ----
        if (light.Type == LIGHT_TYPE_SPOT)
        {
            float cosAngle = dot(-L, light.Direction);
            float coneAtten = saturate((cosAngle - light.SpotAngles.x) * light.SpotAngles.y);
            attenuation *= coneAtten * coneAtten;
        }
        else if (light.Type == LIGHT_TYPE_RECT)
        {
            // 面の表側のみ照らす簡易近似
            attenuation *= (dot(-L, light.Direction) > 0.0f) ? 1.0f : 0.0f;
        }

        float3 contribution = light.Color * (NdotL * attenuation);

        if (max(contribution.r, max(contribution.g, contribution.b)) < SHADOW_TRACE_THRESHOLD)
        {
            continue; // 寄与が小さすぎる: 遮蔽トレースを省略して破棄
        }

        // ---- 遮蔽トレース (ライトまで) ----
        float shadow = TraceLumenRay(
            rayStart, L, max(dist - surfaceBias, 0.0f),
            LOCAL_SHADOW_CONE_TAN, PassNumLumenObjects).Visibility;

        directLighting += contribution * shadow;
    }

    RWDirectLighting[texel.AtlasTexel] = float4(directLighting, 1.0f);
}
