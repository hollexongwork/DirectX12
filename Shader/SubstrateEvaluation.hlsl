#ifndef SUBSTRATE_EVALUATION_HLSL
#define SUBSTRATE_EVALUATION_HLSL

#include "Substrate.hlsl"
#include "DeferredLightingCommon.hlsl"

// =============================================================
//  SubstrateEvaluation
//  Slab BSDF の直接光評価
//  (Sub-Surface Type 6 種すべて) と IBL 評価を提供する。
//  デファード (DeferredPS) とフォワード半透明 (TranslucentPS) が
//  共有する。
//
//  Sub-Surface Type :
//    NONE              : 標準 Lambert
//    WRAP              : ラップライティング (W = 0.5)
//    TWO_SIDED_WRAP    : 表ラップ + 裏面透過 (HG 位相 x スラブ透過)
//    DIFFUSION /
//    DIFFUSION_PROFILE : スクリーン空間拡散パス非対応環境のため、
//                        非散乱ディフューズへフォールバック
//    SIMPLEVOLUME      : 透過 = Beer-Lambert、散乱 = 単散乱スラブ
//                        解析解 (Hanrahan-Krueger / Chandrasekhar)
//                        を INV_PI 正規化した近似
//
//  ライト 1 灯の評価は SubstrateDeferredLighting。減衰 / シャドウ /
//  面光源の積分コンテキスト (FAreaLightIntegrateContext) は
//  DeferredLightingCommon.hlsl のレガシー経路と共有する。
// =============================================================

// -------------------------------------------------------------
//  Henyey-Greenstein 位相関数 (∫_{S^2} P dΩ = 1 の正規化)
// -------------------------------------------------------------
float HenyeyGreensteinPhase(float G, float CosTheta)
{
    float G2 = G * G;
    float Denom = 1.0f + G2 - 2.0f * G * CosTheta;
    return (1.0f - G2) / max(4.0f * PI * Denom * sqrt(max(Denom, 1e-8f)), 1e-8f);
}

// 4π 正規化位相 (g = 0 で 1。ランバート等価の基準にする)
float SubstratePhase4Pi(float G, float CosTheta)
{
    return 4.0f * PI * HenyeyGreensteinPhase(G, CosTheta);
}

// -------------------------------------------------------------
//  F90 フェード (F0 < 0.02 で F90 は黒へフェードする)
//  50 * avg(F0) は avg(F0) = 0.02 でちょうど 1 になる。
// -------------------------------------------------------------
float3 SubstrateComputeF90(float3 F0, float3 F90)
{
    return F90 * saturate(50.0f * F0RGBToF0(F0));
}

// 一般化 Schlick フレネル (F90 は SubstrateComputeF90 適用済みを渡す)
float3 SubstrateSchlickFresnel(float3 F0, float3 F90, float VoH)
{
    float f = pow(1.0f - max(VoH, 0.0f), 5.0f);
    return F0 + (F90 - F0) * f;
}

// -------------------------------------------------------------
//  ファズ (布シーン): Charlie NDF + Ashikhmin 可視項
// -------------------------------------------------------------
float SubstrateD_Charlie(float Roughness, float NoH)
{
    float InvAlpha = 1.0f / max(Roughness, 0.01f);
    float Cos2H = NoH * NoH;
    float Sin2H = max(1.0f - Cos2H, 0.0078125f); // 2^-7: fp16 でも安全
    return (2.0f + InvAlpha) * pow(Sin2H, InvAlpha * 0.5f) / (2.0f * PI);
}

float SubstrateV_Ashikhmin(float NoV, float NoL)
{
    return rcp(max(4.0f * (NoL + NoV - NoL * NoV), 1e-4f));
}

// -------------------------------------------------------------
//  GGX スペキュラローブ 1 本 (球 / チューブ光源のエネルギー正規化込み)。
//  戻り値は D * Vis * F (NoL は掛けない)。ShadingModels.hlsl の
//  SpecularGGX と同じ式で、フレネルだけ F0 / F90 の一般化 Schlick。
// -------------------------------------------------------------
float3 SubstrateSpecularLobe(
    float Roughness, float3 F0, float3 F90,
    BxDFContext Context, float NoL, FAreaLight AreaLight)
{
    float a2 = Pow4(Roughness);
    float Energy = EnergyNormalization(a2, Context.VoH, AreaLight);

    float D = D_GGX(a2, Context.NoH) * Energy;

    // 既存の SmithGeometry はラフネスを受け取るので、広げた後の a2 から戻す
    float RoughnessG = sqrt(sqrt(a2));
    float Vis = SmithGeometry(Context.NoV, NoL, RoughnessG) / max(4.0f * Context.NoV * NoL, EPSILON);
    float3 F = SubstrateSchlickFresnel(F0, F90, Context.VoH);

    return (D * Vis) * F;
}

