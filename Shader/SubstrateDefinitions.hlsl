#ifndef SUBSTRATE_DEFINITIONS_HLSL
#define SUBSTRATE_DEFINITIONS_HLSL

#include "Constant.hlsl"

// =============================================================
//  SubstrateDefinitions
//  SubstrateDefinitions.ush 相当。Substrate Slab BSDF の型定数・
//  ヘッダレイアウト・パッキング補助をまとめる。
//
//  デファードでは Slab を SubstrateMaterial0/1 (RGBA32_UINT x 2,
//  SV_TARGET3/4) にパックする。Substrate.MaterialTextureArray
//  (uint スロット列) 相当の固定 8 スロット簡易版:
//    [0] ヘッダ (BSDF 種別 / SSSType / フラグ)
//    [1] F0.rgb (unorm8) | SSSMFPScale (unorm8)
//    [2] F90.rgb (unorm8) | SSSPhaseAnisotropy (snorm8)
//    [3] SSSMFP.rg (f16 x 2) [m]
//    [4] SSSMFP.b (f16) | Thickness (f16) [m]
//    [5] EmissiveColor (R9G9B9E5 共有指数)
//    [6] FuzzColor.rgb (unorm8) | FuzzAmount (unorm8)
//    [7] FuzzRoughness | SecondRoughness | SecondRoughnessWeight |
//        Anisotropy (snorm8)
//  DiffuseAlbedo は GBufferC、Roughness / AO は GBufferB、法線は
//  GBufferA を共用する (レガシー経路と同居)。
// =============================================================

// ---- BSDF 種別 (ヘッダ bit0-3。0 = 非 Substrate ピクセル) ----
#define SUBSTRATE_BSDF_TYPE_NONE 0
#define SUBSTRATE_BSDF_TYPE_SLAB 1

// スラブ厚の下限 [cm]。
// MFP 導出 (PS 側)・GetSubstrateSlabBSDF 内部・アンパック側の
// 全クランプをこの 1 定数に統一する。床が食い違うと
// τ = Thickness / MFP の分子と分母が別の値で下げ止まり、
// 極小 Thickness で本来厳密な相殺 (τ = -log T / MFPScale) が
// 壊れて見た目が不連続に変化する。
#define SUBSTRATE_MIN_THICKNESS_CM 0.001f

// ---- Sub-Surface Type (Slab の SubSurface Type と 1:1) ----
//   NONE              : 散乱なし (標準 Lambert)
//   WRAP              : ラップライティング (レガシー Subsurface 相当)
//   TWO_SIDED_WRAP    : 両面ラップ (レガシー Two Sided Foliage 相当)
//   DIFFUSION         : スクリーン空間拡散。拡散パス非対応環境では
//                       仕様どおり非散乱ディフューズへフォールバック
//   DIFFUSION_PROFILE : プロファイル版 (同上フォールバック)
//   SIMPLEVOLUME      : 透過 = Beer-Lambert / 散乱 = 単散乱スラブ近似
#define SUBSTRATE_SSS_TYPE_NONE              0
#define SUBSTRATE_SSS_TYPE_WRAP              1
#define SUBSTRATE_SSS_TYPE_TWO_SIDED_WRAP    2
#define SUBSTRATE_SSS_TYPE_DIFFUSION         3
#define SUBSTRATE_SSS_TYPE_DIFFUSION_PROFILE 4
#define SUBSTRATE_SSS_TYPE_SIMPLEVOLUME      5
#define SUBSTRATE_SSS_TYPE_COUNT             6

// ---- ヘッダビットレイアウト (SubstrateMaterial0.x) ----
#define SUBSTRATE_HEADER_BSDF_TYPE_MASK      0xFu        // bit0-3  : BSDF 種別
#define SUBSTRATE_HEADER_SSS_TYPE_SHIFT      4           // bit4-6  : SSSType
#define SUBSTRATE_HEADER_SSS_TYPE_MASK       0x7u
#define SUBSTRATE_HEADER_FLAG_ISTHIN         (1u << 7)   // Is Thin Surface
#define SUBSTRATE_HEADER_FLAG_HASSSS         (1u << 8)   // SSSType != NONE
#define SUBSTRATE_HEADER_FLAG_HASFUZZ        (1u << 9)   // FuzzAmount > 0
#define SUBSTRATE_HEADER_FLAG_HASSECONDROUGH (1u << 10)  // SecondRoughnessWeight > 0
#define SUBSTRATE_HEADER_FLAG_HASEMISSIVE    (1u << 11)  // Emissive != 0
#define SUBSTRATE_HEADER_FLAG_HASANISOTROPY  (1u << 12)  // Anisotropy != 0

