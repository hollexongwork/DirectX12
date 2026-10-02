#ifndef BRDF_HLSL
#define BRDF_HLSL

#include "Constant.hlsl"

// =============================================================
//  BRDF
//  BRDF.ush のうち、ライトの枠組みが使う部分
//  (BxDFContext / SphereMaxNoH / D_GGX)。
//  サーフェスの BxDF カーネル本体 (既存の GGX_NDF / SmithGeometry /
//  SchlickFresnel) は PBR_Utility.hlsl に残す。
//  レジスタを宣言しない (グラフィックス / コンピュート共用)。
// =============================================================

float Square(float x)
{
    return x * x;
}

float Pow2(float x)
{
    return x * x;
}

float Pow4(float x)
{
    float xx = x * x;
    return xx * xx;
}

float Pow5(float x)
{
    float xx = x * x;
    return xx * xx * x;
}

// -------------------------------------------------------------
//  BxDFContext
//  ライト 1 灯に対する N / V / L の内積一式。
// -------------------------------------------------------------
struct BxDFContext
{
    float NoV;
    float NoL;
    float VoL;
    float NoH;
    float VoH;
};

void Init(inout BxDFContext Context, float3 N, float3 V, float3 L)
{
    Context.NoL = dot(N, L);
    Context.NoV = dot(N, V);
    Context.VoL = dot(V, L);
    float InvLenH = rsqrt(2.0f + 2.0f * Context.VoL);
    Context.NoH = saturate((Context.NoL + Context.NoV) * InvLenH);
    Context.VoH = saturate(InvLenH + InvLenH * Context.VoL);
}

// -------------------------------------------------------------
//  SphereMaxNoH
//  球光源 (見かけの半角 α, SinAlpha = sin α) の円盤内で N・H が最大になる
//  方向へ L を寄せる。球光源のハイライトの形を正しく出すためのもの。
//  [ de Carpentier 2017, "Decima Engine: Advances in Lighting and AA" ]
//    bNewtonIteration : ニュートン法を 1 回かけて精度を上げる
// -------------------------------------------------------------
void SphereMaxNoH(inout BxDFContext Context, float SinAlpha, bool bNewtonIteration)
{
    if (SinAlpha > 0.0f)
    {
        float CosAlpha = sqrt(1.0f - Pow2(SinAlpha));

        float RoL = 2.0f * Context.NoL * Context.NoV - Context.VoL;
        if (RoL >= CosAlpha)
        {
            // 反射ベクトルが光源の円盤内: ハイライトの中心
            Context.NoH = 1.0f;
            Context.VoH = abs(Context.NoV);
        }
        else
        {
            float rInvLengthT = SinAlpha * rsqrt(1.0f - RoL * RoL);
            float NoTr = rInvLengthT * (Context.NoV - RoL * Context.NoL);
            float VoTr = rInvLengthT * (2.0f * Context.NoV * Context.NoV - 1.0f - RoL * Context.VoL);

            if (bNewtonIteration)
            {
                // dot( cross(N,L), V )
                float NxLoV = sqrt(saturate(1.0f - Pow2(Context.NoL) - Pow2(Context.NoV) - Pow2(Context.VoL) + 2.0f * Context.NoL * Context.NoV * Context.VoL));

                float NoBr = rInvLengthT * NxLoV;
                float VoBr = rInvLengthT * NxLoV * 2.0f * Context.NoV;

                float NoLVTr = Context.NoL * CosAlpha + Context.NoV + NoTr;
                float VoLVTr = Context.VoL * CosAlpha + 1.0f + VoTr;

                float p = NoBr * VoLVTr;
                float q = NoLVTr * VoLVTr;
                float s = VoBr * NoLVTr;

                float xNum = q * (-0.5f * p + 0.25f * VoBr * NoLVTr);
                float xDenom = p * p + s * (s - 2.0f * p) + NoLVTr * ((Context.NoL * CosAlpha + Context.NoV) * Pow2(VoLVTr) + q * (-0.5f * (VoLVTr + Context.VoL * CosAlpha) - 0.5f));
                float TwoX1 = 2.0f * xNum / (Pow2(xDenom) + Pow2(xNum));
                float SinTheta = TwoX1 * xDenom;
                float CosTheta = 1.0f - TwoX1 * xNum;
                NoTr = CosTheta * NoTr + SinTheta * NoBr;
                VoTr = CosTheta * VoTr + SinTheta * VoBr;
            }

            Context.NoL = Context.NoL * CosAlpha + NoTr; // dot( N, L * CosAlpha + T * SinAlpha )
            Context.VoL = Context.VoL * CosAlpha + VoTr;

            float InvLenH = rsqrt(2.0f + 2.0f * Context.VoL);
            Context.NoH = saturate((Context.NoL + Context.NoV) * InvLenH);
            Context.VoH = saturate(InvLenH + InvLenH * Context.VoL);
        }
    }
}

// GGX / Trowbridge-Reitz 法線分布 (a2 = α^2 = Roughness^4)
// [Walter et al. 2007, "Microfacet models for refraction through rough surfaces"]
float D_GGX(float a2, float NoH)
{
    float d = (NoH * a2 - NoH) * NoH + 1.0f;
    return a2 / (PI * d * d);
}

#endif
