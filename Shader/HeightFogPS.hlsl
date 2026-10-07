#include "HeightFogCommon.hlsl"

// =============================================================
//  HeightFogPS
//  ExponentialPixelMain 相当のフルスクリーン
//  フォグパス。デファードライティング後の HDR SceneColor に対し、
//  深度からワールド座標を再構築して Exponential Height Fog
//  (+ Volumetric Fog の froxel 積分結果) を合成する。
//
//  出力: rgb = インスキャッタ (加算), a = 透過率 (乗算)
//  ブレンド (PSO "HeightFog", EBlendStatePreset::HeightFog):
//    SceneColor' = Src.rgb * 1 + SceneColor * Src.a   (RGB のみ書き込み)
//  = TStaticBlendState<CW_RGB, BO_Add, BF_One, BF_SourceAlpha>
//
//  トランスルーセントはこのパスの対象外 (TranslucentPS が
//  サーフェス位置でフォグを直接評価して合成する)。
//  深度 1.0 (何も描かれていない) のピクセルは遠クリップ面の
//  ワールド座標としてフォグが掛かる (スカイドームと同じ扱い)。
// =============================================================

PS_OUTPUT main(PS_INPUT input)
{
    PS_OUTPUT output;

    // ---- ワールド座標の再構築 (DeferredPS と同じ手順) ----
    float2 uv = input.TexCoord;
    float depth = TextureDepth.Sample(Sampler2, uv).r;

    float4 ndcPos = float4(
        uv.x * 2.0f - 1.0f,
        (1.0f - uv.y) * 2.0f - 1.0f,
        depth,
        1.0f);

    float4 worldPos = mul(ndcPos, InvViewProjection);
    worldPos /= worldPos.w;

    // ビュー空間 Z (Volumetric Fog の Z スライス選択用)
    float sceneDepth = mul(float4(worldPos.xyz, 1.0f), View).z;

    // ---- 解析フォグ + Volumetric Fog ----
    float4 fog = ComputeFogInscatteringAndOpacity(worldPos.xyz, input.Position.xy, sceneDepth);

    output.Color = float4(fog.rgb, fog.a);
    return output;
}
