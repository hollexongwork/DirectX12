#ifndef HEIGHT_FOG_COMMON_HLSL
#define HEIGHT_FOG_COMMON_HLSL

#include "Common.hlsl"

// =============================================================
//  HeightFogCommon
//  HeightFogCommon.ush の移植。Exponential Height Fog の解析積分
//  (2 層の指数密度 + Directional Inscattering + Inscattering Cubemap +
//   Start / End / Cutoff Distance + Max Opacity) と、Volumetric Fog
//  (froxel 積分結果 t34) との合成 (CombineVolumetricFog) を提供する。
//
//  パラメータは b7 (FogUniformParameters, ConstantBuffers.hlsl) を
//  参照する。フォグパス (HeightFogPS) とトランスルーセンシー
//  (TranslucentPS) が共用する。
//
//
//  密度関数:  d(y) = FogDensity * exp2(-FogHeightFalloff * (y - FogHeight))
//  透過率:    T = exp2(-∫ d ds)  (exp2 系で統一。
//             CalculateLineIntegralShared の (1 - 2^-a)/a は
//             ∫0^1 2^(-a t) dt の ln2 倍なので exp2 とちょうど整合する)
// =============================================================

static const float FOG_FLT_EPSILON = 0.001f;
static const float FOG_FLT_EPSILON2 = 0.01f;

float FogPow2(float x)
{
    return x * x;
}

// -------------------------------------------------------------
//  インスキャッタ色 (ComputeInscatteringColor)
//  キューブマップ使用時は、受光点までの距離で
//    NonDirectional (最終ミップ = 全方位平均) <-> Directional (ミップ 0)
//  をフェードする。色は ExponentialFogColorParameter.rgb
//  (= InscatteringTextureTint) を乗算する。
// -------------------------------------------------------------
float3 ComputeInscatteringColor(float3 CameraToReceiver, float CameraToReceiverLength)
{
    float3 Inscattering = ExponentialFogColorParameter.xyz;

    [branch]
    if (ExponentialFogParameters3.z > 0.0f)
    {
        float FadeAlpha = saturate(CameraToReceiverLength * FogInscatteringTextureParameters.x
            + FogInscatteringTextureParameters.y);

        // Y 軸まわりに InscatteringColorCubemapAngle だけ回転
        // (x' = x cos - z sin, z' = x sin + z cos)
        float3 CubemapLookupVector = CameraToReceiver;
        const float2 SinCos = SinCosInscatteringColorCubemapRotation.xy;
        CubemapLookupVector.xz = float2(
            dot(CubemapLookupVector.xz, float2(SinCos.y, -SinCos.x)),
            dot(CubemapLookupVector.xz, float2(SinCos.x, SinCos.y)));

        float3 DirectionalColor = FogInscatteringColorCubemap.SampleLevel(Sampler2, CubemapLookupVector, 0.0f).xyz;
        float3 NonDirectionalColor = FogInscatteringColorCubemap.SampleLevel(Sampler2, CubemapLookupVector,
            FogInscatteringTextureParameters.z).xyz;

        Inscattering *= lerp(NonDirectionalColor, DirectionalColor, FadeAlpha);
    }

    return Inscattering;
}

// -------------------------------------------------------------
//  カメラ -> 受光点の線積分 (共有項) (CalculateLineIntegralShared)
//  指数密度 d = GlobalDensity * exp(-HeightFalloff * y) をレイに沿って
//  積分した「単位レイ長あたり」の値。RayDirectionY はレイの Y 成分
//  (正規化しない = レイ全長ぶんの高さ変化)。
// -------------------------------------------------------------
float CalculateLineIntegralShared(float FogHeightFalloff, float RayDirectionY, float RayOriginTerms)
{
    // -127 未満は exp2 が発散する (UE と同じクランプ)
    float Falloff = max(-127.0f, FogHeightFalloff * RayDirectionY);
    float LineIntegral = (1.0f - exp2(-Falloff)) / Falloff;
    // 0 近傍のテイラー展開 (0 除算回避)
    float LineIntegralTaylor = log(2.0f) - (0.5f * FogPow2(log(2.0f))) * Falloff;

    return RayOriginTerms * ((abs(Falloff) > FOG_FLT_EPSILON2) ? LineIntegral : LineIntegralTaylor);
}

