#include "Common.hlsl"

// =============================================================
//  ShadowDepthVS
//  シャドウ深度パスの頂点シェーダ (FShadowDepthVS)。
//  b0 (View / Projection) にはライトのビュー / 射影が入っている
//  (FShadowSceneRenderer が各シャドウビューごとに詰め直す)。
//
//  Masked マテリアルの OpacityMask クリップ (ShadowDepthMaskedPS)
//  のため TexCoord / 頂点カラーも出力する。不透明マテリアル用の
//  ShadowDepthPS は SV_POSITION だけを読む (PS 入力署名は
//  VS 出力署名の部分集合として正しくリンクされる)。
// =============================================================

struct SHADOW_DEPTH_VS_OUTPUT
{
    float4 Position : SV_POSITION;
    float2 TexCoord : TEXCOORD;
    float4 Color : COLOR;
};

SHADOW_DEPTH_VS_OUTPUT main(VS_INPUT input)
{
    SHADOW_DEPTH_VS_OUTPUT output;

    float4x4 wvp = mul(LocalToWorld, View);
    wvp = mul(wvp, Projection);

    output.Position = mul(float4(input.Position, 1.0f), wvp);
    output.TexCoord = input.TexCoord;
    output.Color = input.Color;

    return output;
}
