#ifndef DEFERRED_LIGHTING_COMMON_HLSL
#define DEFERRED_LIGHTING_COMMON_HLSL

#include "PBR_Utility.hlsl"

// =============================================================
//  DeferredLightingCommon
//  DynamicLightingCommon.ush / DeferredLightingCommon.ush /
//  CapsuleLight.ush に相当するローカルライト (Point / Spot / Rect)
//  の評価。
//
//  - 減衰 (逆二乗 + 半径窓 / 指数)
//  - 面光源 (球 / チューブ / 矩形) は Karis 2013 の
//    representative point 近似 (Rect のみ LTC 積分。
//    ここでは軽量な MRP + エネルギー正規化で統一)
//  - 距離の単位はメートル
//  - FLightShaderParameters.Direction は「発光方向」。
//
//  減衰・MRP のセットアップは FAreaLight + SetupAreaLight() に
//  分離してあり、レガシー BRDF (AreaLightBRDF) と Substrate Slab
//  (SubstrateEvaluation.hlsl) の両方が同じセットアップを共有する
//  (AreaLightCommon.ush の FAreaLight 相当)。
// =============================================================

float Square(float x)
{
    return x * x;
}

// -------------------------------------------------------------
//  半径ウィンドウマスク (GetLocalLightAttenuation の窓関数)
//  逆二乗の裾を AttenuationRadius で滑らかに 0 へ落とす。
//    ( 1 - (d^2 / r^2)^2 )^2
// -------------------------------------------------------------
float RadialAttenuationMask(float DistanceSqr, float InvRadius)
{
    return Square(saturate(1.0f - Square(DistanceSqr * Square(InvRadius))));
}

// -------------------------------------------------------------
//  旧式指数フォールオフ (RadialAttenuation)
//  bUseInverseSquaredFalloff = false のとき距離応答全体を担う。
//  WorldLightVector = ToLight * InvRadius (半径で正規化済み)
// -------------------------------------------------------------
float RadialAttenuation(float3 WorldLightVector, float FalloffExponent)
{
    float NormalizeDistanceSquared = dot(WorldLightVector, WorldLightVector);
    return pow(1.0f - saturate(NormalizeDistanceSquared), FalloffExponent);
}

// -------------------------------------------------------------
//  スポットコーン減衰 (SpotAttenuation)
//  SpotAngles.x = cos(Outer), SpotAngles.y = 1 / (cos(Inner) - cos(Outer))
//  L = 受光点 -> ライト方向。-L とコーン軸 (発光方向) の角度で減衰。
// -------------------------------------------------------------
float SpotAttenuation(float3 L, float3 EmissionDirection, float2 SpotAngles)
{
    return Square(saturate((dot(-L, EmissionDirection) - SpotAngles.x) * SpotAngles.y));
}

// -------------------------------------------------------------
//  レイ (原点, 方向 R) に最も近い線分 [L0, L1] 上の点 (Karis 2013)
//  L0 / L1 は受光点からの相対座標。
// -------------------------------------------------------------
float3 ClosestPointOnSegmentToRay(float3 L0, float3 L1, float3 R)
{
    float3 Ld = L1 - L0;
    float RoLd = dot(R, Ld);
    float Denominator = dot(Ld, Ld) - RoLd * RoLd;
    float t = (RoLd * dot(R, L0) - dot(L0, Ld)) / max(Denominator, 1e-6f);
    return L0 + Ld * saturate(t);
}

// -------------------------------------------------------------
//  原点 (受光点) に最も近い線分 [L0, L1] 上の点
// -------------------------------------------------------------
float3 ClosestPointOnSegment(float3 L0, float3 L1)
{
    float3 Ld = L1 - L0;
    float t = dot(-L0, Ld) / max(dot(Ld, Ld), 1e-6f);
    return L0 + Ld * saturate(t);
}

// =============================================================
//  FAreaLight (AreaLightCommon.ush の FAreaLight 相当)
//  ローカルライト 1 灯のセットアップ結果:
//  減衰マスク + Diffuse / Specular 代表方向 + フォールオフ +
//  面光源の見かけ角。BRDF 非依存なので、レガシー Cook-Torrance と
//  Substrate Slab が共有する。
// =============================================================
struct FAreaLight
{
    float3 DiffuseL; // Diffuse 代表方向 (受光点 -> 光源)
    float3 SpecularL; // Specular 代表方向 (受光点 -> 光源)
    float DiffuseFalloff; // Diffuse 距離フォールオフ
    float SpecularFalloff; // Specular 距離フォールオフ
    float SphereSinAlpha; // 光源の見かけ角 sin (Karis 正規化用)
    float SoftSinAlpha; // SoftSourceRadius 分の追加見かけ角
    float LightMask; // 半径窓 / 指数 / コーン / 発光面の向き
};

