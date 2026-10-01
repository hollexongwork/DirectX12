#ifndef POSTPROCESS_UPSCALE_HLSL
#define POSTPROCESS_UPSCALE_HLSL

// =============================================================
//  PostProcessUpscale.hlsl  (UE PostProcessUpscale.usf, r.Upscale.Quality)
//  一次空間アップスケール: トーンマップ済み LDR (ポスト解像度 P, t0) ->
//  バックバッファ (出力解像度 O)。UE と同じくトーンマップの後に走る
//  (表示参照の LDR を拡大する)。
//
//  ラッパ (PostProcessUpscale_<Name>_PS.hlsl) が UPSCALE_METHOD を定義してから include する:
//    0 Nearest     : 最近傍 (Load)
//    1 Bilinear    : バイリニア 1 タップ
//    2 Directional : 4 タップの輝度勾配から等輝度方向を求めて 2 タップ + アンシャープ [M]
//    3 CatmullRom  : 5 タップ Catmull-Rom (角除去, 既定)
//    4 Lanczos     : Lanczos-3、中央ペアをバイリニアで合成した 5x5 格子の菱形 13 タップ [M]
//    5 Gaussian    : 最大 36 タップのガウシアン (sigma^2 = 0.5) + ラプラシアンによるアンシャープ [L]
//
//  入力:  t0 = TextureBaseColor (トーンマップ出力 RGBA8, PSR)
//         b4 = SceneTexelSize (= 1 / 入力解像度), UpscaleUnsharpAmount (mode 5)
//  UV:    exact-size なので出力ビューポート UV = 入力 UV (input.TexCoord)
//  出力:  float4(saturate(rgb), 1)
// =============================================================

#ifndef UPSCALE_METHOD
#error "UPSCALE_METHOD (0..5) を定義してから PostProcessUpscale.hlsl を include すること"
#endif

#include "Common.hlsl"
#include "TemporalAACommon.hlsl"   // CatmullRom5Taps

#define UPSCALE_METHOD_NEAREST     0
#define UPSCALE_METHOD_BILINEAR    1
#define UPSCALE_METHOD_DIRECTIONAL 2
#define UPSCALE_METHOD_CATMULLROM  3
#define UPSCALE_METHOD_LANCZOS     4
#define UPSCALE_METHOD_GAUSSIAN    5

// 入力サイズ (W, H, 1/W, 1/H)。SceneTexelSize = 1/入力解像度 はレンダラが設定する。
// W, H は 1/(1/W) の丸め誤差で 1 小さい整数に切り捨てられないよう最近接整数へ丸める
float4 GetUpscaleInputSize()
{
    const float2 InvSize = float2(PostProcess.SceneTexelSizeX, PostProcess.SceneTexelSizeY);
    return float4(round(1.0f / InvSize), InvSize);
}

// バイリニア 1 タップ (s1 = LINEAR, CLAMP。範囲外 UV は端のテクセル)
float3 SampleUpscaleInput(float2 UV)
{
    return TextureBaseColor.SampleLevel(Sampler2, UV, 0.0f).rgb;
}

// テクセル整数座標で読む (範囲外は端へクランプ)
float3 LoadUpscaleInput(int2 Pixel, float4 InSize)
{
    const int2 MaxPixel = int2(InSize.xy) - 1;
    return TextureBaseColor.Load(int3(clamp(Pixel, int2(0, 0), MaxPixel), 0)).rgb;
}

#if UPSCALE_METHOD == UPSCALE_METHOD_LANCZOS
// Lanczos-3: sinc(x) * sinc(x / 3) (|x| < 3)。定数倍は正規化で相殺される
// (早期 return を使わない単一の return: /Od の X4000 を避ける)
float Lanczos3(float x)
{
    x = abs(x);
    const float px = PI * max(x, 1e-5f);                        // x = 0 の 0 除算を避ける (極限値 1 は下で選ぶ)
    const float w = (sin(px) / px) * (sin(px / 3.0f) / (px / 3.0f));
    return (x < 1e-5f) ? 1.0f : ((x < 3.0f) ? w : 0.0f);
}

// 1 軸分のタップ位置 (入力 px 座標) と重み。中央 2 タップ (tc, tc+1) はバイリニアで 1 タップへ合成
//   位置 = { tc-2, tc-1, S2, tc+2, tc+3 }, 重み = { w0, w1, w2+w3, w4, w5 }
void LanczosAxisTaps(float Pix, out float Pos[5], out float W[5])
{
    const float tc = floor(Pix - 0.5f) + 0.5f;   // Pix の左 (上) 隣のテクセル中心
    const float f = Pix - tc + 2.0f;              // tc-2 のタップから Pix までの距離
    float w[6];
    [unroll] for (int k = 0; k < 6; ++k) { w[k] = Lanczos3(f - (float)k); }
    const float W2 = w[2] + w[3];                 // 中央の 2 タップ (frac in [0,1) で常に正)
    Pos[0] = tc - 2.0f; W[0] = w[0];
    Pos[1] = tc - 1.0f; W[1] = w[1];
    Pos[2] = tc + w[3] / W2; W[2] = W2;
    Pos[3] = tc + 2.0f; W[3] = w[4];
    Pos[4] = tc + 3.0f; W[4] = w[5];
}
#endif