// -------------------------------------------------------------
//  Exponential Height Fog の評価 (GetExponentialHeightFog)
//    WorldPositionRelativeToCamera : 受光点 - カメラ位置 [m]
//    ExcludeDistance               : この距離までは解析フォグを
//                                    積分しない (Volumetric Fog が
//                                    覆う範囲 / StartDistance)
//  戻り値: rgb = インスキャッタ (加算項), a = 透過率 (乗算項)
//    SceneColor' = SceneColor * a + rgb
// -------------------------------------------------------------
float4 GetExponentialHeightFog(float3 WorldPositionRelativeToCamera, float ExcludeDistance)
{
    const float MinFogOpacity = ExponentialFogColorParameter.w;
    const float MaxWorldObserverHeight = ExponentialFogParameters.z;

    // 観測者高さをクランプ (高空からの数値精度対策。UE と同じ)
    const float3 CameraOrigin = WorldCameraOrigin.xyz;
    const float3 WorldObserverOrigin = float3(CameraOrigin.x, min(CameraOrigin.y, MaxWorldObserverHeight), CameraOrigin.z);

    float3 CameraToReceiver = WorldPositionRelativeToCamera;
    // クランプぶんの補正 (受光点の絶対高さは変えない)
    CameraToReceiver.y += CameraOrigin.y - WorldObserverOrigin.y;
    float CameraToReceiverLengthSqr = dot(CameraToReceiver, CameraToReceiver);
    float CameraToReceiverLengthInv = rsqrt(max(CameraToReceiverLengthSqr, 0.00000001f));
    float CameraToReceiverLength = CameraToReceiverLengthSqr * CameraToReceiverLengthInv;
    float3 CameraToReceiverNormalized = CameraToReceiver * CameraToReceiverLengthInv;

    float RayOriginTerms = ExponentialFogParameters.x;
    float RayOriginTermsSecond = ExponentialFogParameters2.x;
    float RayLength = CameraToReceiverLength;
    float RayDirectionY = CameraToReceiver.y;

    // ---- StartDistance / Volumetric Fog の範囲を除外 ----
    ExcludeDistance = max(ExcludeDistance, ExponentialFogParameters.w);

    [branch]
    if (ExcludeDistance > 0.0f)
    {
        float ExcludeIntersectionTime = ExcludeDistance * CameraToReceiverLengthInv;
        float CameraToExclusionIntersectionY = ExcludeIntersectionTime * CameraToReceiver.y;
        float ExclusionIntersectionY = WorldObserverOrigin.y + CameraToExclusionIntersectionY;
        float ExclusionIntersectionToReceiverY = CameraToReceiver.y - CameraToExclusionIntersectionY;

        // レイの起点を除外距離の交点へ移す (カメラからではなくそこから積分する)
        RayLength = (1.0f - ExcludeIntersectionTime) * CameraToReceiverLength;
        RayDirectionY = ExclusionIntersectionToReceiverY;

        float Exponent = max(-127.0f, ExponentialFogParameters.y * (ExclusionIntersectionY - ExponentialFogParameters3.y));
        RayOriginTerms = ExponentialFogParameters3.x * exp2(-Exponent);

        float ExponentSecond = max(-127.0f, ExponentialFogParameters2.y * (ExclusionIntersectionY - ExponentialFogParameters2.w));
        RayOriginTermsSecond = ExponentialFogParameters2.z * exp2(-ExponentSecond);
    }

    // ---- EndDistance (UE 5.4): この距離以遠は密度を積み増さない ----
    [flatten]
    if (ExponentialFogParameters4.x > 0.0f)
    {
        RayLength = min(RayLength, max(ExponentialFogParameters4.x - max(ExcludeDistance, 0.0f), 0.0f));
    }

    // 2 層 (異なる高さ減衰 / 密度) の線積分を足し合わせた共有項
    // (Directional Inscattering も同じ項を使う)
    float ExponentialHeightLineIntegralShared =
        CalculateLineIntegralShared(ExponentialFogParameters.y, RayDirectionY, RayOriginTerms)
        + CalculateLineIntegralShared(ExponentialFogParameters2.y, RayDirectionY, RayOriginTermsSecond);

    float ExponentialHeightLineIntegral = ExponentialHeightLineIntegralShared * RayLength;

    float3 InscatteringColor = ComputeInscatteringColor(CameraToReceiver, CameraToReceiverLength);
    float3 DirectionalInscattering = float3(0.0f, 0.0f, 0.0f);

    // ---- Directional Inscattering ----
    // InscatteringLightDirection.w が負なら無効、非負なら開始距離。
    // キューブマップ使用時は無効 (UE と同じ: 指向性はキューブマップが担う)
    [branch]
    if (InscatteringLightDirection.w >= 0.0f && ExponentialFogParameters3.z == 0.0f)
    {
        float DirectionalInscatteringStartDistance = InscatteringLightDirection.w;

        // ライト方向まわりのコサインローブで、環境ヘイズからの
        // ディレクショナルライトのインスキャッタを近似する
        float3 DirectionalLightInscattering = DirectionalInscatteringColor.xyz
            * pow(saturate(dot(CameraToReceiverNormalized, InscatteringLightDirection.xyz)), DirectionalInscatteringColor.w);

        // 開始距離から先の線積分 (ほぼ常に見えるように)
        float DirExponentialHeightLineIntegral = ExponentialHeightLineIntegralShared
            * max(RayLength - DirectionalInscatteringStartDistance, 0.0f);
        // 透過方程式で「フォグを通り抜けた光の量」
        float DirectionalInscatteringFogFactor = saturate(exp2(-DirExponentialHeightLineIntegral));
        // ライトからの最終インスキャッタ
        DirectionalInscattering = DirectionalLightInscattering * (1.0f - DirectionalInscatteringFogFactor);
    }

    // 透過方程式で「フォグを通り抜けた光の量」。MinFogOpacity (= 1 - FogMaxOpacity) で下限
    float ExpFogFactor = max(saturate(exp2(-ExponentialHeightLineIntegral)), MinFogOpacity);

    // ---- FogCutoffDistance: この距離以遠はフォグを掛けない ----
    [flatten]
    if (ExponentialFogParameters3.w > 0.0f && CameraToReceiverLength > ExponentialFogParameters3.w)
    {
        ExpFogFactor = 1.0f;
        DirectionalInscattering = float3(0.0f, 0.0f, 0.0f);
    }

    float3 FogColor = InscatteringColor * (1.0f - ExpFogFactor) + DirectionalInscattering;

    return float4(FogColor, ExpFogFactor);
}

