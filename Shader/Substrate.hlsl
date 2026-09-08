#ifndef SUBSTRATE_HLSL
#define SUBSTRATE_HLSL

#include "SubstrateDefinitions.hlsl"

// =============================================================
//  Substrate
//  Substrate.ush 相当。Slab BSDF のマテリアル定義
//  (GetSubstrateSlabBSDF) と、デファード用パック / アンパック
//  (SubstrateMaterial0/1 = RGBA32_UINT x 2) を提供する。
//
//    - GetSubstrateSlabBSDF はマテリアルテンプレートの呼び出し
//      シグネチャと引数順を完全一致させている (サンプル
//      substrate_Slab_BSDF_Material_Sample.hlsl 準拠)。
//    - Glint / SpecularProfile / ClearCoatSecondNormal は API と
//      して受理するが評価は無効 (意図的乖離。MANIFEST 参照)。
//    - MFP / Thickness は UE 同様 cm でオーサリングし、本エンジン
//      はメートル単位ワールドのためメートルへ変換して保持する。
// =============================================================

// -------------------------------------------------------------
//  FSubstratePixelFootprint (API パリティ用の簡易フットプリント)
//  UE では法線/UV の微分からフィルタリングに使う。本エンジンでは
//  値は保持のみで評価には未使用。
// -------------------------------------------------------------
struct FSubstratePixelFootprint
{
    float PixelRadius;
};

FSubstratePixelFootprint GetSubstratePixelFootprint()
{
    FSubstratePixelFootprint Footprint;
    Footprint.PixelRadius = 1.0f;
    return Footprint;
}

// -------------------------------------------------------------
//  FSubstrateBSDF (Slab)
//  Slab 1 枚分の完全なパラメータセット。
// -------------------------------------------------------------
struct FSubstrateBSDF
{
    // ---- 界面 (Interface) ----
    float3 Normal; // ワールド法線 (デファードでは GBufferA 側を採用)
    float3 DiffuseAlbedo; // 拡散アルベド (既定 0.18)
    float3 F0; // 垂直入射フレネル反射率
    float3 F90; // 斜入射フレネル反射率 (F0 < 0.02 で黒へフェード)
    float Roughness; // GGX ラフネス
    float Anisotropy; // 異方性 [-1,1] (評価は等方近似: 意図的乖離)

    // ---- Sub-Surface (媒質) ----
    float3 SSSMFP; // 平均自由行程 [m] (チャンネル別)
    float SSSMFPScale; // MFP スカラースケール (本配線では中立 1.0)
    float SSSPhaseAnisotropy; // Henyey-Greenstein の g [-1,1]
    uint SSSType; // SUBSTRATE_SSS_TYPE_*
    uint SSSProfileId; // プロファイル ID (本エンジンでは未使用)

    // ---- エミッシブ ----
    float3 Emissive; // 放射輝度 [cd/m^2 相当]

    // ---- 第 2 スペキュラローブ ----
    float SecondRoughness;
    float SecondRoughnessWeight; // 0 = 第 1 ローブのみ

    // ---- ファズ (布・産毛のシーン) ----
    float FuzzAmount; // 0 = 無効
    float3 FuzzColor;
    float FuzzRoughness;

    // ---- レイヤー形状 ----
    float Thickness; // スラブ厚 [m]
    uint bIsThin; // Thin Surface (葉・紙など両面薄板)
};

// -------------------------------------------------------------
//  透過色 <-> 平均自由行程 (Beer-Lambert)
//  指定厚での垂直透過率が TransmittanceColor と一致する
//  MFP を返す。MFP == Thickness のとき透過率 36% (1/e)。
//    T = exp(-Thickness / MFP)  =>  MFP = Thickness / (-ln T)
// -------------------------------------------------------------
float3 TransmittanceToMeanFreePath(float3 TransmittanceColor, float Thickness)
{
    float3 SafeT = clamp(TransmittanceColor, 1e-5f, 1.0f - 1e-5f);
    return Thickness / max(-log(SafeT), 1e-8f);
}

