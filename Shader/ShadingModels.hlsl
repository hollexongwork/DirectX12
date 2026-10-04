#ifndef SHADING_MODELS_HLSL
#define SHADING_MODELS_HLSL

#include "PBR_Utility.hlsl"
#include "DeferredShadingCommon.hlsl"
#include "AreaLightCommon.hlsl"
#include "RectLightLTC.hlsl"

// =============================================================
//  ShadingModels
//  面光源の積分コンテキスト
//  (L / NoL / Falloff / FAreaLight) を受け取ってサーフェスの BxDF を評価する。
//
//  ライト側の枠組みは SphereMaxNoH / EnergyNormalization / LTC。
//  BxDF のカーネル (拡散の (1 - F) / Smith-Schlick の G / Schlick の F) は
//  既存のマテリアルモデル (PBR_Utility.hlsl) をそのまま使う。
//  光源形状を持たないライト (SourceRadius = 0 など) の結果は従来の
//  CookTorrance と一致する。
// =============================================================

// ライト 1 灯の直接光 (ライト色とシャドウを掛ける前)
struct FDirectLighting
{
    float3 Diffuse;
    float3 Specular;
    float3 Transmission;
};

// ライト 1 灯のシャドウ係数 (1 = 影なし)
struct FShadowTerms
{
    float SurfaceShadow; // サーフェスのシャドウ
    float TransmissionShadow; // 透過光のシャドウ (本エンジンは SurfaceShadow と同値)
    float TransmissionThickness;
};

// ラフネスの下限。解析ライトのハイライトが点に潰れるのを防ぐ
static const float MIN_ROUGHNESS = 0.02f;

// 球光源の見かけ角で GGX の a2 を広げる (New_a2)
float New_a2(float a2, float SinAlpha, float VoH)
{
    return a2 + 0.25f * SinAlpha * (3.0f * sqrt(a2) + SinAlpha) / (VoH + 0.001f);
}

// -------------------------------------------------------------
//  面光源のエネルギー正規化 (EnergyNormalization)
//  光源が大きいほどハイライトが広がるので、広がった分だけ明るさを落とす。
//    SphereSinAlphaSoft : a2 を広げるだけ (正規化しない = 柔らかくなるだけ)
//    SphereSinAlpha     : 球光源。a2 / 広げた a2
//    LineCosSubtended   : チューブ。1 軸方向だけ広がるので平方根
//  a2 は SoftSourceRadius 分だけ書き換わる。
// -------------------------------------------------------------
float EnergyNormalization(inout float a2, float VoH, FAreaLight AreaLight)
{
    if (AreaLight.SphereSinAlphaSoft > 0.0f)
    {
        // ラフネスを広げる
        a2 = saturate(a2 + Pow2(AreaLight.SphereSinAlphaSoft) / (VoH * 3.6f + 0.4f));
    }

    float Sphere_a2 = a2;
    float Energy = 1.0f;
    if (AreaLight.SphereSinAlpha > 0.0f)
    {
        Sphere_a2 = New_a2(a2, AreaLight.SphereSinAlpha, VoH);
        Energy = a2 / Sphere_a2;
    }

    if (AreaLight.LineCosSubtended < 1.0f)
    {
        float LineCosTwoAlpha = AreaLight.LineCosSubtended;
        float LineTanAlpha = sqrt((1.0001f - LineCosTwoAlpha) / (1.0f + LineCosTwoAlpha));
        float Line_a2 = New_a2(Sphere_a2, LineTanAlpha, VoH);
        Energy *= sqrt(Sphere_a2 / Line_a2);
    }

    return Energy;
}

// -------------------------------------------------------------
//  GGX スペキュラ (SpecularGGX)。戻り値は D * Vis * F (NoL は掛けない)。
//  D は面光源のエネルギー正規化込み。Vis / F は既存カーネル。
// -------------------------------------------------------------
float3 SpecularGGX(float Roughness, float3 SpecularColor, BxDFContext Context, float NoL, FAreaLight AreaLight)
{
    float a2 = Pow4(Roughness);
    float Energy = EnergyNormalization(a2, Context.VoH, AreaLight);

    float D = D_GGX(a2, Context.NoH) * Energy;

    // 既存の SmithGeometry はラフネスを受け取るので、広げた後の a2 から戻す
    float RoughnessG = sqrt(sqrt(a2));
    float Vis = SmithGeometry(Context.NoV, NoL, RoughnessG) / max(4.0f * Context.NoV * NoL, EPSILON);
    float3 F = SchlickFresnel(SpecularColor, Context.VoH);

    return (D * Vis) * F;
}

// -------------------------------------------------------------
//  DefaultLitBxDF
//    L / Falloff / NoL / AreaLight : 面光源の積分コンテキスト
//  拡散はフォールオフ x NoL、スペキュラは球 / チューブなら GGX、
//  レクトライトなら LTC。
// -------------------------------------------------------------
FDirectLighting DefaultLitBxDF(FGBufferData GBuffer, float3 N, float3 V, float3 L, float Falloff, float NoL, FAreaLight AreaLight, FShadowTerms Shadow)
{
    BxDFContext Context;
    Init(Context, N, V, L);

    // 拡散へ回るエネルギー (1 - F) は、光源形状で曲げる前のハーフベクトルで評価する (既存カーネル)
    float3 DiffuseFresnel = SchlickFresnel(GBuffer.SpecularColor, Context.VoH);

    SphereMaxNoH(Context, AreaLight.SphereSinAlpha, true);
    Context.NoV = saturate(abs(Context.NoV) + 1e-5f);

    FDirectLighting Lighting;

    Lighting.Diffuse = (1.0f - DiffuseFresnel) * GBuffer.DiffuseColor * INV_PI;
    Lighting.Diffuse *= AreaLight.FalloffColor * (Falloff * NoL);

    [branch]
    if (IsRectLight(AreaLight))
    {
        Lighting.Specular = RectGGXApproxLTC(GBuffer.Roughness, GBuffer.SpecularColor, N, V, AreaLight.Rect);
    }
    else
    {
        Lighting.Specular = AreaLight.FalloffColor * (Falloff * NoL) * SpecularGGX(GBuffer.Roughness, GBuffer.SpecularColor, Context, NoL, AreaLight);
    }

    Lighting.Transmission = float3(0.0f, 0.0f, 0.0f);
    return Lighting;
}

// シェーディングモデルごとの BxDF へ振り分ける (本エンジンは DefaultLit のみ)
FDirectLighting IntegrateBxDF(FGBufferData GBuffer, float3 N, float3 V, float3 L, float Falloff, float NoL, FAreaLight AreaLight, FShadowTerms Shadow)
{
    return DefaultLitBxDF(GBuffer, N, V, L, Falloff, NoL, AreaLight, Shadow);
}

#endif
