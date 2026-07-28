#include "PBR_Utility.hlsl"
#include "DeferredLightingCommon.hlsl"
#include "SubstrateEvaluation.hlsl"
#include "RefractionCommon.hlsl"
#include "ShadowFilteringCommon.hlsl"
#include "LightGridCommon.hlsl"

// =============================================================
//  TranslucentPS
//  トランスルーセンシーパス (BLEND_Translucent / BLEND_Additive) の
//  フォワードシェーディング PS。BasePassPixelShader (translucent
//  マテリアル) 相当。
//
//  デファードライティング (DeferredPS) と同一のライティング入力
//  (b0 View / b3 ForwardLightData / b5 Shadow, t6-t8 IBL,
//   t13 ライトバッファ, t14-t18 シャドウ, t19-t20 ライトグリッド)
//  をサーフェス位置で直接評価し、確定済み SceneColor へ
//  ハードウェアブレンドで合成する:
//    BLEND_Translucent : SrcAlpha / InvSrcAlpha
//    BLEND_Additive    : SrcAlpha / One
//  シェーダ本体は両モード共通 (ブレンドステートのみ異なる)。
//
//  Substrate (bUseSubstrate) のときは Slab BSDF をフォワードで
//  構築・評価する (SubstrateEvaluation.hlsl。デファードと同じ数式)。
//
//  Refraction (RefractionMethod != NONE, BLEND_Translucent のみ):
//  シーンカラーコピー (t21) を屈折オフセット付きでサンプルし、
//    rgb = Lighting + 屈折背景 x 透過色
//    a   = Opacity
//  を出力してカバレッジ合成はハードウェアブレンド
//  (SrcAlpha/InvSrcAlpha) に任せる (RefractionCommon.hlsl)。
//  ブレンド宛先はライブな SceneColor なので、先に描かれた半透明面
//  (同一メッシュの手前向き三角形や奥の半透明オブジェクト) を
//  消さない。屈折背景そのものは半透明パス開始前のシーン
//  (UE の SceneColor 参照と同じ制約)。Additive は対象外。
//
//  深度は不透明結果に対するテストのみ (PSO: DepthRead、書き込みなし)。
//  α = BaseColor テクスチャ α x 頂点カラー α x Material.Opacity。
//  ※ 深度 SRV (t3) はこのパスでは DSV としてバインド中のため
//    参照しないこと。LinearDepth (t4) は別リソースで SRV のまま
//    なので参照可 (屈折の深度棄却に使用)。SceneColorCopy (t21) と
//    b4 (PostProcess: SceneTexelSize) はこのパス直前に確定する。
// =============================================================

// GeometryPS と同一の TBN 行列生成 (Gram-Schmidt 直交化)
float3x3 BuildTBN(float3 N, float3 tangentWS)
{
    float3 T = normalize(tangentWS - N * dot(N, tangentWS));
    if (any(isnan(T)) || dot(T, T) < 1e-8)
    {
        float3 up = abs(N.y) < 0.999 ? float3(0, 1, 0) : float3(1, 0, 0);
        T = normalize(cross(up, N));
    }
    float3 B = cross(N, T);
    return float3x3(T, B, N);
}