// -------------------------------------------------------------
//  高さフォグの評価 (CalculateHeightFog)
//  Volumetric Fog 有効時は、その最大距離 (ビュー Z) までの区間を
//  解析フォグから除外する (froxel 積分が担うため二重計上しない)。
// -------------------------------------------------------------
float4 CalculateHeightFog(float3 WorldPositionRelativeToCamera)
{
    float ExcludeDistance = 0.0f;

    [branch]
    if (VolumetricFogGridZParams.w > 0.0f)
    {
        // Volumetric Fog はビュー Z で MaxDistance まで覆う。
        // 視線方向の距離に換算して除外する
        float CosAngle = dot(normalize(WorldPositionRelativeToCamera), VolumetricFogViewForward.xyz);
        float InvCosAngle = (CosAngle > FOG_FLT_EPSILON) ? rcp(CosAngle) : 0.0f;
        ExcludeDistance = VolumetricFogParameters.x * InvCosAngle;
    }

    return GetExponentialHeightFog(WorldPositionRelativeToCamera, ExcludeDistance);
}

// -------------------------------------------------------------
//  Volumetric Fog のボリューム UV
//  ビュー Z -> 正規化 Z スライス (ComputeNormalizedZSliceFromDepth)
//  スクリーン座標 -> XY (SVPosition * (1 / (GridSize.xy * PixelSize)))
// -------------------------------------------------------------
float ComputeNormalizedZSliceFromDepth(float SceneDepth)
{
    return log2(SceneDepth * VolumetricFogGridZParams.x + VolumetricFogGridZParams.y)
        * VolumetricFogGridZParams.z * VolumetricFogParameters.y;
}

float3 ComputeVolumeUVFromSvPosition(float2 SvPositionXY, float SceneDepth)
{
    return float3(SvPositionXY * VolumetricFogParameters.zw, ComputeNormalizedZSliceFromDepth(SceneDepth));
}

// -------------------------------------------------------------
//  Volumetric Fog との合成 (CombineVolumetricFog)
//    GlobalFog : 解析フォグ (rgb = インスキャッタ, a = 透過率)
//    VolumeUV  : 積分テクスチャのサンプル位置
//  froxel 積分結果 (rgb = カメラからの累積インスキャッタ, a = 透過率)
//  の手前に解析フォグを重ねる:
//    rgb = Volumetric.rgb + GlobalFog.rgb * Volumetric.a
//    a   = Volumetric.a * GlobalFog.a
// -------------------------------------------------------------
float4 CombineVolumetricFog(float4 GlobalFog, float3 VolumeUV)
{
    float4 VolumetricFogLookup = float4(0.0f, 0.0f, 0.0f, 1.0f);

    [branch]
    if (VolumetricFogGridZParams.w > 0.0f)
    {
        VolumetricFogLookup = IntegratedLightScattering.SampleLevel(Sampler2, VolumeUV, 0.0f);
    }

    return float4(VolumetricFogLookup.rgb + GlobalFog.rgb * VolumetricFogLookup.a,
        VolumetricFogLookup.a * GlobalFog.a);
}

// -------------------------------------------------------------
//  受光点のフォグ (解析 + Volumetric) を一括で評価するヘルパ
//    WorldPos     : 受光点 (ワールド) [m]
//    SvPositionXY : ピクセル座標 (SV_Position.xy)
//    SceneDepth   : ビュー空間 Z (mul(worldPos, View).z)
// -------------------------------------------------------------
float4 ComputeFogInscatteringAndOpacity(float3 WorldPos, float2 SvPositionXY, float SceneDepth)
{
    float4 HeightFog = CalculateHeightFog(WorldPos - WorldCameraOrigin.xyz);
    return CombineVolumetricFog(HeightFog, ComputeVolumeUVFromSvPosition(SvPositionXY, SceneDepth));
}

#endif
