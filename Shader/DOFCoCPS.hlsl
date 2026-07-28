#include "PostProcess_Utility.hlsl"

// =============================================================
//  Depth of Field : CoC / Prep パス (Gaussian)
//
//  フル解像度の HDR SceneColor(t0) と 線形深度(t5, R=view距離) を読み、
//  ハーフ解像度ターゲットへ以下を書き出す:
//    RGB = color * |CoC|   … プリマルチプライ済みブラー色
//    A   = |CoC|           … 正規化 CoC 重み (0=シャープ, 1=最大ボケ)
//
//  プリマルチプライにより、後段のガウスブラーで「ボケていない画素の色が
//  ボケ領域へにじむ (color bleeding)」のを防ぐ。合成時に A で正規化する。
//
//  CoC 符号:  view距離 < 焦点 → 手前 (near)、 > 焦点 → 奥 (far)。
// =============================================================

// ビュー距離から符号付き CoC (-1..+1) を求める純粋関数。
//   負 = 手前ボケ / 正 = 奥ボケ / 0 = シャープ帯
float ComputeSignedCoC(float viewDepth)
{
    float focal   = PostProcess.FocalDistance;
    float halfReg = PostProcess.FocalRegion * 0.5f;

    // 焦点面を中心にしたシャープ帯の前後の縁。
    float nearEdge = focal - halfReg;
    float farEdge  = focal + halfReg;

    float coc = 0.0f;

    if (viewDepth < nearEdge)
    {
        // 手前側: nearEdge から NearTransitionRange かけて -1 へ。
        float range = max(PostProcess.NearTransitionRange, 1e-3f);
        coc = -saturate((nearEdge - viewDepth) / range);
        coc *= PostProcess.NearBlurScale;
    }
    else if (viewDepth > farEdge)
    {
        // 奥側: farEdge から FarTransitionRange かけて +1 へ。
        float range = max(PostProcess.FarTransitionRange, 1e-3f);
        coc = saturate((viewDepth - farEdge) / range);
        coc *= PostProcess.FarBlurScale;
    }

    return clamp(coc, -1.0f, 1.0f);
}

PS_OUTPUT main(PS_INPUT input)
{
    PS_OUTPUT output;
    float2 uv = input.TexCoord;

    // ハーフ解像度なので、ダウンサンプル時のエイリアス低減に 4-tap 平均を取る。
    // (フル解像度の 2x2 近傍を平均して 1 テクセルへ)
    float2 t = float2(PostProcess.SceneTexelSizeX, PostProcess.SceneTexelSizeY) * 0.5f;

    float3 c0 = TextureBaseColor.Sample(Sampler2, uv + t * float2(-1, -1)).rgb;
    float3 c1 = TextureBaseColor.Sample(Sampler2, uv + t * float2( 1, -1)).rgb;
    float3 c2 = TextureBaseColor.Sample(Sampler2, uv + t * float2(-1,  1)).rgb;
    float3 c3 = TextureBaseColor.Sample(Sampler2, uv + t * float2( 1,  1)).rgb;
    float3 color = (c0 + c1 + c2 + c3) * 0.25f;

    // 深度は中心 1-tap で十分 (CoC は連続量、近傍平均は輪郭を鈍らせる)。
    float viewDepth = TextureLinearDepth.Sample(Sampler2, uv).r;

    float signedCoC = ComputeSignedCoC(viewDepth);
    float absCoC    = abs(signedCoC);

    // プリマルチプライ + CoC 重みを A に格納。
    output.Color = float4(color * absCoC, absCoC);
    return output;
}
