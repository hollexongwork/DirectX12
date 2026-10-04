#ifndef RECT_LIGHT_HLSL
#define RECT_LIGHT_HLSL

#include "CapsuleLight.hlsl"

// =============================================================
//  RectLight
//  矩形の面光源。
//  拡散は多角形の放射照度の解析解 (Lambert の公式)、スペキュラは
//  LTC (RectLightIntegrate.hlsl の RectGGXApproxLTC) で評価する。
//  レジスタを宣言しない (グラフィックス / コンピュート共用)。
//
//  FRect は受光点を原点とする矩形:
//    Origin  = 受光点 -> 矩形の中心
//    Axis[0] = 幅軸、Axis[1] = 高さ軸 (ライトの Tangent)、
//    Axis[2] = ライトの Direction (発光方向の逆。受光点から見て矩形の奥向き)
//    Extent  = 半幅 / 半高
// =============================================================

struct FRect
{
    float3 Origin;
    float3x3 Axis;
    float2 Extent; // 受光点から見える部分の半幅 / 半高 (バーンドアで切った後)
    float2 FullExtent; // 元の半幅 / 半高
    float2 Offset; // 見える部分の中心の、元の中心からのずれ (符号反転)
};

// バーンドアに完全に隠れていないか
bool IsRectVisible(FRect Rect)
{
    return Rect.Extent.x != 0.0f && Rect.Extent.y != 0.0f;
}

// -------------------------------------------------------------
//  GetRect
//  ライトのパラメータから受光点基準の矩形を作る。
//  bComputeVisibleRect が真でバーンドアが有効 (cos > 0.035 = 88 度未満) なら、
//  受光点から 4 枚のドア越しに見える部分へ矩形を切り詰める
//  (中心と大きさが変わる。平面は同じ)。
//
//  バーンドアは発光面の各辺から外へ角度 θ (BarnCosAngle = cos θ) で
//  長さ BarnLength だけ張り出す板。軸ごとに独立に扱う:
//  受光点とドアの先端を通る直線が発光面と交わる位置までが見える範囲。
//  受光点がドアの先端より発光面に近いときは、ドアを受光点の深さで切って考える。
// -------------------------------------------------------------
FRect GetRect(
    float3 ToLight,
    float3 LightDataDirection,
    float3 LightDataTangent,
    float LightDataSourceRadius,
    float LightDataSourceLength,
    float LightDataRectLightBarnCosAngle,
    float LightDataRectLightBarnLength,
    bool bComputeVisibleRect)
{
    FRect Rect;
    Rect.Origin = ToLight;
    Rect.Axis[1] = LightDataTangent;
    Rect.Axis[2] = LightDataDirection;
    Rect.Axis[0] = cross(Rect.Axis[1], Rect.Axis[2]);
    Rect.Extent = float2(LightDataSourceRadius, LightDataSourceLength);
    Rect.FullExtent = Rect.Extent;
    Rect.Offset = float2(0.0f, 0.0f);

    [branch]
    if (bComputeVisibleRect && LightDataRectLightBarnCosAngle > 0.035f)
    {
        // 受光点のライトローカル座標 (xy = 発光面内、Depth = 発光面からの前方距離)
        const float3 LightToPoint = -ToLight;
        const float2 LocalPosition = float2(dot(LightToPoint, Rect.Axis[0]), dot(LightToPoint, Rect.Axis[1]));
        const float Depth = -dot(LightToPoint, Rect.Axis[2]);

        const float CosTheta = LightDataRectLightBarnCosAngle;
        const float SinTheta = sqrt(saturate(1.0f - CosTheta * CosTheta));

        // ドアの先端の深さと横位置
        const float BarnDepth = min(Depth, CosTheta * LightDataRectLightBarnLength);
        const float2 BarnTip = Rect.FullExtent + BarnDepth * (SinTheta / CosTheta);

        // 受光点とドアの先端を通る直線が発光面 (Depth = 0) と交わる位置
        const float InvDepthRange = rcp(max(Depth - BarnDepth, 1e-5f));
        float2 MaxXY = (Depth * BarnTip - LocalPosition * BarnDepth) * InvDepthRange; // + 側のドアによる上限
        float2 MinXY = (-Depth * BarnTip - LocalPosition * BarnDepth) * InvDepthRange; // - 側のドアによる下限

        MinXY = max(MinXY, -Rect.FullExtent);
        MaxXY = min(MaxXY, Rect.FullExtent);

        if (MinXY.x >= MaxXY.x || MinXY.y >= MaxXY.y)
        {
            // ドアに完全に隠れた
            Rect.Extent = float2(0.0f, 0.0f);
        }
        else
        {
            const float2 RectOffset = 0.5f * (MinXY + MaxXY);

            Rect.Extent = 0.5f * (MaxXY - MinXY);
            Rect.Origin = Rect.Origin + Rect.Axis[0] * RectOffset.x + Rect.Axis[1] * RectOffset.y;
            Rect.Offset = -RectOffset;
        }
    }

    return Rect;
}

