#include "TemporalAACommon.hlsl"

// =============================================================
//  TemporalAAMitchellNetravali_CS (§6.6, [L])
//  MainSuperSampling (r.TemporalAA.HistoryScreenPercentage > 100) の後段ダウンサンプル。
//  TAA が書いた拡大履歴 H (= 入力) を Mitchell-Netravali (B = C = 1/3) で S (= 出力) へ戻す。
//  UE FDefaultTemporalUpscaler::AddPasses の ComputeMitchellNetravaliDownsample に相当。
//    ・分離可能な 2D カーネル。台は出力 2 px (= 入力 2 x InputPerOutputPixel px)
//    ・入力比 InputPerOutputPixel = H / S は 1..2 なので 1 軸 9 タップで足りる
//      (台の開区間の長さ 4 x 比 <= 8 入力 px)
//    ・重みは正規化する (1D の生の総和 = 比)。負ローブ (最小 -0.035) による負値 / NaN は
//      SanitizeColor でガードする
//  コンピュートルートシグネチャは TAA と共通 (§3.7): b0 = この cbuffer (定数リングのスロット 1),
//  t0 = 入力 (履歴 H, RD), u0 = 出力 (S)。t1..t5 / u1 / u2 はダミー (読まない)。
// =============================================================

cbuffer MitchellNetravaliParameters : register(b0)   // C++ FMitchellNetravaliParameters (§3.5, 48 B)
{
    float4 InputSize;             // (H.x, H.y, 1/H.x, 1/H.y)
    float4 OutputSize;            // (S.x, S.y, 1/S.x, 1/S.y)
    float2 InputPerOutputPixel;   // (H.x/S.x, H.y/S.y) (1..2)
    float2 Pad;
};
Texture2D<float4>   InputTexture  : register(t0);
RWTexture2D<float4> OutputTexture : register(u0);

[numthreads(TAA_TILE_SIZE, TAA_TILE_SIZE, 1)]
void main(uint2 DTid : SV_DispatchThreadID)
{
    if (any(DTid >= (uint2)OutputSize.xy)) return;                            // LDS 不使用なので早期 return 可
    const float2 InPos  = (float2(DTid) + 0.5f) * InputPerOutputPixel;       // 入力 (履歴) の連続座標
    const float2 Radius = 2.0f * InputPerOutputPixel;                          // 台 = 出力 2 px
    const int2   First  = int2(floor(InPos - Radius - 0.5f)) + 1;              // 中心 (i + 0.5) > InPos - Radius
    float wx[9], wy[9];
    [unroll] for (int k = 0; k < 9; ++k)
    {
        wx[k] = MitchellNetravali(((float)(First.x + k) + 0.5f - InPos.x) / InputPerOutputPixel.x);
        wy[k] = MitchellNetravali(((float)(First.y + k) + 0.5f - InPos.y) / InputPerOutputPixel.y);
    }
    float3 acc = 0.0f;
    float wsum = 0.0f;
    [loop] for (int y = 0; y < 9; ++y)
    {
        [unroll] for (int x = 0; x < 9; ++x)
        {
            const float w = wx[x] * wy[y];
            if (w == 0.0f) continue;
            const int2 p = clamp(First + int2(x, y), int2(0, 0), int2(InputSize.xy) - 1);   // 端は複製 (クランプ)
            acc += InputTexture.Load(int3(p, 0)).rgb * w;
            wsum += w;
        }
    }
    const float3 c = SanitizeColor(acc / wsum, 65504.0f);                      // 負ローブ (-0.035) と NaN のガード
    OutputTexture[DTid] = float4(c, 1.0f);
}
