#include "Common.hlsl"
#include "VertexFactory.hlsl"

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
//
//  頂点ファクトリ (VertexFactory.hlsl): ShadowDepthVS_GPUSkin.hlsl が
//  GPUSKIN_VERTEX_FACTORY を定義して本ファイルを include する。
// =============================================================

struct SHADOW_DEPTH_VS_OUTPUT
{
    float4 Position : SV_POSITION;
    float2 TexCoord : TEXCOORD;
    float4 Color : COLOR;
};

SHADOW_DEPTH_VS_OUTPUT main(FVertexFactoryInput input)
{
    SHADOW_DEPTH_VS_OUTPUT output;

    const FVertexFactoryIntermediates vfi = GetVertexFactoryIntermediates(input);
    const float3 localPosition = VertexFactoryGetLocalPosition(input, vfi);

    float4x4 wvp = mul(LocalToWorld, View);
    wvp = mul(wvp, Projection);

    output.Position = mul(float4(localPosition, 1.0f), wvp);
    output.TexCoord = input.TexCoord;
    output.Color = input.Color;

    return output;
}