float3 MeanFreePathToTransmittance(float3 MeanFreePath, float Thickness)
{
    return exp(-Thickness / max(MeanFreePath, 1e-8f));
}

// -------------------------------------------------------------
//  誘電体 F0 <-> IOR (Substrate 屈折の既定 IOR 導出に使用)
// -------------------------------------------------------------
float DielectricF0ToIor(float F0)
{
    float SqrtF0 = sqrt(clamp(F0, 0.0f, 0.99f));
    return (1.0f + SqrtF0) / (1.0f - SqrtF0);
}

float DielectricIorToF0(float Ior)
{
    float t = (Ior - 1.0f) / (Ior + 1.0f);
    return t * t;
}

// F0 RGB -> 代表スカラー (輝度荷重ではなく平均: UE の簡易系に合わせる)
float F0RGBToF0(float3 F0)
{
    return dot(F0, (1.0f / 3.0f).xxx);
}

// -------------------------------------------------------------
//  GetSubstrateSlabBSDF
//  マテリアルテンプレートと同一の引数順。未対応機能
//  (Glint / SpecularProfile / ClearCoat 第 2 法線) も API として
//  受理する (評価は無効)。
//    SSSType  : float で受ける (テンプレート互換) -> uint へ変換
//    Thickness: [cm] で受ける (UE 準拠) -> [m] へ変換して保持
// -------------------------------------------------------------
FSubstrateBSDF GetSubstrateSlabBSDF(
    FSubstratePixelFootprint PixelFootprint,
    float3 Normal,
    float3 DiffuseAlbedo,
    float3 F0,
    float3 F90,
    float Roughness,
    float Anisotropy,
    float SSSProfileId,
    bool bSupportDefaultSSSProfile,
    float3 SSSMFP,
    float SSSMFPScale,
    float SSSPhaseAniso,
    float SSSType,
    float3 EmissiveColor,
    float SecondRoughness,
    float SecondRoughnessWeight,
    float SecondRoughnessAsSimpleClearCoat,
    float ClearCoatUseSecondNormal,
    float3 ClearCoatBottomNormal,
    float FuzzAmount,
    float3 FuzzColor,
    float FuzzRoughness,
    float GlintValue,
    float2 GlintUV,
    float SpecularProfileId,
    float Thickness,
    bool IsThin,
    bool IsAtBottom,
    uint LocalBasisIndex,
    uint SharedLocalBasesTypes)
{
    FSubstrateBSDF BSDF = (FSubstrateBSDF)0;

    BSDF.Normal = Normal;
    BSDF.DiffuseAlbedo = saturate(DiffuseAlbedo);
    BSDF.F0 = saturate(F0);
    BSDF.F90 = saturate(F90);
    BSDF.Roughness = clamp(Roughness, 0.001f, 1.0f);
    BSDF.Anisotropy = clamp(Anisotropy, -1.0f, 1.0f);

    BSDF.SSSMFP = max(SSSMFP, 0.0f); // [m]
    // ---- ノード配線 (Substrate Transmittance-To-MeanFreePath) ----
    //   MFP 出力       -> SSSMFP ピン
    //   Thickness 出力 -> SSSMFPScale ピン  (SSS 評価厚 [cm] を運ぶ)
    // Slab 本体の Thickness 引数は既定レイヤー厚 (0.01cm) 固定。
    // 本実装は MFPScale ピンが運ぶ値を SSS 評価厚として BSDF.Thickness
    // に格納し (cm -> m)、スカラーの MFP スケールは中立 (1.0) とする。
    // τ = 評価厚 / MFP = -log(TransmittanceColor) となり UE と同一。
    BSDF.SSSMFPScale = 1.0f;
    BSDF.SSSPhaseAnisotropy = clamp(SSSPhaseAniso, -0.99f, 0.99f);
    BSDF.SSSType = min((uint)SSSType, SUBSTRATE_SSS_TYPE_COUNT - 1u);
    BSDF.SSSProfileId = (uint)SSSProfileId;

    BSDF.Emissive = max(EmissiveColor, 0.0f);

    BSDF.SecondRoughness = clamp(SecondRoughness, 0.001f, 1.0f);
    BSDF.SecondRoughnessWeight = saturate(SecondRoughnessWeight);

    BSDF.FuzzAmount = saturate(FuzzAmount);
    BSDF.FuzzColor = saturate(FuzzColor);
    BSDF.FuzzRoughness = clamp(FuzzRoughness, 0.01f, 1.0f);

    // 下限は SUBSTRATE_MIN_THICKNESS_CM に統一 (PS 側の MFP 導出と同じ床)
    // SSS 評価厚は SSSMFPScale ピン経由 (上記配線)。Thickness 引数
    // (レイヤー厚) はトポロジー / カバレッジ基盤を持たない本実装では
    // API 互換のため受理のみ。
    BSDF.Thickness = max(SSSMFPScale, SUBSTRATE_MIN_THICKNESS_CM) * CENTIMETER_TO_METER; // SSS 評価厚 [cm] -> [m]
    BSDF.bIsThin = IsThin ? 1u : 0u;

    return BSDF;
}