// -------------------------------------------------------------
//  屈折面色の計算 (BLEND_Translucent + RefractionMethod != NONE)
//    SurfaceLighting  : 面のライティング (エミッシブ込み)
//    ViewTransmittance: 背景に乗る透過色 (Substrate SSS 由来 / 1)
//    RoughnessForBlur : 粗い屈折のブラー量 (レガシーは 0)
//  戻り値は「面そのものの色」= Lighting + 屈折背景 x 透過色。
//  呼び出し側が α = Opacity で出力し、カバレッジ合成 (面の外側に
//  ライブな背景を通す) はハードウェアブレンドが行う。
//  ※ かつては Coverage で古い背景コピーと lerp して α = 1 で
//    置換していたが、その方式は先に描かれた半透明面を消してしまう
//    (深度書き込みが無いため、後から描かれた奥向き三角形の置換を
//     深度テストで防げない)。
// -------------------------------------------------------------
float3 ComputeRefractedSurfaceColor(
    float3 SurfaceLighting,
    float3 ViewTransmittance,
    float RoughnessForBlur,
    float2 SvPositionXY,
    float3 WorldNormal,
    float3 WorldVertexNormal,
    float SurfaceViewDepth,
    FMaterialRefractionData RefractionData)
{
    float2 SceneTexelSize = float2(PostProcess.SceneTexelSizeX, PostProcess.SceneTexelSizeY);
    float2 SceneUV = SvPositionXY * SceneTexelSize;

    float2 OffsetUV = ComputeRefractionOffsetUV(
        Material.RefractionMethod,
        RefractionData,
        WorldNormal,
        WorldVertexNormal,
        SceneTexelSize);

    float2 RefractedUV = ResolveRefractedSceneUV(
        SceneUV, OffsetUV,
        SurfaceViewDepth,
        RefractionData.RefractionDepthBias,
        SceneTexelSize);

    float3 RefractedBackground = SampleRefractedSceneColor(
        RefractedUV, RoughnessForBlur, SceneTexelSize);

    return SurfaceLighting + RefractedBackground * ViewTransmittance;
}

