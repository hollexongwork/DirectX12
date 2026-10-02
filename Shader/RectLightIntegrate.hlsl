#ifndef RECT_LIGHT_INTEGRATE_HLSL
#define RECT_LIGHT_INTEGRATE_HLSL

#include "ShadingModels.hlsl"

// =============================================================
//  RectLightIntegrate
//  RectLightIntegrate.ush 相当。レクトライトを積分コンテキストへ落とし、
//  BxDF を 1 回評価する。拡散は多角形の放射照度 (RectIrradianceLambert)、
//  スペキュラは BxDF 側が LTC (RectGGXApproxLTC) で評価する。
// =============================================================

FAreaLightIntegrateContext CreateRectIntegrateContext(float Roughness, float3 N, float3 V, FRect Rect)
{
    float NoL = 0.0f;
    float Falloff = 0.0f;

    FAreaLightIntegrateContext Out = InitAreaLightIntegrateContext();

    float3 L = RectIrradianceLambert(N, Rect, Falloff, NoL);

    Out.AreaLight.SphereSinAlpha = 0.0f;
    Out.AreaLight.SphereSinAlphaSoft = 0.0f;
    Out.AreaLight.LineCosSubtended = 1.0f;
    Out.AreaLight.FalloffColor = float3(1.0f, 1.0f, 1.0f);
    Out.AreaLight.Rect = Rect;
    Out.AreaLight.bIsRect = true;
    Out.L = L;
    Out.NoL = NoL;
    Out.Falloff = Falloff;

    return Out;
}

FDirectLighting IntegrateBxDF(FGBufferData GBuffer, float3 N, float3 V, FRect Rect, FShadowTerms Shadow)
{
    FDirectLighting Out = (FDirectLighting) 0;

    // バーンドアに完全に隠れていれば何も足さない
    [branch]
    if (IsRectVisible(Rect))
    {
        FAreaLightIntegrateContext Context = CreateRectIntegrateContext(GBuffer.Roughness, N, V, Rect);
        GBuffer.Roughness = max(GBuffer.Roughness, MIN_ROUGHNESS);
        Out = IntegrateBxDF(GBuffer, N, V, Context.L, Context.Falloff, Context.NoL, Context.AreaLight, Shadow);
    }

    return Out;
}

#endif