// =============================================================
//  デファード G-Buffer パック / アンパック
//  スロット割り (SubstrateDefinitions.hlsl のヘッダコメント参照)
// =============================================================

uint SubstratePackHeader(FSubstrateBSDF BSDF)
{
    uint Header = SUBSTRATE_BSDF_TYPE_SLAB & SUBSTRATE_HEADER_BSDF_TYPE_MASK;
    Header |= (BSDF.SSSType & SUBSTRATE_HEADER_SSS_TYPE_MASK) << SUBSTRATE_HEADER_SSS_TYPE_SHIFT;

    if (BSDF.bIsThin != 0u)
        Header |= SUBSTRATE_HEADER_FLAG_ISTHIN;
    if (BSDF.SSSType != SUBSTRATE_SSS_TYPE_NONE)
        Header |= SUBSTRATE_HEADER_FLAG_HASSSS;
    if (BSDF.FuzzAmount > 0.0f)
        Header |= SUBSTRATE_HEADER_FLAG_HASFUZZ;
    if (BSDF.SecondRoughnessWeight > 0.0f)
        Header |= SUBSTRATE_HEADER_FLAG_HASSECONDROUGH;
    if (any(BSDF.Emissive > 0.0f))
        Header |= SUBSTRATE_HEADER_FLAG_HASEMISSIVE;
    if (abs(BSDF.Anisotropy) > 0.0f)
        Header |= SUBSTRATE_HEADER_FLAG_HASANISOTROPY;

    return Header;
}

bool SubstrateIsSubstrateMaterial(uint Header)
{
    return (Header & SUBSTRATE_HEADER_BSDF_TYPE_MASK) != SUBSTRATE_BSDF_TYPE_NONE;
}

uint SubstrateGetSSSType(uint Header)
{
    return (Header >> SUBSTRATE_HEADER_SSS_TYPE_SHIFT) & SUBSTRATE_HEADER_SSS_TYPE_MASK;
}

bool SubstrateIsThin(uint Header)
{
    return (Header & SUBSTRATE_HEADER_FLAG_ISTHIN) != 0u;
}