// -------------------------------------------------------------
//  多角形のベクトル放射照度 (PolygonIrradiance)
//  放射輝度 1 の四角形が受光点に作る放射照度を、向き付きのベクトルで返す
//  (Lambert の公式: 1/2 Σ θ_i n_i。θ_i は辺が張る角度、n_i は辺と受光点を
//   含む平面の法線)。θ / sin θ は多項式で近似する。
//  大きさは π x フォームファクタ。頂点は受光点から見て反時計回りに渡す。
// -------------------------------------------------------------
float3 PolygonIrradiance(float3 Poly[4])
{
    float3 L0 = normalize(Poly[0]);
    float3 L1 = normalize(Poly[1]);
    float3 L2 = normalize(Poly[2]);
    float3 L3 = normalize(Poly[3]);

    float c01 = dot(L0, L1);
    float c12 = dot(L1, L2);
    float c23 = dot(L2, L3);
    float c30 = dot(L3, L0);

    float w01 = (1.5708f - 0.175f * c01) * rsqrt(c01 + 1.0f);
    float w12 = (1.5708f - 0.175f * c12) * rsqrt(c12 + 1.0f);
    float w23 = (1.5708f - 0.175f * c23) * rsqrt(c23 + 1.0f);
    float w30 = (1.5708f - 0.175f * c30) * rsqrt(c30 + 1.0f);

    float3 L;
    L = cross(L1, -w01 * L0 + w12 * L2);
    L += cross(L3, w30 * L0 + -w23 * L2);

    // ベクトル放射照度
    return 0.5f * L;
}

// -------------------------------------------------------------
//  矩形の拡散放射照度 (RectIrradianceLambert)
//  [ Heitz et al. 2016, "Real-Time Polygonal-Light Shading with Linearly Transformed Cosines" ]
//    BaseIrradiance : 放射輝度 1 の矩形が、矩形の方を向いた面に作る放射照度
//    NoL            : 平均方向に対する N・L (地平線の回り込み込み)
//  戻り値は平均の光源方向。
// -------------------------------------------------------------
float3 RectIrradianceLambert(float3 N, FRect Rect, out float BaseIrradiance, out float NoL)
{
    // 矩形のローカル座標で見た中心 (受光点が原点)
    float3 LocalPosition;
    LocalPosition.x = dot(Rect.Axis[0], Rect.Origin);
    LocalPosition.y = dot(Rect.Axis[1], Rect.Origin);
    LocalPosition.z = dot(Rect.Axis[2], Rect.Origin);

    float x0 = LocalPosition.x - Rect.Extent.x;
    float x1 = LocalPosition.x + Rect.Extent.x;
    float y0 = LocalPosition.y - Rect.Extent.y;
    float y1 = LocalPosition.y + Rect.Extent.y;
    float z0 = LocalPosition.z;

    float3 Poly[4];
    Poly[0] = float3(x0, y0, z0);
    Poly[1] = float3(x1, y0, z0);
    Poly[2] = float3(x1, y1, z0);
    Poly[3] = float3(x0, y1, z0);

    float3 L = PolygonIrradiance(Poly);

    // ワールド空間へ戻す
    L = L.x * Rect.Axis[0] + L.y * Rect.Axis[1] + L.z * Rect.Axis[2];

    float LengthSqr = dot(L, L);
    float InvLength = rsqrt(max(LengthSqr, 1e-16f));
    float Length = LengthSqr * InvLength;

    // 平均の光源方向
    L *= InvLength;

    BaseIrradiance = Length;

    // 等価な球で考える:
    //   球のコサイン重み付き積分 = π * r^2 / d^2、SinAlphaSqr = r^2 / d^2
    float SinAlphaSqr = BaseIrradiance * INV_PI;

    NoL = SphereHorizonCosWrap(dot(N, L), SinAlphaSqr);

    return L;
}

// 矩形の形状だけを考慮したフォールオフ (IntegrateLight)。
// BxDF を伴わない用途 (Lumen の Surface Cache / Volumetric Fog) が使う
float IntegrateLight(FRect Rect)
{
    // バーンドアに完全に隠れた
    if (!IsRectVisible(Rect))
    {
        return 0.0f;
    }

    float NoL;
    float Falloff;
    RectIrradianceLambert(float3(0.0f, 0.0f, 0.0f), Rect, Falloff, NoL);

    return Falloff;
}

#endif
