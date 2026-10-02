#ifndef CAPSULE_LIGHT_INTEGRATE_HLSL
#define CAPSULE_LIGHT_INTEGRATE_HLSL

#include "ShadingModels.hlsl"

// =============================================================
//  CapsuleLightIntegrate
//  CapsuleLightIntegrate.ush 相当。球 / チューブの面光源を
//  「代表方向 + フォールオフ + 見かけ角」の積分コンテキストへ落とし、
//  BxDF を 1 回評価する。
// =============================================================

FAreaLightIntegrateContext CreateCapsuleIntegrateContext(float Roughness, float3 N, float3 V, FCapsuleLight Capsule, bool bInverseSquared)
{
    FAreaLightIntegrateContext Out = InitAreaLightIntegrateContext();

    float NoL;
    float Falloff;
    float LineCosSubtended = 1.0f;

    [branch]
    if (Capsule.Length > 0.0f)
    {
        LineIrradiance(N, Capsule.LightPos[0], Capsule.LightPos[1], Capsule.DistBiasSqr, LineCosSubtended, Falloff, NoL);
    }
    else
    {
        float DistSqr = dot(Capsule.LightPos[0], Capsule.LightPos[0]);
        Falloff = rcp(DistSqr + Capsule.DistBiasSqr);

        float3 L = Capsule.LightPos[0] * rsqrt(max(DistSqr, 1e-8f)); // 距離 0 の NaN を避ける下限 [PORT]
        NoL = dot(N, L);
    }

    if (Capsule.Radius > 0.0f)
    {
        // 球が地平線にかかるときの N・L の回り込み
        float SinAlphaSqr = saturate(Pow2(Capsule.Radius) * Falloff);
        NoL = SphereHorizonCosWrap(NoL, SinAlphaSqr);
    }

    NoL = saturate(NoL);
    Falloff = bInverseSquared ? Falloff : 1.0f;

    float3 ToLight = Capsule.LightPos[0];
    if (Capsule.Length > 0.0f)
    {
        float3 R = reflect(-V, N);

        // 反射レイに最も近い線分上の点をスペキュラの代表点にする
        ToLight = ClosestPointLineToRay(Capsule.LightPos[0], Capsule.LightPos[1], Capsule.Length, R);
    }

    float DistSqr = dot(ToLight, ToLight);
    float InvDist = rsqrt(max(DistSqr, 1e-8f));
    float3 L = ToLight * InvDist;

    Roughness = max(Roughness, MIN_ROUGHNESS);
    float a = Pow2(Roughness);

    Out.AreaLight.SphereSinAlpha = saturate(Capsule.Radius * InvDist * (1.0f - a));
    Out.AreaLight.SphereSinAlphaSoft = saturate(Capsule.SoftRadius * InvDist);
    Out.AreaLight.LineCosSubtended = LineCosSubtended;
    Out.AreaLight.FalloffColor = float3(1.0f, 1.0f, 1.0f);
    Out.AreaLight.Rect = (FRect) 0;
    Out.AreaLight.bIsRect = false;
    Out.NoL = NoL;
    Out.Falloff = Falloff;
    Out.L = L;
    return Out;
}

FDirectLighting IntegrateBxDF(FGBufferData GBuffer, float3 N, float3 V, FCapsuleLight Capsule, FShadowTerms Shadow, bool bInverseSquared)
{
    GBuffer.Roughness = max(GBuffer.Roughness, MIN_ROUGHNESS);
    FAreaLightIntegrateContext Context = CreateCapsuleIntegrateContext(GBuffer.Roughness, N, V, Capsule, bInverseSquared);
    return IntegrateBxDF(GBuffer, N, V, Context.L, Context.Falloff, Context.NoL, Context.AreaLight, Shadow);
}

#endif
