#include "Common.hlsl"
#include "Substrate.hlsl"
#include "BasePassCommon.hlsl"

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
        // Substrate Slab BSDF (呼出規約 / 意図的乖離は BasePassCommon.hlsl 参照)
        FSubstrateBSDF SlabBSDF = GetMaterialSubstrateSlabBSDF(worldNormal, baseColor.rgb, roughness);

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
