#include "LumenSceneLightingCommon.hlsl"

// =============================================================
//  LumenRadiosity_CS
//  LumenRadiosity.usf 相当。Surface Cache の各カードテクセルから
//  半球コサイン分布のレイを SDF トレースし、ヒット先の
//  FinalLighting (前フレーム) を採光して間接イラディアンスを
//  IndirectLightingAtlas (u1) へ書く。
//
//  FinalLighting には Emissive が合成済みのため、発光面の光は
//  ここで壁面へ回り込む (= エミッシブの間接照明)。さらに
//  FinalLighting 自体が (直接 + 間接) から作られるため、フレームを
//  跨いだフィードバックで多バウンスが蓄積される。
//  ミス時はスカイ (IBL irradiance) を採光する。
//
//  更新はフレーム予算制 (CardStartIndex から NumCardsToProcess 枚)。
//
//  テンポラル蓄積: 1 テクセルあたり数本のレイを ~6 フレームごとに
//  差し替えるだけだと、発光面の近くでは「レイが当たった / 外れた」で
//  推定が大きく揺れ、FinalLighting -> スクリーンプローブ経由で画面の
//  明部がプルプル震える。前回の値 (u1 の自分自身) と
//  PassRadiosityParams.x でブレンドして分散を抑える
//  (Radiosity テンポラル蓄積相当。a = 履歴有効マーカー)。
//  Dispatch: (CARD_RES/8, CARD_RES/8, NumCardsToProcess)
// =============================================================

// Wang ハッシュ (テクセル / フレームごとのレイ回転用)
uint LumenWangHash(uint seed)
{
    seed = (seed ^ 61u) ^ (seed >> 16);
    seed *= 9u;
    seed = seed ^ (seed >> 4);
    seed *= 0x27d4eb2du;
    seed = seed ^ (seed >> 15);
    return seed;
}

[numthreads(8, 8, 1)]
void main(uint3 GroupID : SV_GroupID, uint3 GroupThreadID : SV_GroupThreadID)
{
    const uint cardIndex = CardStartIndex + GroupID.z;
    FLumenCardData card = LumenCardBuffer[cardIndex];

    const uint2 texelInCard = GroupID.xy * 8u + GroupThreadID.xy;

    if (card.CardExtentAndValid.w < 0.5f)
    {
        // 無効カード: タイルをゼロで確定させる (未初期化値の混入防止。
        // a = 0 で履歴も無効化 -> 再有効化時は蓄積なしで書き直す)
        RWIndirectLighting[GetLumenCardTileOrigin(cardIndex) + texelInCard] =
            float4(0.0f, 0.0f, 0.0f, 0.0f);
        return;
    }

    FLumenCardTexel texel = ReconstructLumenCardTexel(card, cardIndex, texelInCard);

    if (!texel.bValid)
    {
        RWIndirectLighting[texel.AtlasTexel] = float4(0.0f, 0.0f, 0.0f, 0.0f);
        return;
    }

    const uint numRays = max((uint) PassTraceParams.z, 1u);
    const float maxTrace = PassTraceParams.x;
    const float surfaceBias = PassTraceParams.y;

    // 半球を numRays 個のコーンで分割したときのコーン半角 tan
    // (立体角 2pi/numRays -> cos = 1 - 1/numRays)
    const float coneCos = saturate(1.0f - 1.0f / (float) numRays);
    const float coneTan = sqrt(saturate(1.0f - coneCos * coneCos)) / max(coneCos, 0.1f);

    // テクセル + フレームでレイ方位を回転 (時間方向のサンプル分散)
    uint seed = texel.AtlasTexel.x | (texel.AtlasTexel.y << 16);
    seed = LumenWangHash(seed + (uint) PassAtlasParams.w * 0x9E3779B9u);
    const float randomRotation = (float) (seed & 0xFFFFu) / 65536.0f * (2.0f * LUMEN_PI);

    const float3 rayStart = texel.WorldPosition + texel.WorldNormal * surfaceBias;

    float3 radianceSum = float3(0.0f, 0.0f, 0.0f);

    [loop]
    for (uint r = 0; r < numRays; ++r)
    {
        float3 rayDir = GetLumenHemisphereRay(texel.WorldNormal, r, numRays, randomRotation);

        // ハイブリッド (近距離: メッシュ SDF / 遠距離: Global SDF) または
        // HWRT (RayQuery)。ヒット = 前フレーム FinalLighting (Emissive
        // 合成済み) の採光、ミス = prefilter 環境スカイ。
        FLumenTraceResult trace = TraceLumenRay(
            rayStart, rayDir, maxTrace, coneTan, PassNumLumenObjects);

        radianceSum += ResolveLumenRayRadiance(trace, rayStart, rayDir, PassRCParams1.w);
    }

    // コサイン重点サンプルの irradiance 推定: E = pi * mean(L)
    float3 indirectIrradiance = radianceSum * (LUMEN_PI / (float) numRays);

    // ---- テンポラル蓄積 (前回の自分自身とブレンド) ----
    // alpha >= 1 (C++ 側で型付き UAV ロード非対応 or 蓄積 OFF) のときは
    // UAV を読まずに置き換えるだけ
    const float temporalAlpha = PassRadiosityParams.x;

    [branch]
    if (temporalAlpha < 1.0f)
    {
        float4 prev = RWIndirectLighting[texel.AtlasTexel];
        if (prev.a > 0.5f) // 履歴有効 (無効カード / 起動直後の 0 は蓄積しない)
        {
            indirectIrradiance = lerp(prev.rgb, indirectIrradiance, temporalAlpha);
        }
    }

    RWIndirectLighting[texel.AtlasTexel] = float4(indirectIrradiance, 1.0f);
}
