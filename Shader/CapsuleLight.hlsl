#ifndef CAPSULE_LIGHT_HLSL
#define CAPSULE_LIGHT_HLSL

#include "BRDF.hlsl"

// =============================================================
//  CapsuleLight
//  球 / チューブ (カプセル) の面光源。
//  ポイント / スポットライトとディレクショナルライト (光源 = 円盤) が使う。
//  レジスタを宣言しない (グラフィックス / コンピュート共用)。
//
//  LightPos は受光点からの相対座標。チューブは LightPos[0] と [1] を
//  結ぶ線分で、ポイント (Length = 0) のときは両方がライトの中心。
// =============================================================

struct FCapsuleLight
{
    float3 LightPos[2]; // 線分の両端 (受光点基準)
    float Length; // チューブの長さ
    float Radius; // 球の半径
    float SoftRadius; // 柔らかさだけを足す半径
    float DistBiasSqr; // 逆二乗の特異点を避ける距離バイアスの 2 乗
};

// 線分上で点 (原点) に最も近い点
float3 ClosestPointLineToPoint(float3 Line0, float3 Line1, float Length)
{
    float3 Line01 = Line1 - Line0;
    return Line0 + Line01 * saturate(-dot(Line01, Line0) / Pow2(Length));
}

// 線分上でレイ (原点から方向 R) に最も近い点
float3 ClosestPointLineToRay(float3 Line0, float3 Line1, float Length, float3 R)
{
    float3 L0 = Line0;
    float3 L1 = Line1;
    float3 Line01 = Line1 - Line0;

    // 最短距離
    float A = Square(Length);
    float B = dot(R, Line01);
    float t = saturate(dot(Line0, B * R - Line01) / (A - B * B));

    return Line0 + t * Line01;
}

// -------------------------------------------------------------
//  線光源の放射照度 (LineIrradiance)
//  [ Karis 2013 ] の変形。
//    CosSubtended   : 線分が張る角度の cos
//    BaseIrradiance : 距離フォールオフ (線分の両端の距離の積で近似)
//    NoL            : 両端の方向の N・L の平均
// -------------------------------------------------------------
void LineIrradiance(float3 N, float3 Line0, float3 Line1, float DistanceBiasSqr, out float CosSubtended, out float BaseIrradiance, out float NoL)
{
    float LengthSqr0 = dot(Line0, Line0);
    float LengthSqr1 = dot(Line1, Line1);
    float InvLength0 = rsqrt(LengthSqr0);
    float InvLength1 = rsqrt(LengthSqr1);
    float InvLength01 = InvLength0 * InvLength1;

    CosSubtended = dot(Line0, Line1) * InvLength01;
    BaseIrradiance = InvLength01 / (CosSubtended * 0.5f + 0.5f + DistanceBiasSqr * InvLength01);
    NoL = 0.5f * (dot(N, Line0) * InvLength0 + dot(N, Line1) * InvLength1);
}

// -------------------------------------------------------------
//  球光源の地平線での N・L の回り込み (SphereHorizonCosWrap)
//  球が地平線にかかるとき、見えている部分の放射照度を
//  エルミートスプラインで近似する (SinAlpha < 0.8 でほぼ正確)。
//    SinAlphaSqr : 球の見かけの半角の sin の 2 乗
// -------------------------------------------------------------
float SphereHorizonCosWrap(float NoL, float SinAlphaSqr)
{
    float SinAlpha = sqrt(SinAlphaSqr);

    if (NoL < SinAlpha)
    {
        NoL = max(NoL, -SinAlpha);

        // y0 = 0, y1 = SinAlpha, dy0 = 0, dy1 = 1 のエルミートスプライン
        NoL = Pow2(SinAlpha + NoL) / (4.0f * SinAlpha);
    }

    return NoL;
}

// -------------------------------------------------------------
//  面光源の形状だけを考慮したフォールオフ (IntegrateLight)
//  BxDF を伴わない用途 (Lumen の Surface Cache / Volumetric Fog) が使う。
// -------------------------------------------------------------
float IntegrateLight(FCapsuleLight Capsule, bool bInverseSquared)
{
    float Falloff;

    [branch]
    if (Capsule.Length > 0.0f)
    {
        float NoL;
        float LineCosSubtended = 1.0f;
        LineIrradiance(float3(0.0f, 0.0f, 0.0f), Capsule.LightPos[0], Capsule.LightPos[1], Capsule.DistBiasSqr, LineCosSubtended, Falloff, NoL);
    }
    else
    {
        float3 ToLight = Capsule.LightPos[0];
        float DistSqr = dot(ToLight, ToLight);
        Falloff = rcp(DistSqr + Capsule.DistBiasSqr);
    }

    Falloff = bInverseSquared ? Falloff : 1.0f;

    return Falloff;
}

#endif
