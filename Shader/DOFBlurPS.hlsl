#include "PostProcess_Utility.hlsl"

// =============================================================
//  Depth of Field : 分離ガウスブラー (水平 / 垂直 兼用)
//
//  入力 t0 : プリマルチプライ済み (RGB = color*|CoC|, A = |CoC|)。
//  出力    : 同一レイアウト (次のパスへ引き渡す)。
//
//  方向は PostProcess.DofPad で分岐:
//    0.0 → 水平 (X), 1.0 → 垂直 (Y)
//
//  ブラー半径は「中心画素の CoC」× MaxBlurSize (ハーフ解像度テクセル単位)。
//  Gaussian と同様、CoC が大きいほど広いカーネルでサンプルする。
//  プリマルチプライ済みなので RGB と A を同じ重みで畳み込み、シャープ画素
//  (A≈0) はボケ領域へ寄与しない。
//
//  9-tap の重み付きガウス。半径は連続的にスケールする (テクセル間はハード
//  ウェアバイリニアで補間)。
// =============================================================

// 正規化済み 9-tap ガウス重み (σ ≈ 2)。中心 + 左右対称 4 対。
static const int   DOF_TAPS = 9;
static const float DOF_OFFSET[DOF_TAPS] =
{
    -4.0f, -3.0f, -2.0f, -1.0f, 0.0f, 1.0f, 2.0f, 3.0f, 4.0f
};
static const float DOF_WEIGHT[DOF_TAPS] =
{
    0.028532f, 0.067234f, 0.124009f, 0.179044f, 0.202360f,
    0.179044f, 0.124009f, 0.067234f, 0.028532f
};

PS_OUTPUT main(PS_INPUT input)
{
    PS_OUTPUT output;
    float2 uv = input.TexCoord;

    float2 texel = float2(PostProcess.SceneTexelSizeX, PostProcess.SceneTexelSizeY);

    // 方向ベクトル (水平 or 垂直)。
    float2 dir = (PostProcess.DofPad < 0.5f)
        ? float2(texel.x, 0.0f)
        : float2(0.0f, texel.y);

    // 中心画素の CoC (= A) からブラー半径を決める。
    float centerCoC = TextureBaseColor.Sample(Sampler2, uv).a;
    float radius = centerCoC * PostProcess.MaxBlurSize;

    // CoC が実質ゼロなら畳み込みを省略してそのまま通す (シャープ保持)。
    if (radius < 0.5f)
    {
        output.Color = TextureBaseColor.Sample(Sampler2, uv);
        return output;
    }

    float4 sum = float4(0, 0, 0, 0);
    [unroll]
    for (int i = 0; i < DOF_TAPS; ++i)
    {
        float2 offset = dir * (DOF_OFFSET[i] * radius / 4.0f);
        sum += TextureBaseColor.Sample(Sampler2, uv + offset) * DOF_WEIGHT[i];
    }

    output.Color = sum;
    return output;
}
