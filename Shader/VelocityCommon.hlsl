#ifndef VELOCITY_COMMON_HLSL
#define VELOCITY_COMMON_HLSL

// =============================================================
//  VelocityCommon.hlsl
//  UE VelocityCommon.ush (Gen4: xy のみ) 相当。レジスタを宣言しない
//  (コンピュートシェーダ / 自己テストからも include できる)。
//
//  ベロシティ V = s_cur(ジッタ無し) - s_prev(ジッタ無し) [ScreenPos = NDC.xy 単位, +y 上]。
//  R16G16_UNORM へ V * 0.2495 + 32767/65535 で格納する。
//  0 (クリア値) = 未書き込み -> TAA / 可視化は深度からカメラモーションを再構築する。
//  C++ 側の鏡像 (自己テスト T6) は VelocityRendering.cpp の EncodeVelocityToTextureCPU。
// =============================================================

static const float VELOCITY_ENCODE_SCALE = 0.499f * 0.5f;           // 0.2495
static const float VELOCITY_ENCODE_BIAS  = 32767.0f / 65535.0f;     // 0.49999237

float2 EncodeVelocityToTexture(float2 V)
{
    V = clamp(V, -2.0f, 2.0f);                   // [PORT] |V| > 2.0038 は UNORM 0 (未書き込み) と衝突するため
    return V * VELOCITY_ENCODE_SCALE + VELOCITY_ENCODE_BIAS;
}

float2 DecodeVelocityFromTexture(float2 E)
{
    return (E - VELOCITY_ENCODE_BIAS) * (1.0f / VELOCITY_ENCODE_SCALE);
}

bool IsVelocityWritten(float2 E)
{
    return E.x > 0.0f;                           // UE: EncodedVelocity.x > 0
}

// ---- VelocityVS -> VelocityPS / VelocityMaskedPS ----
struct VELOCITY_VS_OUTPUT
{
    float4 Position        : SV_POSITION;   // 今フレーム clip (ジッタ込み)。ベースパスと同一式
    float4 PackedVelocityA : TEXCOORD1;     // 今フレーム clip (ジッタ込み, = Position)
    float4 PackedVelocityC : TEXCOORD2;     // 前フレーム clip (前フレームのジッタ込み)
    float2 TexCoord        : TEXCOORD0;
    float4 Color           : COLOR;
};

#endif