// -------------------------------------------------------------
//  SubstrateEvaluateSlabDirect
//  Slab BSDF の直接光 1 灯評価 (ライト色とシャドウを掛ける前)。
//  面光源の積分コンテキスト (代表方向 L / NoL / Falloff / FAreaLight) を
//  受け取る。
//    - スペキュラ: 球 / チューブは GGX、レクトライトは LTC
//    - 拡散 / Sub-Surface: NoL は符号付きで評価し、負側は透過が受け持つ
// -------------------------------------------------------------
FDirectLighting SubstrateEvaluateSlabDirect(
    FSubstrateBSDF BSDF,
    float3 N, float3 V,
    FAreaLightIntegrateContext AreaLightContext)
{
    FDirectLighting Lighting;
    Lighting.Diffuse = float3(0.0f, 0.0f, 0.0f);
    Lighting.Specular = float3(0.0f, 0.0f, 0.0f);
    Lighting.Transmission = float3(0.0f, 0.0f, 0.0f);

    const FAreaLight AreaLight = AreaLightContext.AreaLight;
    const float3 L = AreaLightContext.L;
    const float Falloff = AreaLightContext.Falloff;
    const float NoL = AreaLightContext.NoL; // 光源形状込み (0..1)

    BxDFContext Context;
    Init(Context, N, V, L);
    SphereMaxNoH(Context, AreaLight.SphereSinAlpha, true);
    Context.NoV = saturate(abs(Context.NoV) + 1e-5f);

    const float NoV = Context.NoV;
    const float NoL_d = dot(N, L); // 符号付き (裏面透過用)

    float3 F90 = SubstrateComputeF90(BSDF.F0, BSDF.F90);

    // ------------------------------------------------------------
    //  Specular (界面反射: 表側のみ)
    //  第 2 ラフネスは 2 ローブの重み lerp、ファズはカバレッジ lerp
    //  (布はグロスをシーンで置き換える)。
    // ------------------------------------------------------------
    [branch]
    if (IsRectLight(AreaLight))
    {
        // レクトライト: LTC がローブを矩形で積分する (Falloff * NoL は含まれている)
        float3 Lobe = RectGGXApproxLTC(BSDF.Roughness, BSDF.F0, F90, N, V, AreaLight.Rect);

        [branch]
        if (BSDF.SecondRoughnessWeight > 0.0f)
        {
            float3 SecondLobe = RectGGXApproxLTC(BSDF.SecondRoughness, BSDF.F0, F90, N, V, AreaLight.Rect);
            Lobe = lerp(Lobe, SecondLobe, BSDF.SecondRoughnessWeight);
        }

        Lighting.Specular = Lobe;

        [branch]
        if (BSDF.FuzzAmount > 0.0f)
        {
            // ファズは平均方向で 1 回評価する (面積分の近似)
            float Sheen = SubstrateD_Charlie(BSDF.FuzzRoughness, Context.NoH)
                        * SubstrateV_Ashikhmin(NoV, NoL);
            Lighting.Specular = lerp(Lighting.Specular, BSDF.FuzzColor * Sheen * (Falloff * NoL), BSDF.FuzzAmount);
        }
    }
    else if (NoL > 0.0f)
    {
        float3 Lobe = SubstrateSpecularLobe(BSDF.Roughness, BSDF.F0, F90, Context, NoL, AreaLight);

        [branch]
        if (BSDF.SecondRoughnessWeight > 0.0f)
        {
            float3 SecondLobe = SubstrateSpecularLobe(BSDF.SecondRoughness, BSDF.F0, F90, Context, NoL, AreaLight);
            Lobe = lerp(Lobe, SecondLobe, BSDF.SecondRoughnessWeight);
        }

        Lighting.Specular = Lobe * NoL;

        [branch]
        if (BSDF.FuzzAmount > 0.0f)
        {
            float Sheen = SubstrateD_Charlie(BSDF.FuzzRoughness, Context.NoH)
                        * SubstrateV_Ashikhmin(NoV, NoL);
            Lighting.Specular = lerp(Lighting.Specular, BSDF.FuzzColor * Sheen * NoL, BSDF.FuzzAmount);
        }

        Lighting.Specular *= AreaLight.FalloffColor * Falloff;
    }

    // ------------------------------------------------------------
    //  拡散へ入るエネルギー (界面フレネルの残り)
    //  代表方向が裏面でも安定なようビュー基準の近似。
    // ------------------------------------------------------------
    float3 kD = 1.0f - SubstrateSchlickFresnel(BSDF.F0, F90, NoV);

    // ------------------------------------------------------------
    //  Diffuse / Sub-Surface (SSSType 別)
    // ------------------------------------------------------------
    float3 MFPEff = max(BSDF.SSSMFP * BSDF.SSSMFPScale, 1e-9f); // [m]
    float3 SigmaT = rcp(MFPEff);
    float3 OpticalDepth = SigmaT * BSDF.Thickness; // τ (チャンネル別)
    float PhaseCos = dot(-L, V); // 散乱角: 入射伝播 (-L) -> 出射 (V)
    float Phase4Pi = SubstratePhase4Pi(BSDF.SSSPhaseAnisotropy, PhaseCos);

    float3 Diffuse = float3(0.0f, 0.0f, 0.0f);

    [branch]
    if (BSDF.SSSType == SUBSTRATE_SSS_TYPE_WRAP)
    {
        // ---- ラップライティング (レガシー Subsurface 相当, W = 0.5) ----
        const float W = 0.5f;
        float WrapNoL = saturate((NoL_d + W) / Square(1.0f + W));
        Diffuse = BSDF.DiffuseAlbedo * INV_PI * WrapNoL;
    }
    else if (BSDF.SSSType == SUBSTRATE_SSS_TYPE_TWO_SIDED_WRAP)
    {
        // ---- 両面ラップ (レガシー Two Sided Foliage 相当) ----
        // 表: ラップ拡散 / 裏: スラブ透過 x HG 位相 (前方散乱で逆光が抜ける)
        const float W = 0.5f;
        float WrapNoL = saturate((NoL_d + W) / Square(1.0f + W));
        float TransNoL = saturate((-NoL_d + W) / Square(1.0f + W));

        float3 SlabTransmittance = exp(-OpticalDepth);
        Diffuse = BSDF.DiffuseAlbedo * INV_PI * WrapNoL
                + SlabTransmittance * TransNoL * Phase4Pi * INV_PI;
    }
    else if (BSDF.SSSType == SUBSTRATE_SSS_TYPE_SIMPLEVOLUME)
    {
        // ---- シンプルボリューム: 単散乱スラブ解析解 ----
        //  反射構成 (μi > 0):
        //    R = ω P4π/π * μi/(μi+μo) * (1 - exp(-τ(1/μi + 1/μo)))
        //  透過構成 (μi < 0, μt = |μi|):
        //    T = ω P4π/π * μt * (exp(-τ/μo) - exp(-τ/μt)) / (μo - μt)
        //    (μo ≈ μt の特異点は極限値 τ/μo^2 * exp(-τ/μo) で回避)
        //  τ -> 0 で 0 (薄い)、τ -> ∞ で反射は飽和・透過は 0 (厚い)。
        [branch]
        if (NoL_d > 0.0f)
        {
            float MuI = max(NoL_d, 1e-4f);
            float MuO = NoV;
            float3 Slab = 1.0f - exp(-OpticalDepth * (rcp(MuI) + rcp(MuO)));
            Diffuse = BSDF.DiffuseAlbedo * (Phase4Pi * INV_PI)
                    * (MuI / (MuI + MuO)) * Slab;
        }
        else
        {
            float MuT = max(-NoL_d, 1e-4f);
            float MuO = NoV;
            float Den = MuO - MuT;
            float3 Ratio = (abs(Den) > 1e-3f)
                ? (exp(-OpticalDepth / MuO) - exp(-OpticalDepth / MuT)) / Den
                : (OpticalDepth / (MuO * MuO)) * exp(-OpticalDepth / MuO);
            Diffuse = BSDF.DiffuseAlbedo * (Phase4Pi * INV_PI) * MuT * Ratio;
        }
    }
    else
    {
        // ---- NONE / DIFFUSION / DIFFUSION_PROFILE ----
        // 非散乱ディフューズへフォールバックする。NoL は光源形状込み
        // (球光源の地平線の回り込み / レクトライトの平均方向)。
        Diffuse = BSDF.DiffuseAlbedo * INV_PI * NoL;
    }

    Lighting.Diffuse = Diffuse * kD * (AreaLight.FalloffColor * Falloff);

    return Lighting;
}

