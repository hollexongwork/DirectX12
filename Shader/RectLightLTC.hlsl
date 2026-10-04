#ifndef RECT_LIGHT_LTC_HLSL
#define RECT_LIGHT_LTC_HLSL

#include "Resources.hlsl"
#include "RectLight.hlsl"

// =============================================================
//  RectLightLTC
//  RectGGXApproxLTC。レクトライトのスペキュラを
//  LTC (Linearly Transformed Cosines) で評価する。
//  [ Heitz et al. 2016, "Real-Time Polygonal-Light Shading with Linearly Transformed Cosines" ]
//
//  LTC テクスチャ (t37 / t38) を読むので、
//  レジスタに依存しない RectLight.hlsl から分けてある。
//
//  GGX のローブを「クランプコサイン分布を行列で変換したもの」で近似する。
//  矩形を逆行列で変換すれば、コサイン分布に対する多角形の積分
//  (PolygonIrradiance) でローブの積分が解析的に求まる。
//    LTCMatTexture : 逆行列の 4 成分 (UV = (Roughness, sqrt(1 - NoV)))
//    LTCAmpTexture : x = ローブの大きさ (F0 = 1 の方向アルベド), y = フレネル項
// =============================================================

// F0 / F90 を指定する版 (Substrate Slab 用)
float3 RectGGXApproxLTC(float Roughness, float3 F0, float3 F90, float3 N, float3 V, FRect Rect)
{
    // バーンドアに完全に隠れた
    if (!IsRectVisible(Rect))
    {
        return float3(0.0f, 0.0f, 0.0f);
    }

    float NoV = saturate(abs(dot(N, V)) + 1e-5f);

    float2 UV = float2(Roughness, sqrt(1.0f - NoV));
    UV = UV * (63.0f / 64.0f) + (0.5f / 64.0f);

    float4 LTCMat = LTCMatTexture.SampleLevel(Sampler2, UV, 0.0f);
    float2 LTCAmp = LTCAmpTexture.SampleLevel(Sampler2, UV, 0.0f);

    float3x3 LTC =
    {
        float3(LTCMat.x, 0.0f, LTCMat.z),
        float3(0.0f, 1.0f, 0.0f),
        float3(LTCMat.y, 0.0f, LTCMat.w)
    };

    // 接空間へ回す (T1 = V の接平面成分)
    float3 T1 = V - N * dot(N, V);
    float T1LengthSqr = dot(T1, T1);
    // 真上から見ているとき (V = N) は接線が定まらないので任意の方向を使う (ローブは回転対称)
    T1 = (T1LengthSqr > 1e-8f) ? T1 * rsqrt(T1LengthSqr) : normalize(cross(N, (abs(N.y) < 0.99f) ? float3(0.0f, 1.0f, 0.0f) : float3(1.0f, 0.0f, 0.0f)));
    float3 T2 = cross(N, T1);
    float3x3 TangentBasis = float3x3(T1, T2, N);

    LTC = mul(LTC, TangentBasis);

    float3 Poly[4];
    Poly[0] = mul(LTC, Rect.Origin - Rect.Axis[0] * Rect.Extent.x - Rect.Axis[1] * Rect.Extent.y);
    Poly[1] = mul(LTC, Rect.Origin + Rect.Axis[0] * Rect.Extent.x - Rect.Axis[1] * Rect.Extent.y);
    Poly[2] = mul(LTC, Rect.Origin + Rect.Axis[0] * Rect.Extent.x + Rect.Axis[1] * Rect.Extent.y);
    Poly[3] = mul(LTC, Rect.Origin - Rect.Axis[0] * Rect.Extent.x + Rect.Axis[1] * Rect.Extent.y);

    // ベクトル放射照度
    float3 L = PolygonIrradiance(Poly);

    float LengthSqr = dot(L, L);
    float InvLength = rsqrt(max(LengthSqr, 1e-16f));
    float Length = LengthSqr * InvLength;

    // 平均の光源方向
    L *= InvLength;

    // 等価な球で考える:
    //   球のコサイン重み付き積分 = π * r^2 / d^2、SinAlphaSqr = r^2 / d^2
    // 放射照度 / π = クランプコサイン分布に対するフォームファクタ
    float SinAlphaSqr = Length * INV_PI;

    float NoL = SphereHorizonCosWrap(L.z, SinAlphaSqr);
    float Irradiance = SinAlphaSqr * NoL;

    // 負と NaN を落とす
    Irradiance = -min(-Irradiance, 0.0f);

    // Schlick のフレネルをローブで積分した結果: F0 * (大きさ - フレネル項) + F90 * フレネル項
    float3 SpecularColor = F90 * LTCAmp.y + (LTCAmp.x - LTCAmp.y) * F0;

    return Irradiance * SpecularColor;
}

// F90 = 1 の版 (レガシー経路)
float3 RectGGXApproxLTC(float Roughness, float3 SpecularColor, float3 N, float3 V, FRect Rect)
{
    return RectGGXApproxLTC(Roughness, SpecularColor, float3(1.0f, 1.0f, 1.0f), N, V, Rect);
}

#endif