// -------------------------------------------------------------
//  SetupAreaLight
//  減衰マスクと代表点 (MRP) を計算する。マスクが 0 (完全遮蔽 /
//  範囲外) のときは false を返す。数式は従来 IntegrateLocalLight
//  内のものを逐語移動 (挙動不変)。
// -------------------------------------------------------------
bool SetupAreaLight(
    FLightShaderParameters Light,
    float3 WorldPos, float3 N, float3 V,
    out FAreaLight OutAreaLight)
{
    OutAreaLight = (FAreaLight)0;

    float3 ToLight = Light.Position - WorldPos;
    float DistanceSqr = dot(ToLight, ToLight);
    float3 L = ToLight * rsqrt(max(DistanceSqr, 1e-8f));

    // ------------------------------------------------------------
    //  減衰マスク (半径窓 / 指数 / コーン / 発光面の向き)
    // ------------------------------------------------------------
    float LightMask;
    if (Light.Flags & LIGHT_FLAG_INVERSE_SQUARED)
    {
        LightMask = RadialAttenuationMask(DistanceSqr, Light.InvRadius);
    }
    else
    {
        LightMask = RadialAttenuation(ToLight * Light.InvRadius, Light.FalloffExponent);
    }

    if (Light.Type == LIGHT_TYPE_SPOT)
    {
        LightMask *= SpotAttenuation(L, Light.Direction, Light.SpotAngles);
    }
    else if (Light.Type == LIGHT_TYPE_RECT)
    {
        // 発光面の裏側は照らさない
        LightMask = (dot(Light.Direction, -L) > 0.0f) ? LightMask : 0.0f;
    }

    [branch]
    if (LightMask <= 0.0f)
    {
        return false;
    }

    // ------------------------------------------------------------
    //  代表点 (MRP) — 面光源は Diffuse / Specular で別の方向・距離
    // ------------------------------------------------------------
    float3 DiffuseL = L;
    float DiffuseDistSqr = DistanceSqr;
    float3 SpecularL = L;
    float SpecularDistSqr = DistanceSqr;
    float SphereSinAlpha = 0.0f;

    [branch]
    if (Light.Type == LIGHT_TYPE_RECT)
    {
        // ---- 矩形面光源 (SourceRadius = 半幅, SourceLength = 半高) ----
        float3 AxisX = Light.Tangent;                             // 幅軸
        float3 AxisY = normalize(cross(Light.Direction, AxisX));  // 高さ軸

        float2 HalfExtents = float2(max(Light.SourceRadius, 1e-3f), max(Light.SourceLength, 1e-3f));

        float3 LightToPoint = -ToLight;                           // ライト -> 受光点
        float Depth = dot(LightToPoint, Light.Direction);         // 発光面前方の距離 (表側なので > 0)

        // ---- バーンドアによる可視矩形のクリップ (対称近似) ----
        // ドア先端 = 横 (半幅 + L*sinθ) / 前方 (L*cosθ)。受光点から
        // 先端を通る直線が光面 (depth 0) と交わる横位置まで有効半幅を
        // 縮める。
        // cos > 0.035 (≒88 度未満) のときのみ有効 — 既定 88 度は全開。
        [branch]
        if (Light.RectLightBarnCosAngle > 0.035f)
        {
            float BarnCos = Light.RectLightBarnCosAngle;
            float BarnSin = sqrt(saturate(1.0f - BarnCos * BarnCos));

            float2 Lateral = abs(float2(dot(LightToPoint, AxisX), dot(LightToPoint, AxisY)));
            float2 TipLateral = HalfExtents + Light.RectLightBarnLength * BarnSin;
            float TipDepth = Light.RectLightBarnLength * BarnCos;

            float2 VisibleExtent = TipLateral
                + (Lateral - TipLateral) * (-TipDepth) / max(Depth - TipDepth, 1e-4f);

            // 可視半幅が消えたら完全遮蔽 (ドアの影は意図的にハードエッジ)
            if (VisibleExtent.x <= 0.0f || VisibleExtent.y <= 0.0f)
            {
                return false;
            }
            HalfExtents = clamp(VisibleExtent, float2(1e-3f, 1e-3f), HalfExtents);
        }

        // ---- Diffuse: 矩形上の最近接点を代表点にする ----
        float2 PlanePos = float2(dot(LightToPoint, AxisX), dot(LightToPoint, AxisY));
        float2 NearestPlane = clamp(PlanePos, -HalfExtents, HalfExtents);
        float3 NearestPoint = Light.Position + AxisX * NearestPlane.x + AxisY * NearestPlane.y;

        float3 ToNearest = NearestPoint - WorldPos;
        DiffuseDistSqr = dot(ToNearest, ToNearest);
        DiffuseL = ToNearest * rsqrt(max(DiffuseDistSqr, 1e-8f));

        // ---- Specular: 反射レイと光面の交点を矩形へクランプ ----
        float3 R = reflect(-V, N);
        float RoD = dot(R, Light.Direction);
        float3 SpecPoint = NearestPoint;
        [branch]
        if (RoD < -1e-4f)   // レイが発光面へ向かうときのみ交点あり
        {
            // 平面: dot(X - Position, Direction) = 0 との交点
            float t = dot(ToLight, Light.Direction) / RoD;   // 分子分母とも負 -> t > 0
            float3 HitLocal = (WorldPos + R * t) - Light.Position;
            float2 HitPlane = float2(dot(HitLocal, AxisX), dot(HitLocal, AxisY));
            float2 ClampedHit = clamp(HitPlane, -HalfExtents, HalfExtents);
            SpecPoint = Light.Position + AxisX * ClampedHit.x + AxisY * ClampedHit.y;
        }

        float3 ToSpec = SpecPoint - WorldPos;
        SpecularDistSqr = dot(ToSpec, ToSpec);
        SpecularL = ToSpec * rsqrt(max(SpecularDistSqr, 1e-8f));

        // 見かけ角は平均半径の球で近似
        float EffectiveRadius = 0.5f * (HalfExtents.x + HalfExtents.y);
        SphereSinAlpha = saturate(EffectiveRadius * rsqrt(max(SpecularDistSqr, 1e-8f)));

        // 発光面はランバート発光 (正面ほど明るいコサイン分布)
        LightMask *= saturate(dot(Light.Direction, -DiffuseL));
    }
    else if (Light.SourceLength > 0.0f || Light.SourceRadius > 0.0f)
    {
        // ---- Point / Spot の球・チューブ光源 (Karis representative point) ----
        float3 R = reflect(-V, N);
        float3 SpecToLight = ToLight;

        [branch]
        if (Light.SourceLength > 0.0f)
        {
            // チューブ: 軸 = Tangent (コンポーネント +X)、長さ = SourceLength
            float3 HalfSeg = Light.Tangent * (Light.SourceLength * 0.5f);
            float3 L0 = ToLight - HalfSeg;
            float3 L1 = ToLight + HalfSeg;

            // Diffuse は受光点への最近接点
            float3 NearestSeg = ClosestPointOnSegment(L0, L1);
            DiffuseDistSqr = dot(NearestSeg, NearestSeg);
            DiffuseL = NearestSeg * rsqrt(max(DiffuseDistSqr, 1e-8f));

            // Specular は反射レイへの最近接点
            SpecToLight = ClosestPointOnSegmentToRay(L0, L1, R);
        }

        // 球: 反射レイに最も近い球面上の点へ寄せる (Karis 2013)
        [branch]
        if (Light.SourceRadius > 0.0f)
        {
            float3 CenterToRay = dot(SpecToLight, R) * R - SpecToLight;
            SpecToLight = SpecToLight + CenterToRay
                * saturate(Light.SourceRadius * rsqrt(max(dot(CenterToRay, CenterToRay), 1e-8f)));
        }

        SpecularDistSqr = dot(SpecToLight, SpecToLight);
        SpecularL = SpecToLight * rsqrt(max(SpecularDistSqr, 1e-8f));
        SphereSinAlpha = saturate(Light.SourceRadius * rsqrt(max(SpecularDistSqr, 1e-8f)));
    }

    // SoftSourceRadius はハイライトを広げるだけ (エネルギー正規化なし)
    float SoftSinAlpha = saturate(Light.SoftSourceRadius * rsqrt(max(SpecularDistSqr, 1e-8f)));

    // ------------------------------------------------------------
    //  距離フォールオフ
    // ------------------------------------------------------------
    float DiffuseFalloff = 1.0f;
    float SpecularFalloff = 1.0f;
    if (Light.Flags & LIGHT_FLAG_INVERSE_SQUARED)
    {
        // 逆二乗。光源サイズ由来の下限で特異点を回避
        float DistBiasSqr = max(Square(Light.SourceRadius), 1e-4f);
        DiffuseFalloff = rcp(DiffuseDistSqr + DistBiasSqr);
        SpecularFalloff = rcp(SpecularDistSqr + DistBiasSqr);
    }
    // (旧式フォールオフは RadialAttenuation が距離応答全体を担うので 1)

    OutAreaLight.DiffuseL = DiffuseL;
    OutAreaLight.SpecularL = SpecularL;
    OutAreaLight.DiffuseFalloff = DiffuseFalloff;
    OutAreaLight.SpecularFalloff = SpecularFalloff;
    OutAreaLight.SphereSinAlpha = SphereSinAlpha;
    OutAreaLight.SoftSinAlpha = SoftSinAlpha;
    OutAreaLight.LightMask = LightMask;
    return true;
}