// -------------------------------------------------------------
//  SubstrateDeferredLighting
//  ライト 1 灯の Slab 評価。
//  減衰 / シャドウ / 面光源の形状はレガシー経路
//  (DeferredLightingCommon.hlsl の AccumulateDynamicLighting) と同じ手順で、
//  BxDF だけが Slab になる。
//    SceneDepth       : 受光点のビュー空間 Z [m] (コンタクトシャドウ用)
//    LightAttenuation : 呼び出し側が求めたシャドウ係数
// -------------------------------------------------------------
float3 SubstrateDeferredLighting(
    FSubstrateBSDF BSDF,
    float3 WorldPosition, float3 CameraVector, float3 N, float SceneDepth,
    FDeferredLightData LightData, float LightAttenuation, float Dither)
{
    float3 OutLighting = float3(0.0f, 0.0f, 0.0f);

    float3 V = -CameraVector;

    float3 L = LightData.Direction; // 正規化済み
    float3 ToLight = L;
    float3 MaskedLightColor = LightData.Color;
    float LightMask = 1.0f;
    if (LightData.bRadialLight)
    {
        LightMask = GetLocalLightAttenuation(WorldPosition, LightData, ToLight, L);
        MaskedLightColor *= LightMask;
    }

    [branch]
    if (LightMask > 0.0f)
    {
        FShadowTerms Shadow;
        Shadow.SurfaceShadow = 1.0f;
        Shadow.TransmissionShadow = 1.0f;
        Shadow.TransmissionThickness = 1.0f;
        GetShadowTerms(SceneDepth, LightData, WorldPosition, L, LightAttenuation, Dither, Shadow);

        [branch]
        if (Shadow.SurfaceShadow > 0.0f)
        {
            // 解析ライトのハイライトが点に潰れないようラフネスに下限を入れる
            BSDF.Roughness = max(BSDF.Roughness, MIN_ROUGHNESS);
            BSDF.SecondRoughness = max(BSDF.SecondRoughness, MIN_ROUGHNESS);

            FAreaLightIntegrateContext AreaLightContext = InitAreaLightIntegrateContext();
            bool bVisible = true;

            if (LightData.bRectLight)
            {
                FRect Rect = GetRect(ToLight, LightData);
                bVisible = IsRectVisible(Rect);
                if (bVisible)
                {
                    AreaLightContext = CreateRectIntegrateContext(BSDF.Roughness, N, V, Rect);
                }
            }
            else
            {
                FCapsuleLight Capsule = GetCapsule(ToLight, LightData);
                AreaLightContext = CreateCapsuleIntegrateContext(BSDF.Roughness, N, V, Capsule, LightData.bInverseSquared);
            }

            [branch]
            if (bVisible)
            {
                // 拡散の符号付き NoL (Sub-Surface の裏面透過) は光源形状を考慮しない代表方向で評価する
                FDirectLighting Lighting = SubstrateEvaluateSlabDirect(BSDF, N, V, AreaLightContext);

                Lighting.Specular *= LightData.SpecularScale;
                Lighting.Diffuse *= LightData.DiffuseScale;

                OutLighting = (Lighting.Diffuse + Lighting.Specular) * (MaskedLightColor * Shadow.SurfaceShadow);
            }
        }
    }

    return OutLighting;
}