// ---- レイヤー既定値 (最下層スラブの暗黙厚は 0.01 cm) ----
#define SUBSTRATE_LAYER_DEFAULT_THICKNESS_CM 0.01f

// F0 がこの値を下回ると F90 は黒へフェードする 
#define SUBSTRATE_MIN_F0_FOR_F90 0.02f

// SharedLocalBases (API パリティ用。本エンジンは単一基底)
#define SHAREDLOCALBASIS_INDEX_0   0
#define SHAREDLOCALBASIS_INDEX_0_0 0
#define SHAREDLOCALBASIS_INDEX_0_1 0

// =============================================================
//  ビットパッキング補助
// =============================================================

uint SubstratePackUnorm8(float Value)
{
    return (uint)(saturate(Value) * 255.0f + 0.5f);
}

float SubstrateUnpackUnorm8(uint Value)
{
    return (float)(Value & 0xFFu) / 255.0f;
}

// [-1,1] -> unorm8
uint SubstratePackSnorm8(float Value)
{
    return SubstratePackUnorm8(clamp(Value, -1.0f, 1.0f) * 0.5f + 0.5f);
}

float SubstrateUnpackSnorm8(uint Value)
{
    return SubstrateUnpackUnorm8(Value) * 2.0f - 1.0f;
}

// rgb (unorm8 x 3) + a (unorm8) -> uint
uint SubstratePackColorAndScalar(float3 Color, float Scalar)
{
    return SubstratePackUnorm8(Color.r)
         | (SubstratePackUnorm8(Color.g) << 8)
         | (SubstratePackUnorm8(Color.b) << 16)
         | (SubstratePackUnorm8(Scalar) << 24);
}

float4 SubstrateUnpackColorAndScalar(uint Packed)
{
    return float4(
        SubstrateUnpackUnorm8(Packed),
        SubstrateUnpackUnorm8(Packed >> 8),
        SubstrateUnpackUnorm8(Packed >> 16),
        SubstrateUnpackUnorm8(Packed >> 24));
}

// f16 x 2 -> uint
uint SubstratePackHalf2(float A, float B)
{
    return f32tof16(A) | (f32tof16(B) << 16);
}

float2 SubstrateUnpackHalf2(uint Packed)
{
    return float2(f16tof32(Packed), f16tof32(Packed >> 16));
}

// ---- R9G9B9E5 共有指数 (エミッシブ HDR 用) ----
uint SubstratePackR9G9B9E5(float3 Color)
{
    // 最大表現値 = (511/512) * 2^15
    const float MaxValue = 65408.0f;
    float3 c = clamp(Color, 0.0f, MaxValue);

    float MaxChannel = max(max(c.r, c.g), c.b);

    // 共有指数: mantissa 9bit / exponent bias 15
    int Exponent = clamp((int)ceil(log2(max(MaxChannel, 1e-8f))), -15, 16);
    float Scale = exp2((float)(9 - Exponent));

    uint3 Mantissa = (uint3)(c * Scale + 0.5f);

    // 丸めで 512 に達したら指数を 1 上げる (上限 16 で飽和)
    if (max(max(Mantissa.r, Mantissa.g), Mantissa.b) > 511u)
    {
        Exponent = min(Exponent + 1, 16);
        Scale = exp2((float)(9 - Exponent));
        Mantissa = (uint3)(c * Scale + 0.5f);
    }
    Mantissa = min(Mantissa, 511u);

    uint BiasedExponent = (uint)(Exponent + 15);
    return Mantissa.r | (Mantissa.g << 9) | (Mantissa.b << 18) | (BiasedExponent << 27);
}

float3 SubstrateUnpackR9G9B9E5(uint Packed)
{
    int Exponent = (int)(Packed >> 27) - 15;
    float Scale = exp2((float)(Exponent - 9));
    return float3(
        (float)(Packed & 0x1FFu),
        (float)((Packed >> 9) & 0x1FFu),
        (float)((Packed >> 18) & 0x1FFu)) * Scale;
}

#endif