PS_OUTPUT main(PS_INPUT input, bool bIsFrontFace : SV_IsFrontFace)
{
    PS_OUTPUT output;

    // ---- BaseColor + Opacity ----
    float4 baseColor = TextureBaseColor.Sample(Sampler, input.TexCoord) * input.Color;
    float opacity = saturate(baseColor.a * Material.Opacity);

    // 屈折を自前合成するか (BLEND_Translucent のみ。Additive は対象外)
    bool bUseRefraction = (Material.BlendMode == BLEND_TRANSLUCENT)
                       && (Material.RefractionMethod != REFRACTION_METHOD_NONE);

    // ---- ワールド座標 / ビュー深度 (両経路共通) ----
    // ワールド座標は深度再構築ではなく VS 補間値 (input.WorldPosition)。
    float3 worldPos = input.WorldPosition.xyz;
    float3 viewDir = normalize(WorldCameraOrigin.xyz - worldPos);
    float viewDepth = mul(float4(worldPos, 1.0f), View).z;

    // ---- Unlit: エミッシブのみ (ベースパスの Unlit 経路と同じ合成) ----
    if (Material.Unlit)
    {
        float3 emissive = Material.EmissionColor.rgb + baseColor.rgb * Material.BaseColor.rgb;

        [branch]
        if (bUseRefraction)
        {
            // Unlit ガラス: ライティングなしで背景屈折のみ (UE の
            // GetUnlitMaterialRefractionIOR 相当)。透過色は 1。
            float3 vertexNormalUnlit = normalize(input.Normal.xyz);
            float3 refracted = ComputeRefractedSurfaceColor(
                emissive,
                float3(1.0f, 1.0f, 1.0f), 0.0f,
                input.Position.xy,
                vertexNormalUnlit, vertexNormalUnlit,
                viewDepth,
                GetMaterialRefraction());
            // カバレッジ合成はハードウェアブレンドに任せる
            output.Color = float4(refracted, opacity);
            return output;
        }

        output.Color = float4(emissive, opacity);
        return output;
    }

    // ---- 法線 (TBN + Two Sided 反転) ----
    float3 vertexNormal = normalize(input.Normal.xyz);

    // Two Sided: 裏面は幾何法線を反転する (TwoSidedSign 相当)
    if (Material.TwoSided && !bIsFrontFace)
    {
        vertexNormal = -vertexNormal;
    }

    float3x3 TBN = BuildTBN(vertexNormal, input.Tangent);
    float3 normalSample = TextureNormal.Sample(Sampler, input.TexCoord).xyz * 2.0f - 1.0f;
    float3 mappedNormal = normalize(mul(normalSample, TBN));
    float3 normal = normalize(lerp(vertexNormal, mappedNormal, Material.NormalWeight));

    // ---- ARM (GeometryPS と同じフォールバック規約) ----
    float4 ARM = TextureMSRA.Sample(Sampler, input.TexCoord);
    float occlusion = (ARM.r == 0.0f) ? 1.0f : ARM.r;
    float roughness = (ARM.g == 0.0f) ? Material.Roughness : ARM.g;
    float metallic = (ARM.b == 0.0f) ? Material.Metallic : ARM.b;

    float3 lightDir = normalize(DirectionalLightDirection.xyz);

    // ---- ディレクショナルシャドウ (CSM + DF) ----
    float directionalShadow = GetDirectionalShadow(worldPos, normal, viewDepth);

    // ---- ライトグリッドセル (両経路共通) ----
    uint3 GridCoordinate = ComputeLightGridCellCoordinate(uint2(input.Position.xy), viewDepth);
    uint GridIndex = ComputeLightGridCellIndex(GridCoordinate);

    float3 surfaceLighting;
    float3 viewTransmittance = float3(1.0f, 1.0f, 1.0f); // 屈折背景に乗る透過色
    float refractionRoughness = 0.0f; // 粗い屈折のブラー量
    FMaterialRefractionData refractionData = GetMaterialRefraction();

    [branch]
    if (Material.bUseSubstrate)
    {
        // ============================================================
        //  Substrate Slab 経路 (フォワード)
        //  GeometryPS と同じ手順で Slab を構築し、デファードと同じ
        //  数式 (SubstrateEvaluation.hlsl) で直接評価する。
        // ============================================================
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
            /*Normal*/                           normal,
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
            /*ClearCoatBottomNormal*/            normal,
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

        // ---- ディレクショナルライト ----
        float3 light = SubstrateEvaluateSlabDirect(
            SlabBSDF, normal, viewDir,
            lightDir, lightDir,
            0.0f, 0.0f, 1.0f, 1.0f, 1.0f)
            * DirectionalLightColor.rgb * directionalShadow;

        // ---- ローカルライト (Point / Spot / Rect) ----
        float3 localLight = float3(0.0f, 0.0f, 0.0f);

        [branch]
        if (bUseLightGrid != 0u)
        {
            FCulledLightsGridHeader GridHeader = GetCulledLightsGridHeader(GridIndex);

            for (uint i = 0; i < GridHeader.NumLights; ++i)
            {
                uint LightIndex = GetCulledLightDataGrid(GridHeader.DataStartIndex + i);
                FLightShaderParameters localLightParams = ForwardLocalLights[LightIndex];

                float localShadow = GetLocalLightShadow(
                    localLightParams, LocalShadowParams[LightIndex], worldPos, normal);

                localLight += IntegrateLocalLightSubstrate(
                    localLightParams,
                    worldPos, normal, viewDir,
                    SlabBSDF) * localShadow;
            }
        }
        else
        {
            for (uint i = 0; i < NumLocalLights; ++i)
            {
                FLightShaderParameters localLightParams = ForwardLocalLights[i];

                float localShadow = GetLocalLightShadow(
                    localLightParams, LocalShadowParams[i], worldPos, normal);

                localLight += IntegrateLocalLightSubstrate(
                    localLightParams,
                    worldPos, normal, viewDir,
                    SlabBSDF) * localShadow;
            }
        }

        // ---- IBL + エミッシブ ----
        float3 iblAmbient = SubstrateEnvLighting(SlabBSDF, normal, viewDir, occlusion);

        surfaceLighting = light + localLight + iblAmbient + SlabBSDF.Emissive;

        // 屈折背景の着色 (Colored Transmittance): SSS 有効時は
        // 視線方向のスラブ透過。粗い屈折は Slab のラフネス。
        [branch]
        if (bUseRefraction && SlabBSDF.SSSType != SUBSTRATE_SSS_TYPE_NONE)
        {
            viewTransmittance = SubstrateViewTransmittance(
                SlabBSDF, max(dot(normal, viewDir), 1e-4f));
        }
        refractionRoughness = SlabBSDF.Roughness;

        // ---- Index Of Refraction From F0 (UE5.8) ----
        // Substrate では界面を F0 が定義するため、屈折 IOR も
        // 同じ F0 から導出できる (誘電体逆変換)。手入力 IOR と
        // F0 由来のスペキュラが食い違わないのが利点。
        [branch]
        if (bUseRefraction
            && Material.RefractionMethod == REFRACTION_METHOD_INDEX_OF_REFRACTION
            && Material.bRefractionUseF0)
        {
            refractionData.Data.x = DielectricF0ToIor(F0RGBToF0(SlabBSDF.F0));
        }
    }
    else
    {
        // ============================================================
        //  レガシー Metallic/Specular 経路 (従来と同一)
        // ============================================================
        float3 light = CookTorrance(
            normal, lightDir, viewDir,
            baseColor.rgb, roughness, metallic,
            DirectionalLightColor.rgb) * directionalShadow;

        // ---- ローカルライト (Point / Spot / Rect) ----
        // タイルドライトカリング (ライトグリッド) はデファードと共通。
        float3 localLight = float3(0.0f, 0.0f, 0.0f);

        [branch]
        if (bUseLightGrid != 0u)
        {
            FCulledLightsGridHeader GridHeader = GetCulledLightsGridHeader(GridIndex);

            for (uint i = 0; i < GridHeader.NumLights; ++i)
            {
                uint LightIndex = GetCulledLightDataGrid(GridHeader.DataStartIndex + i);
                FLightShaderParameters localLightParams = ForwardLocalLights[LightIndex];

                float localShadow = GetLocalLightShadow(
                    localLightParams, LocalShadowParams[LightIndex], worldPos, normal);

                localLight += IntegrateLocalLight(
                    localLightParams,
                    worldPos, normal, viewDir,
                    baseColor.rgb, roughness, metallic) * localShadow;
            }
        }
        else
        {
            // フォールバック: 全灯ループ (グリッド無効時の従来経路)
            for (uint i = 0; i < NumLocalLights; ++i)
            {
                FLightShaderParameters localLightParams = ForwardLocalLights[i];

                float localShadow = GetLocalLightShadow(
                    localLightParams, LocalShadowParams[i], worldPos, normal);

                localLight += IntegrateLocalLight(
                    localLightParams,
                    worldPos, normal, viewDir,
                    baseColor.rgb, roughness, metallic) * localShadow;
            }
        }

        // --- IBL Ambient (diffuse irradiance + specular split-sum) ---
        float3 iblAmbient = IBL_Ambient(
            baseColor.rgb,
            metallic, roughness, occlusion,
            normal, viewDir);

        // Lit 経路の合成はデファードと同一 (エミッシブは Unlit 経路のみ、
        // GeometryPS / DeferredPS の挙動と一致させる)
        surfaceLighting = light + localLight + iblAmbient;
    }

    // ---- Refraction 合成 / 通常のハードウェアブレンド ----
    [branch]
    if (bUseRefraction)
    {
        // IOR は refractionData に確定済み (bRefractionUseF0 のとき
        // Substrate 分岐内で F0 から導出済み。それ以外は b2 の手入力値)。
        float3 refracted = ComputeRefractedSurfaceColor(
            surfaceLighting,
            viewTransmittance, refractionRoughness,
            input.Position.xy,
            normal, vertexNormal,
            viewDepth,
            refractionData);

        // α = Opacity: カバレッジ合成 (面の外側にライブな背景を通す)
        // は SrcAlpha/InvSrcAlpha ブレンドが行う。宛先はライブな
        // SceneColor なので、先に描かれた半透明面 (同一メッシュの
        // 手前向き三角形や奥の半透明オブジェクト) を消さない。
        output.Color = float4(refracted, opacity);
    }
    else
    {
        output.Color = float4(surfaceLighting, opacity);
    }

    return output;
}
