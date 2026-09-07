#include "Common.hlsl"

// =============================================================
//  LumenCardCapturePS
//  LumenCardPixelShader (Surface Cache カードキャプチャ) 相当。
//  メッシュをカードのオルソ投影 (ローカル空間, b1 = 単位行列で
//  b0.View / Projection にカードビューが入る) で描き、マテリアル
//  属性を Surface Cache アトラスのタイルへ焼く:
//    RT0 = AlbedoAtlas   (RGBA8)      : 拡散アルベド (a=1 有効マーカー)
//    RT1 = NormalAtlas   (RGBA8)      : カード空間法線 *0.5+0.5 (a=1)
//    RT2 = EmissiveAtlas (R11G11B10F) : エミッシブラディアンス
//    DSV = DepthAtlas    (D32)        : カード深度 (0..1, オルソ線形)
//
//  Emissive はここでアトラスに入り、LumenSceneCombine_CS が
//  FinalLighting へ合成することで「発光面が光源として振る舞う」
//  (Radiosity / スクリーン GI のトレースが採光する)。
//
//  キャプチャは常にカリング無効 PSO (裏面は法線反転)。
//  Translucent / Additive サブセットはキャプチャに参加しない
//  (プロキシ側 DrawCardCapture でスキップ)。
// =============================================================

struct PS_OUTPUT_CARD_CAPTURE
{
    float4 Albedo : SV_TARGET0;
    float4 Normal : SV_TARGET1;
    float4 Emissive : SV_TARGET2;
};

PS_OUTPUT_CARD_CAPTURE main(PS_INPUT input, bool bIsFrontFace : SV_IsFrontFace)
{
    PS_OUTPUT_CARD_CAPTURE output;

    // ---- BaseColor ----
    float4 baseColor = TextureBaseColor.Sample(Sampler, input.TexCoord) * input.Color;

    // ---- BLEND_Masked: OpacityMask クリップ (ベースパスと同一規約) ----
    if (Material.BlendMode == BLEND_MASKED)
    {
        clip(baseColor.a - Material.OpacityMaskClipValue);
    }

    // ---- 法線 (b1 = 単位行列なので input.Normal はローカル法線) ----
    // キャプチャはカリング無効: 裏面ピクセルは幾何法線を反転する
    float3 localNormal = normalize(input.Normal.xyz);
    if (!bIsFrontFace)
    {
        localNormal = -localNormal;
    }

    // カード空間へ (b0.View = カードビュー行列。回転のみ有効)
    float3 cardNormal = normalize(mul(float4(localNormal, 0.0f), View).xyz);

    // ---- アルベド ----
    // Unlit はライティング寄与なし (アルベド 0)。
    // Substrate は DiffuseAlbedo ピン x BaseColor (GeometryPS と同一規約)。
    // レガシーは金属を除外した拡散アルベド (DeferredPS の
    // lumenDiffuseAlbedo = baseColor * (1 - metallic) と一致させる。
    // 金属を拡散反射体として焼くと、直接描画には存在しない色付きバウンス
    // が発生し Radiosity のフィードバックでエネルギーが増える)。
    float3 albedo = baseColor.rgb;
    if (Material.bUseSubstrate)
    {
        albedo = Material.SubstrateDiffuseAlbedo.rgb * baseColor.rgb;
    }
    else
    {
        // ARM フォールバック規約は GeometryPS と同一
        float4 ARM = TextureMSRA.Sample(Sampler, input.TexCoord);
        float metallic = (ARM.b == 0.0f) ? Material.Metallic : ARM.b;
        albedo = baseColor.rgb * (1.0f - metallic);
    }
    if (Material.Unlit)
    {
        albedo = float3(0.0f, 0.0f, 0.0f);
    }

    // ---- エミッシブ (発光面 -> Surface Cache 光源化の入口) ----
    // 直接描画にエミッシブが出る経路とだけ一致させる:
    //   Unlit     : GeometryPS の Unlit 合成 (EmissionColor + BaseColor 変調)
    //   Substrate : SlabBSDF.Emissive として DeferredPS が加算する
    //   レガシー Lit : G-Buffer にエミッシブチャンネルが無く画面に出ない。
    //                  ここで焼くと「自分は真っ黒なのに周囲だけ照らす」
    //                  不可視光源になるため対象外とする。
    float3 emissive = float3(0.0f, 0.0f, 0.0f);
    if (Material.Unlit)
    {
        emissive = Material.EmissionColor.rgb + baseColor.rgb * Material.BaseColor.rgb;
    }
    else if (Material.bUseSubstrate)
    {
        emissive = Material.EmissionColor.rgb;
    }

    output.Albedo = float4(albedo, 1.0f); // a=1: 有効テクセルマーカー
    output.Normal = float4(cardNormal * 0.5f + 0.5f, 1.0f);
    output.Emissive = float4(emissive, 1.0f);

    return output;
}
