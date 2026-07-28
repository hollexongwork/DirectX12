#include "Grading.hlsl"

// ---- PostProcess flag (ConstantBuffers.hlsl の PP_FLAG_* と 1:1) ----
// 本 CS は独立ルートシグネチャで b0 を専有するため、レジスタを宣言する
// 共有ヘッダ (ConstantBuffers.hlsl 等) は include しない。
#define PP_FLAG_WHITE_BALANCE  (1u << 5)
#define PP_FLAG_ARTIST_LUT     (1u << 6)

// -------------------------------------------------------------
//  b0 : GradingParams (C++ GRADING_PARAMS 構造体と 1:1)
// -------------------------------------------------------------
cbuffer GradingParams : register(b0)
{
    float4 ColorSaturation; // rgb=チャンネル別, a=全体
    float4 ColorContrast; // rgb, a=全体
    float4 ColorGamma; // rgb, a=全体
    float4 ColorGain; // rgb, a=全体
    float4 ColorOffset; // rgb=加算, a=全体
    float WhiteTemp; // Kelvin
    float WhiteTint; // -1..1
    uint LUTSize; // 33 (出力ボリュームサイズ)
    uint Flags; // PP_FLAG_*

    float ArtistLUTWeight; // 0=手続きのみ, 1=完全にアーティスト LUT
    float ArtistLUTTileSize; // アーティスト LUT キューブの辺長 (16, 32 等)
    float ArtistLUTPixelsX; // ストリップ幅  (tileSize * tileSize)
    float ArtistLUTPixelsY; // ストリップ高  (tileSize)
};

RWTexture3D<float4> LUTOut : register(u0);
Texture2D<float4> ArtistLUT : register(t0); // 2D ストリップ LUT (任意)
SamplerState LUTSampler : register(s0); // linear clamp

// -------------------------------------------------------------
//  2D ストリップ LUT ルックアップ (本 CS 固有)
//  レイアウト (LUT_Adventure.DDS, 256x16 -> 16^3 で検証済):
//    X 方向のタイル index = RED, タイル内 X = BLUE, タイル内 Y = GREEN。
//    pixel.x = red * N + blue,  pixel.y = green。
//  RED 隣接 2 タイルを補間して trilinear 相当にする。
// -------------------------------------------------------------
float3 SampleArtistLUT(float3 c)
{
    float N = ArtistLUTTileSize;
    float3 col = saturate(c);

    // blue でどのタイル (0..N-1) を選ぶか
    float blue = col.b * (N - 1.0f);
    float bLo = floor(blue);
    float bHi = min(bLo + 1.0f, N - 1.0f);
    float bFrac = blue - bLo;

    // タイル内 UV : red = X, green = Y (ハーフテクセル補正)
    float u = (col.r * (N - 1.0f) + 0.5f) / ArtistLUTPixelsX;
    float v = (col.g * (N - 1.0f) + 0.5f) / ArtistLUTPixelsY;

    // 1 タイルの水平幅 (ストリップ全体で N ピクセル)
    float tileU = N / ArtistLUTPixelsX;

    float2 uvLo = float2(u + bLo * tileU, v);
    float2 uvHi = float2(u + bHi * tileU, v);

    float3 lo = ArtistLUT.SampleLevel(LUTSampler, uvLo, 0).rgb;
    float3 hi = ArtistLUT.SampleLevel(LUTSampler, uvHi, 0).rgb;
    return lerp(lo, hi, bFrac);
}

[numthreads(4, 4, 4)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= LUTSize || id.y >= LUTSize || id.z >= LUTSize)
        return;

    // voxel 中心 -> [0,1] のニュートラル入力色。この LUT は DISPLAY 空間
    // 変換 : トーンマッパーが ACES 出力を先に sRGB エンコード
    // してから本 LUT をサンプルする。よってニュートラル座標は display 空間
    // の色で、手続きグレード / アーティスト LUT も display 空間で動作する。
    float inv = 1.0f / (float) (LUTSize - 1);
    float3 neutral = float3(id) * inv;

    float3 c = neutral;

    // 1) white balance
    if (Flags & PP_FLAG_WHITE_BALANCE)
        c *= WhiteBalanceScale(WhiteTemp, WhiteTint);

    // 2) 手続きグレーディング (Grading.hlsl 共通実装)
    c = ColorGradeApply(c, ColorSaturation, ColorContrast,
                        ColorGamma, ColorGain, ColorOffset);
    c = saturate(c);

    // 3) アーティスト LUT 統合: 手続きグレード後の色に重ねて
    //    ArtistLUTWeight でブレンド。
    if (Flags & PP_FLAG_ARTIST_LUT)
    {
        float3 artist = SampleArtistLUT(c);
        c = lerp(c, artist, saturate(ArtistLUTWeight));
    }

    LUTOut[id] = float4(saturate(c), 1.0f);
}