// Slab -> uint4 x 2。DiffuseAlbedo / Roughness / Normal / AO は
// 既存 G-Buffer (GBufferC / GBufferB / GBufferA) 側に書く。
void SubstratePackSlabData(FSubstrateBSDF BSDF, out uint4 OutData0, out uint4 OutData1)
{
    OutData0.x = SubstratePackHeader(BSDF);
    OutData0.y = SubstratePackColorAndScalar(BSDF.F0, BSDF.SSSMFPScale);
    OutData0.z = SubstratePackColorAndScalar(BSDF.F90, BSDF.SSSPhaseAnisotropy * 0.5f + 0.5f);
    // MFP / Thickness の格納単位は [cm] (UE 準拠)。
    // 本エンジンの実値 [m] は 1e-4 ~ 1e-5 スケールになり half の
    // 正規最小 (6.1e-5) を割って精度劣化 / 非正規化フラッシュの
    // 危険があるため、100 倍の cm で格納して正規域に乗せる。
    // 65504 (half 最大) クランプは T -> 1 の極端値で MFP が
    // 発散したときの inf 化防止 (τ ~= 0 の完全透過なので視覚差なし)。
    OutData0.w = SubstratePackHalf2(
        min(BSDF.SSSMFP.r * METER_TO_CENTIMETER, 65504.0f),
        min(BSDF.SSSMFP.g * METER_TO_CENTIMETER, 65504.0f));

    OutData1.x = SubstratePackHalf2(
        min(BSDF.SSSMFP.b * METER_TO_CENTIMETER, 65504.0f),
        BSDF.Thickness * METER_TO_CENTIMETER);
    OutData1.y = SubstratePackR9G9B9E5(BSDF.Emissive);
    OutData1.z = SubstratePackColorAndScalar(BSDF.FuzzColor, BSDF.FuzzAmount);
    OutData1.w = SubstratePackUnorm8(BSDF.FuzzRoughness)
               | (SubstratePackUnorm8(BSDF.SecondRoughness) << 8)
               | (SubstratePackUnorm8(BSDF.SecondRoughnessWeight) << 16)
               | (SubstratePackSnorm8(BSDF.Anisotropy) << 24);
}

// uint4 x 2 -> Slab。DiffuseAlbedo / Roughness / Normal は呼び出し側で
// G-Buffer から上書きする (SubstrateUnpackSlabData 後に設定)。
FSubstrateBSDF SubstrateUnpackSlabData(uint4 Data0, uint4 Data1)
{
    FSubstrateBSDF BSDF = (FSubstrateBSDF)0;

    uint Header = Data0.x;
    BSDF.SSSType = SubstrateGetSSSType(Header);
    BSDF.bIsThin = SubstrateIsThin(Header) ? 1u : 0u;

    float4 F0AndScale = SubstrateUnpackColorAndScalar(Data0.y);
    BSDF.F0 = F0AndScale.rgb;
    BSDF.SSSMFPScale = F0AndScale.a;

    float4 F90AndPhase = SubstrateUnpackColorAndScalar(Data0.z);
    BSDF.F90 = F90AndPhase.rgb;
    BSDF.SSSPhaseAnisotropy = clamp(F90AndPhase.a * 2.0f - 1.0f, -0.99f, 0.99f);

    // 格納単位 [cm] -> 実行単位 [m] へ戻す (パック側コメント参照)
    float2 MFPrg = SubstrateUnpackHalf2(Data0.w);
    float2 MFPbThickness = SubstrateUnpackHalf2(Data1.x);
    BSDF.SSSMFP = float3(MFPrg.x, MFPrg.y, MFPbThickness.x) * CENTIMETER_TO_METER;
    BSDF.Thickness = max(MFPbThickness.y, SUBSTRATE_MIN_THICKNESS_CM) * CENTIMETER_TO_METER;

    BSDF.Emissive = SubstrateUnpackR9G9B9E5(Data1.y);

    float4 FuzzColorAmount = SubstrateUnpackColorAndScalar(Data1.z);
    BSDF.FuzzColor = FuzzColorAmount.rgb;
    BSDF.FuzzAmount = FuzzColorAmount.a;

    BSDF.FuzzRoughness = max(SubstrateUnpackUnorm8(Data1.w), 0.01f);
    BSDF.SecondRoughness = max(SubstrateUnpackUnorm8(Data1.w >> 8), 0.001f);
    BSDF.SecondRoughnessWeight = SubstrateUnpackUnorm8(Data1.w >> 16);
    BSDF.Anisotropy = SubstrateUnpackSnorm8(Data1.w >> 24);

    // 呼び出し側で上書きされる既定値
    BSDF.Normal = float3(0.0f, 0.0f, 1.0f);
    BSDF.DiffuseAlbedo = 0.18f.xxx;
    BSDF.Roughness = 0.5f;

    return BSDF;
}

#endif
