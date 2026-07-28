#include "Common.hlsl"
#include "Substrate.hlsl"

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

// GBufferパス (Opaque / Masked)
// Translucent / Additive はトランスルーセンシーパス (TranslucentPS) が描く。
//
// Substrate (bUseSubstrate) のときは Slab BSDF を構築して
//   RT0 (GBufferC) = DiffuseAlbedo
//   RT2 (GBufferB) = (Metallic=0, Specular=0.5, Roughness, AO)
//   RT3/RT4        = SubstratePackSlabData (F0/F90/SSS/Emissive 等)
// に書く。非 Substrate ピクセルは RT3.x = 0 (レガシー経路マーカー)。
PS_OUTPUT_GEOMETRY main(PS_INPUT input, bool bIsFrontFace : SV_IsFrontFace)
{
    PS_OUTPUT_GEOMETRY output;

    // ---- BaseColor ----
    float4 baseColor = TextureBaseColor.Sample(Sampler, input.TexCoord) * input.Color;

    // ---- BLEND_Masked: OpacityMask クリップ ----
    // (GetMaterialCoverageAndClipping 相当。OpacityMask =
    //  BaseColor テクスチャ α x 頂点カラー α。しきい値は
    //  UMaterial::OpacityMaskClipValue)
    if (Material.BlendMode == BLEND_MASKED)
    {
        clip(baseColor.a - Material.OpacityMaskClipValue);
    }

    // ---- Unlit: EmissionColor + BaseColor をColorに書き、ライティングをスキップさせる ----
    // Normal.w == 0.0 をUnlitマーカーとして使用
    if (Material.Unlit)
    {
        // EmissionColor と BaseColor を合成してColorバッファへ
        float3 emissive = Material.EmissionColor.rgb + baseColor.rgb * Material.BaseColor.rgb;
        output.Color = float4(emissive, baseColor.a);

        // Normal.w = 0.0 → DeferredPS がライティングをスキップする合図
        output.Normal = float4(0.0f, 0.0f, 0.0f, 0.0f);

        // MSRAは書き込まない（ゴミ値でいいがゼロで明示）
        output.MSRA = float4(0.0f, 0.0f, 0.0f, 0.0f);

        // Substrate バッファもゼロ (ヘッダ 0 = 非 Substrate)
        output.SubstrateData0 = uint4(0u, 0u, 0u, 0u);
        output.SubstrateData1 = uint4(0u, 0u, 0u, 0u);

        return output;
    }

    // ---- Normal mapping ----
    float3 vertexNormal = normalize(input.Normal.xyz);

    // ---- Two Sided: 裏面は幾何法線を反転する (TwoSidedSign 相当) ----
    // PSO 側 (BasePassTwoSided) でカリングが無効化されているため、
    // 裏面ピクセルもここに到達する。
    if (Material.TwoSided && !bIsFrontFace)
    {
        vertexNormal = -vertexNormal;
    }

    float3x3 TBN = BuildTBN(vertexNormal, input.Tangent);
    float3 normalSample = TextureNormal.Sample(Sampler, input.TexCoord).xyz;
    normalSample = normalSample * 2.0f - 1.0f;
    // 接線空間 → ワールド空間（row-vector × TBN）
    float3 mappedNormal = normalize(mul(normalSample, TBN));
    float3 worldNormal = normalize(lerp(vertexNormal, mappedNormal, Material.NormalWeight));
    output.Normal = float4(worldNormal, 1.0f);

    // ---- MSR from ARM texture (t2 = GBufferB スロットをマテリアル ARM として流用) ----
    // ワールド座標 G-Buffer は廃止: ライティングパスが深度 +
    // InvViewProjection からワールド座標を再構築する。
    float4 ARM = TextureMSRA.Sample(Sampler, input.TexCoord);

    float ambientOcclusion = (ARM.r == 0.0f) ? 1.0f : ARM.r;
    float roughness = (ARM.g == 0.0f) ? Material.Roughness : ARM.g;
    float metallic = (ARM.b == 0.0f) ? Material.Metallic : ARM.b;

    [branch]
    if (Material.bUseSubstrate)
    {
        // ------------------------------------------------------------
        //  Substrate Slab BSDF (UE5.8)
        //  呼出規約はサンプル逐語:
        //    MFP -> SSSMFP ピン / Thickness -> SSSMFPScale ピン
        //    (SSS 評価厚 [cm]) / Slab の Thickness 引数 = 0.01cm 固定
        //  【意図的乖離】MFP 導出距離のみ固定参照厚 1cm
        //  (SUBSTRATE_TRANSMITTANCE_REFERENCE_CM)。サンプルどおり
        //  Thickness で導出すると評価厚と相殺し Thickness が無効化
        //  されるため。τ = Thickness x (-log T) / 1cm で Thickness が
        //  濃度スケールとして機能する (Constant.hlsl 参照)。
        // ------------------------------------------------------------
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

        FSubstrateBSDF SlabBSDF = GetSubstrateSlabBSDF(
            GetSubstratePixelFootprint(),
            /*Normal*/                           worldNormal,
            /*DiffuseAlbedo*/                    Material.SubstrateDiffuseAlbedo.rgb * baseColor.rgb,
            /*F0*/                               Material.SubstrateF0.rgb,
            /*F90*/                              Material.SubstrateF90.rgb,
            /*Roughness*/                        roughness,
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
            /*ClearCoatBottomNormal*/            worldNormal,
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

        // GBufferC = DiffuseAlbedo (ライティングパスが Slab の
        // アルベドとして読む)。GBufferB は Metallic=0 / Specular=0.5
        // 固定 (Substrate は F0 で界面を定義するため)。
        output.Color = float4(SlabBSDF.DiffuseAlbedo, baseColor.a);
        output.MSRA = float4(0.0f, 0.5f, SlabBSDF.Roughness, ambientOcclusion);

        SubstratePackSlabData(SlabBSDF, output.SubstrateData0, output.SubstrateData1);
    }
    else
    {
        // ---- レガシー Metallic/Specular ワークフロー ----
        output.Color = baseColor;
        output.MSRA = float4(metallic, Material.Specular, roughness, ambientOcclusion);

        // ヘッダ 0 = 非 Substrate ピクセル (レガシー経路マーカー)
        output.SubstrateData0 = uint4(0u, 0u, 0u, 0u);
        output.SubstrateData1 = uint4(0u, 0u, 0u, 0u);
    }

    return output;
}
