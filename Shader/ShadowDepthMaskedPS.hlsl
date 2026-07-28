#include "Common.hlsl"

// =============================================================
//  ShadowDepthMaskedPS
//  Masked (BLEND_Masked) マテリアル用シャドウ深度 PS。
//  ベースパス (GeometryPS) と同じ OpacityMask 判定
//  (BaseColor テクスチャ α x 頂点カラー α vs OpacityMaskClipValue)
//  で不合格テクセルの深度書き込みを破棄する。
//  カラー出力なし (RTV 0 枚の深度専用 PSO)。
//  描画側 (DrawShadowDepth) が t0 (BaseColor) と b2 (Material) を
//  バインドすること。
// =============================================================

void main(float4 Position : SV_POSITION, float2 TexCoord : TEXCOORD, float4 Color : COLOR)
{
    float opacityMask = TextureBaseColor.Sample(Sampler, TexCoord).a * Color.a;
    clip(opacityMask - Material.OpacityMaskClipValue);
}