// -------------------------------------------------------------
//  SubstrateEnvLighting
//  Slab の IBL 評価 (split-sum の F0/F90 一般化):
//    diffuse  = irradiance x DiffuseAlbedo x kD
//    specular = prefiltered x (F0 * scale + F90 * bias)
//    第 2 ラフネスは 2 ミップの lerp、ファズはカバレッジ lerp、
//    Sub-Surface は背面 irradiance x スラブ透過のアンビエント透過。
//  エミッシブは呼び出し側で 1 回だけ加算する。
// -------------------------------------------------------------
float3 SubstrateEnvLightingWithReflection(
    FSubstrateBSDF BSDF,
    float3 N, float3 V,
    float Occlusion,
    float3 ReflectionOverride,
    float ReflectionWeight)
{
    float NoV = max(dot(N, V), 1e-4f);
    float3 R = reflect(-V, N);
    float3 F90 = SubstrateComputeF90(BSDF.F0, BSDF.F90);

    // --- Diffuse (1 tap) ---
    float3 Irradiance = IrradianceCube.Sample(Sampler2, N).rgb;
    float3 kD = 1.0f - SubstrateSchlickFresnel(BSDF.F0, F90, NoV);
    float3 Diffuse = kD * Irradiance * BSDF.DiffuseAlbedo;

    // --- Specular split-sum (F0/F90 一般化) ---
    // Lumen Reflections はプレフィルタサンプルをトレース結果へ差し替える
    // (第 1 ローブのみ。BRDF / フレネル重みは既存経路のまま)
    float2 EnvBRDF = BRDFLut.Sample(Sampler2, float2(NoV, BSDF.Roughness)).rg;
    float3 Prefiltered = PrefilterCube.SampleLevel(
        Sampler2, R, BSDF.Roughness * PREFILTER_MAX_MIP).rgb;
    Prefiltered = lerp(Prefiltered, ReflectionOverride, ReflectionWeight);
    float3 Specular = Prefiltered * (BSDF.F0 * EnvBRDF.x + F90 * EnvBRDF.y);

    [branch]
    if (BSDF.SecondRoughnessWeight > 0.0f)
    {
        float2 EnvBRDF2 = BRDFLut.Sample(Sampler2, float2(NoV, BSDF.SecondRoughness)).rg;
        float3 Prefiltered2 = PrefilterCube.SampleLevel(
            Sampler2, R, BSDF.SecondRoughness * PREFILTER_MAX_MIP).rgb;
        float3 Specular2 = Prefiltered2 * (BSDF.F0 * EnvBRDF2.x + F90 * EnvBRDF2.y);
        Specular = lerp(Specular, Specular2, BSDF.SecondRoughnessWeight);
    }

    [branch]
    if (BSDF.FuzzAmount > 0.0f)
    {
        // ファズの環境応答はシーン環境近似 (拡散 irradiance x 定数 DG)
        float3 FuzzEnv = BSDF.FuzzColor * Irradiance * 0.25f;
        Specular = lerp(Specular, FuzzEnv, BSDF.FuzzAmount);
    }

    // --- Sub-Surface アンビエント透過 ---
    float3 SSSAmbient = float3(0.0f, 0.0f, 0.0f);
    [branch]
    if (BSDF.SSSType == SUBSTRATE_SSS_TYPE_TWO_SIDED_WRAP
     || BSDF.SSSType == SUBSTRATE_SSS_TYPE_SIMPLEVOLUME)
    {
        float3 MFPEff = max(BSDF.SSSMFP * BSDF.SSSMFPScale, 1e-9f);
        float3 SlabTransmittance = exp(-BSDF.Thickness / MFPEff);
        float3 BackIrradiance = IrradianceCube.Sample(Sampler2, -N).rgb;
        SSSAmbient = kD * BackIrradiance * SlabTransmittance * BSDF.DiffuseAlbedo;
    }

    return (Diffuse + Specular + SSSAmbient) * Occlusion;
}

// 従来シグネチャ (差し替えなし = 既存挙動)
float3 SubstrateEnvLighting(
    FSubstrateBSDF BSDF,
    float3 N, float3 V,
    float Occlusion)
{
    return SubstrateEnvLightingWithReflection(BSDF, N, V, Occlusion,
        float3(0.0f, 0.0f, 0.0f), 0.0f);
}

// -------------------------------------------------------------
//  ビュー方向スラブ透過 (半透明 Substrate の背景着色に使用)
//  Colored Transmittance 相当: 視線が厚み分の媒質を斜めに
//  通過する Beer-Lambert。
// -------------------------------------------------------------
float3 SubstrateViewTransmittance(FSubstrateBSDF BSDF, float NoV)
{
    float3 MFPEff = max(BSDF.SSSMFP * BSDF.SSSMFPScale, 1e-9f);
    return exp(-(BSDF.Thickness / max(NoV, 0.05f)) / MFPEff);
}

#endif