// -------------------------------------------------------------
//  面光源対応 Cook-Torrance。
//  Diffuse と Specular で別々の代表方向 / フォールオフを使う
//  (エリアライトも diffuse / specular で MRP が異なる)。
//    SphereSinAlpha : 光源の見かけ角 sin。GGX ラフネスを押し広げ、
//                     増えたエネルギーを (a/a')^2 で正規化 (Karis 2013)
//    SoftSinAlpha   : SoftSourceRadius 分の追加見かけ角。
//                     正規化しないので「柔らかくなるだけ」
// -------------------------------------------------------------
float3 AreaLightBRDF(
    float3 N, float3 V,
    float3 DiffuseL, float3 SpecularL,
    float3 Albedo, float Roughness, float Metallic,
    float SphereSinAlpha, float SoftSinAlpha,
    float SpecularScale,
    float DiffuseFalloff, float SpecularFalloff)
{
    float NoV = max(dot(N, V), 1e-5f);
    float NoL_d = max(dot(N, DiffuseL), 0.0f);
    float NoL_s = max(dot(N, SpecularL), 0.0f);

    float3 F0 = lerp(DIELECTRIC_F0, Albedo, Metallic);

    // ---- Diffuse (Lambert)。kD は diffuse 代表方向のフレネルで近似 ----
    float3 Hd = normalize(DiffuseL + V);
    float3 Fd = SchlickFresnel(F0, max(dot(V, Hd), 0.0f));
    float3 kD = (1.0f - Fd) * (1.0f - Metallic);
    float3 Diffuse = kD * Albedo * INV_PI * NoL_d * DiffuseFalloff;

    // ---- Specular (GGX + Karis 面光源正規化) ----
    float a = max(Square(Roughness), 1e-4f);
    float aHard = saturate(a + 0.5f * SphereSinAlpha);
    float EnergyNormalization = Square(a / aHard);
    float aPrime = saturate(aHard + 0.5f * SoftSinAlpha);

    // 既存の GGX_NDF / SmithGeometry は roughness を内部で二乗するので
    // sqrt(a') を渡して実効 a を合わせる
    float RoughnessPrime = sqrt(aPrime);

    float3 H = normalize(SpecularL + V);
    float NoH = max(dot(N, H), 0.0f);
    float VoH = max(dot(V, H), 0.0f);

    float D = GGX_NDF(NoH, RoughnessPrime);
    float G = SmithGeometry(NoV, NoL_s, RoughnessPrime);
    float3 F = SchlickFresnel(F0, VoH);

    float3 Specular = (D * G * F) / max(4.0f * NoV * NoL_s, EPSILON)
                    * EnergyNormalization * SpecularScale * NoL_s * SpecularFalloff;

    return Diffuse + Specular;
}

// =============================================================
//  ローカルライト 1 灯の放射輝度寄与
//  (GetDynamicLighting + GetLocalLightAttenuation 相当)
//    WorldPos : 受光点   N : 法線   V : 受光点 -> カメラ
// =============================================================
float3 IntegrateLocalLight(
    FLightShaderParameters Light,
    float3 WorldPos, float3 N, float3 V,
    float3 Albedo, float Roughness, float Metallic)
{
    FAreaLight AreaLight;
    [branch]
    if (!SetupAreaLight(Light, WorldPos, N, V, AreaLight))
    {
        return float3(0.0f, 0.0f, 0.0f);
    }

    // ------------------------------------------------------------
    //  BRDF x 色 x マスク
    // ------------------------------------------------------------
    float3 Lighting = AreaLightBRDF(
        N, V, AreaLight.DiffuseL, AreaLight.SpecularL,
        Albedo, Roughness, Metallic,
        AreaLight.SphereSinAlpha, AreaLight.SoftSinAlpha,
        Light.SpecularScale,
        AreaLight.DiffuseFalloff, AreaLight.SpecularFalloff);

    return Lighting * Light.Color * AreaLight.LightMask;
}

#endif
