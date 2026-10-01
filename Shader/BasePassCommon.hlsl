#ifndef BASE_PASS_COMMON_HLSL
#define BASE_PASS_COMMON_HLSL

#include "Common.hlsl"
#include "Substrate.hlsl"

// =============================================================
//  BasePassCommon
//  ベースパス系 PS (GeometryPS / TranslucentPS) で共有する
//  TBN 行列生成とマテリアル (b2) からの Substrate Slab 構築。
// =============================================================

// 頂点タンジェントベースのTBN行列生成
// N: 正規化済みワールド法線, tangentWS: 補間後ワールド接線
float3x3 BuildTBN(float3 N, float3 tangentWS)
{
    // Gram-Schmidt 直交化（補間で法線と接線が非直交になるのを補正）
    float3 T = normalize(tangentWS - N * dot(N, tangentWS));
    // 縮退接線への保険
    if (any(isnan(T)) || dot(T, T) < 1e-8)
    {
        float3 up = abs(N.y) < 0.999 ? float3(0, 1, 0) : float3(1, 0, 0);
        T = normalize(cross(up, N));
    }
    float3 B = cross(N, T);
    return float3x3(T, B, N);
}

// ------------------------------------------------------------
//  Substrate Slab BSDF
//  呼出規約はサンプル逐語:
//    MFP -> SSSMFP ピン / Thickness -> SSSMFPScale ピン
//    (SSS 評価厚 [cm]) / Slab の Thickness 引数 = 0.01cm 固定
//  【意図的乖離】MFP 導出距離のみ固定参照厚 1cm
//  (SUBSTRATE_TRANSMITTANCE_REFERENCE_CM)。サンプルどおり
//  Thickness で導出すると評価厚と相殺し Thickness が無効化
//  されるため。τ = Thickness x (-log T) / 1cm で Thickness が
//  濃度スケールとして機能する (Constant.hlsl 参照)。
//
//    WorldNormal : ワールド法線 (ClearCoatBottomNormal にも使用)
//    BaseColor   : BaseColor テクスチャ x 頂点カラー (DiffuseAlbedo に乗算)
//    Roughness   : ARM フォールバック適用後のラフネス
// ------------------------------------------------------------
FSubstrateBSDF GetMaterialSubstrateSlabBSDF(float3 WorldNormal, float3 BaseColor, float Roughness)
{
    // Slab 厚の下限 (パック/評価のクランプ床と一致)
    const float SlabThicknessCm = max(Material.SubstrateThickness, SUBSTRATE_MIN_THICKNESS_CM);

    // MFP は固定参照厚 1cm で導出する (意図的乖離。サンプルどおり
    // Thickness で導出すると SSS 評価厚 = SSSMFPScale ピンの同値と
    // 相殺して Thickness が無効化されるため。Constant.hlsl 参照)。
    // τ = Thickness x (-log T) / 1cm -> Thickness = 1cm で
    // Transmittance Color が厳密に実現され、厚いほど濃くなる。
    float3 SSSMFP = TransmittanceToMeanFreePath(
        Material.SubstrateTransmittanceColor.rgb,
        SUBSTRATE_TRANSMITTANCE_REFERENCE_CM * CENTIMETER_TO_METER);

    return GetSubstrateSlabBSDF(
        GetSubstratePixelFootprint(),
        /*Normal*/                           WorldNormal,
        /*DiffuseAlbedo*/                    Material.SubstrateDiffuseAlbedo.rgb * BaseColor,
        /*F0*/                               Material.SubstrateF0.rgb,
        /*F90*/                              Material.SubstrateF90.rgb,
        /*Roughness*/                        Roughness,
        /*Anisotropy*/                       Material.SubstrateAnisotropy,
        /*SSSProfileId*/                     0.0f,
        /*bSupportDefaultSSSProfile*/        false,
        /*SSSMFP*/                           SSSMFP,
        /*SSSMFPScale*/                      SlabThicknessCm,
        /*SSSPhaseAniso*/                    Material.SubstrateSSSPhaseAnisotropy,
        /*SSSType*/                          (float)Material.SubstrateSSSType,
        /*EmissiveColor*/                    Material.EmissionColor.rgb,
        /*SecondRoughness*/                  Material.SubstrateSecondRoughness,
        /*SecondRoughnessWeight*/            Material.SubstrateSecondRoughnessWeight,
        /*SecondRoughnessAsSimpleClearCoat*/ 0.0f,
        /*ClearCoatUseSecondNormal*/         0.0f,
        /*ClearCoatBottomNormal*/            WorldNormal,
        /*FuzzAmount*/                       Material.SubstrateFuzzColor.w,
        /*FuzzColor*/                        Material.SubstrateFuzzColor.rgb,
        /*FuzzRoughness*/                    Material.SubstrateFuzzRoughness,
        /*GlintValue*/                       1.0f,
        /*GlintUV*/                          float2(0.0f, 0.0f),
        /*SpecularProfileId*/                0.0f,
        /*Thickness*/                        SUBSTRATE_LAYER_DEFAULT_THICKNESS_CM,
        /*IsThin*/                           Material.SubstrateIsThin,
        /*IsAtBottom*/                       true,
        /*LocalBasisIndex*/                  SHAREDLOCALBASIS_INDEX_0,
        /*SharedLocalBasesTypes*/            0u);
}

#endif
