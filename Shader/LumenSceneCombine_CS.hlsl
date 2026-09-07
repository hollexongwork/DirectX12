#include "LumenSceneLightingCommon.hlsl"

// =============================================================
//  LumenSceneCombine_CS
//  CombineLumenSceneLighting 相当。Surface Cache の最終出射
//  ラディアンスを合成して FinalLightingAtlas (u2) へ書く:
//
//    FinalLighting = (Direct + Indirect) * Albedo / pi
//                  + Emissive * EmissiveBoost
//
//  Emissive がここで FinalLighting に入ることで、発光面は
//  Radiosity / スクリーン GI のトレースから「光源」として見える
//  (= Surface Cache が Emissive を光源として扱う仕組みの本体)。
//  FinalLighting.a はキャプチャ有効率で、採光側 (バイリニア) の
//  無効テクセル混入の重み除去に使う。
//
//  Dispatch: (CARD_RES/8, CARD_RES/8, NumCardsToProcess)
// =============================================================

[numthreads(8, 8, 1)]
void main(uint3 GroupID : SV_GroupID, uint3 GroupThreadID : SV_GroupThreadID)
{
    const uint cardIndex = CardStartIndex + GroupID.z;
    FLumenCardData card = LumenCardBuffer[cardIndex];

    const uint2 texelInCard = GroupID.xy * 8u + GroupThreadID.xy;
    uint2 atlasTexel = GetLumenCardTileOrigin(cardIndex) + texelInCard;

    if (card.CardExtentAndValid.w < 0.5f)
    {
        // 無効カード: タイルをゼロで確定させる (採光側は a=0 で棄却)
        RWFinalLighting[atlasTexel] = float4(0.0f, 0.0f, 0.0f, 0.0f);
        return;
    }

    float4 albedo = LumenAlbedoAtlas.Load(int3(atlasTexel, 0));
    float validity = albedo.a;

    [branch]
    if (validity < 0.5f)
    {
        RWFinalLighting[atlasTexel] = float4(0.0f, 0.0f, 0.0f, 0.0f);
        return;
    }

    float3 direct = LumenDirectLightingSRV.Load(int3(atlasTexel, 0)).rgb;
    float3 indirect = LumenIndirectLightingSRV.Load(int3(atlasTexel, 0)).rgb;
    float3 emissive = LumenEmissiveAtlas.Load(int3(atlasTexel, 0)).rgb;

    const float emissiveBoost = PassTraceParams.w;

    // 拡散面の出射ラディアンス: L = (E_direct + E_indirect) * albedo / pi
    // + エミッシブ (radiance をそのまま加算 = 発光面の光源化)
    float3 finalLighting = (direct + indirect) * albedo.rgb / LUMEN_PI
        + emissive * emissiveBoost;

    RWFinalLighting[atlasTexel] = float4(finalLighting, 1.0f);
}