PS_OUTPUT main(PS_INPUT input)
{
    PS_OUTPUT output;
    const float2 uv = input.TexCoord;
    const float4 InSize = GetUpscaleInputSize();
    float3 rgb;

#if UPSCALE_METHOD == UPSCALE_METHOD_NEAREST
    // ---- 0: 最近傍 ----
    rgb = TextureBaseColor.Load(int3(min(int2(uv * InSize.xy), int2(InSize.xy) - 1), 0)).rgb;

#elif UPSCALE_METHOD == UPSCALE_METHOD_BILINEAR
    // ---- 1: バイリニア ----
    rgb = SampleUpscaleInput(uv);

#elif UPSCALE_METHOD == UPSCALE_METHOD_DIRECTIONAL
    // ---- 2: 方向性ブラー + アンシャープマスク (UE の Directional blur with unsharp mask) [M] ----
    const float2 t = 0.5f * InSize.zw;
    const float3 ColorNW = SampleUpscaleInput(uv + float2(-t.x, -t.y));
    const float3 ColorNE = SampleUpscaleInput(uv + float2( t.x, -t.y));
    const float3 ColorSW = SampleUpscaleInput(uv + float2(-t.x,  t.y));
    const float3 ColorSE = SampleUpscaleInput(uv + float2( t.x,  t.y));
    const float3 C = (ColorNW + ColorNE + ColorSW + ColorSE) * 0.25f;

    const float3 LumaWeights = float3(0.299f, 0.587f, 0.114f);
    const float LumaNW = dot(ColorNW, LumaWeights);
    const float LumaNE = dot(ColorNE, LumaWeights);
    const float LumaSW = dot(ColorSW, LumaWeights);
    const float LumaSE = dot(ColorSE, LumaWeights);
    const float dSWmNE = LumaSW - LumaNE;
    const float dSEmNW = LumaSE - LumaNW;
    // 等輝度方向 (勾配に直交)。ゼロベクトルで NaN にならないよう 6e-8 を足す
    float2 Dir = float2(dSWmNE + dSEmNW, dSWmNE - dSEmNW);
    Dir *= 0.125f * rsqrt(dot(Dir, Dir) + 6e-8f);
    const float3 ColorN = SampleUpscaleInput(uv - Dir * InSize.zw);
    const float3 ColorP = SampleUpscaleInput(uv + Dir * InSize.zw);
    const float UnsharpMask = 0.25f;
    rgb = (ColorN + ColorP) * ((UnsharpMask + 1.0f) * 0.5f) - C * UnsharpMask;

#elif UPSCALE_METHOD == UPSCALE_METHOD_CATMULLROM
    // ---- 3: 5 タップ Catmull-Rom (既定) ----
    float2 TapUV[5];
    float  TapW[5];
    CatmullRom5Taps(uv, InSize, TapUV, TapW);
    // 端のテクセル中心より外を読まない (CLAMP でも合成タップの重みが崩れないよう手動クランプ)
    const float2 UVMin = 0.5f * InSize.zw;
    const float2 UVMax = 1.0f - 0.5f * InSize.zw;
    float3 Acc = 0.0f;
    float  WeightSum = 0.0f;
    [unroll] for (int i = 0; i < 5; ++i)
    {
        Acc += SampleUpscaleInput(clamp(TapUV[i], UVMin, UVMax)) * TapW[i];
        WeightSum += TapW[i];
    }
    rgb = Acc / WeightSum;

#elif UPSCALE_METHOD == UPSCALE_METHOD_LANCZOS
    // ---- 4: Lanczos-3、菱形 13 タップ (|i-2| + |j-2| <= 2) [M] ----
    const float2 Pix = uv * InSize.xy;
    float PosX[5], WX[5], PosY[5], WY[5];
    LanczosAxisTaps(Pix.x, PosX, WX);
    LanczosAxisTaps(Pix.y, PosY, WY);
    float3 Acc = 0.0f;
    float  WeightSum = 0.0f;
    [unroll] for (int j = 0; j < 5; ++j)
    {
        [unroll] for (int i = 0; i < 5; ++i)
        {
            if (abs(i - 2) + abs(j - 2) > 2) continue;   // 展開時に定数で除去される
            const float w = WX[i] * WY[j];
            Acc += SampleUpscaleInput(float2(PosX[i], PosY[j]) * InSize.zw) * w;
            WeightSum += w;
        }
    }
    rgb = Acc / WeightSum;

#elif UPSCALE_METHOD == UPSCALE_METHOD_GAUSSIAN
    // ---- 5: ガウシアン (sigma^2 = 0.5, 半径 3 px) + ラプラシアンのアンシャープ [L] ----
    //  Out = Acc/W - 0.5 * UpscaleUnsharpAmount * (Lap/W)
    //  Lap は w * (2 r^2 - 2) = (1/2) * Laplacian(exp(-r^2)) による畳み込み
    const float2 Pix = uv * InSize.xy;
    const float2 First = floor(Pix - 0.5f) - 2.0f;
    float3 Acc = 0.0f;
    float3 Lap = 0.0f;
    float  WeightSum = 0.0f;
    [unroll] for (int l = 0; l < 6; ++l)
    {
        [unroll] for (int k = 0; k < 6; ++k)
        {
            const float2 Center = First + float2((float)k, (float)l) + 0.5f;
            const float2 o = Pix - Center;
            const float r2 = dot(o, o);
            const float w = (r2 > 9.0f) ? 0.0f : exp(-r2);
            const float3 s = LoadUpscaleInput(int2(First) + int2(k, l), InSize);
            Acc += w * s;
            Lap += w * s * (2.0f * r2 - 2.0f);
            WeightSum += w;
        }
    }
    rgb = Acc / WeightSum - 0.5f * PostProcess.UpscaleUnsharpAmount * (Lap / WeightSum);

#else
#error "UPSCALE_METHOD は 0..5"
#endif

    output.Color = float4(saturate(rgb), 1.0f);
    return output;
}

#endif
