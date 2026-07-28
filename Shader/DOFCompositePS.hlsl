#include "PostProcess_Utility.hlsl"

// =============================================================
//  Depth of Field : 合成パス
//
//  入力:
//    t0  = シャープ HDR SceneColor (フル解像度)
//    t5  = 線形深度 (R = view距離)
//    t13 = ハーフ解像度 DOF ブラー (RGB = color*|CoC|, A = |CoC|)
//
//  ブラーはプリマルチプライ済みなので、A で割って実色へ戻す。
//  合成係数は「その画素の CoC」を smoothstep でならしたもの。焦点面付近で
//  シャープ、CoC が大きいほどブラー色へ寄せる (Gaussian の最終合成)。
//
//  ハーフ解像度ブラーはバイリニア拡大でアップサンプルされる (Sampler2)。
// =============================================================

// CoCPS と同じ符号付き CoC 計算 (合成重み用に再計算)。
float ComputeAbsCoC(float viewDepth)
{
    float focal   = PostProcess.FocalDistance;
    float halfReg = PostProcess.FocalRegion * 0.5f;
    float nearEdge = focal - halfReg;
    float farEdge  = focal + halfReg;

    float coc = 0.0f;
    if (viewDepth < nearEdge)
    {
        float range = max(PostProcess.NearTransitionRange, 1e-3f);
        coc = saturate((nearEdge - viewDepth) / range) * PostProcess.NearBlurScale;
    }
    else if (viewDepth > farEdge)
    {
        float range = max(PostProcess.FarTransitionRange, 1e-3f);
        coc = saturate((viewDepth - farEdge) / range) * PostProcess.FarBlurScale;
    }
    return saturate(coc);
}

PS_OUTPUT main(PS_INPUT input)
{
    PS_OUTPUT output;
    float2 uv = input.TexCoord;

    float3 sharp = TextureBaseColor.Sample(Sampler2, uv).rgb;

    // ブラー (プリマルチプライ) を取得し、A で un-premultiply。
    float4 blurPacked = TextureDOFBlur.Sample(Sampler2, uv);
    float  blurCoC    = blurPacked.a;
    float3 blurred    = blurPacked.rgb / max(blurCoC, 1e-4f);

    // フル解像度の深度から合成重みを再計算 (ハーフ解像度の A よりエッジが精細)。
    float viewDepth = TextureLinearDepth.Sample(Sampler2, uv).r;
    float coc = ComputeAbsCoC(viewDepth);

    // ソフトな遷移 (焦点境界のリンギング回避)。
    float w = smoothstep(0.0f, 1.0f, coc);

    // ブラー領域が実際にボケ色を持つ場合のみ寄せる (CoC=0 の背景保持)。
    float3 color = lerp(sharp, blurred, w * step(1e-4f, blurCoC));

    output.Color = float4(color, 1.0f);
    return output;
}
