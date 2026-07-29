#include "Common.hlsl"

// 深度バッファ(非線形 [0,1])を線形化して書き出すパス
// 出力フォーマットは R32G32_FLOAT を想定
//   R = ViewLinearDepth : ビュー空間の実距離（DepthFade等のエフェクト用）
//   G = NormalizedDepth  : (実距離 - Near) / (Far - Near) を [0,1] に正規化（表示用）

PS_OUTPUT main(PS_INPUT input)
{
    PS_OUTPUT output;

    float zNear = NearFar.x;
    float zFar = NearFar.y;

    // 非線形深度 [0,1] をサンプル
    float depth = TextureDepth.Sample(Sampler2, input.TexCoord).r;

    // ビュー空間の実距離を復元（XMMatrixPerspectiveFovLH, Depth∈[0,1]）
    //   depth = zFar / (zFar - zNear) * (1 - zNear / viewZ)  の逆算
    float viewZ = (zNear * zFar) / (zFar - depth * (zFar - zNear));

    // [0,1] 正規化（表示用）
    float normalized = saturate((viewZ - zNear) / (zFar - zNear) * 20.0f);

    output.Color = float4(viewZ, normalized , 0.0f, 1.0f);

    return output;
}
