#ifndef DEFERRED_LIGHTING_COMMON_HLSL
#define DEFERRED_LIGHTING_COMMON_HLSL

#include "LightData.hlsl"
#include "DynamicLightingCommon.hlsl"
#include "CapsuleLight.hlsl"
#include "RectLight.hlsl"

// =============================================================
//  DeferredLightingCommon
//  DeferredLightingCommon.ush 相当。ライト 1 灯の評価。
//
//  前半 (レジスタ非依存。グラフィックス / コンピュート共用):
//    GetLocalLightAttenuation : ローカルライトの減衰マスク
//                               (半径の窓 / 指数フォールオフ / スポットコーン / レクトの背面)
//    GetCapsule / GetRect     : FDeferredLightData から面光源の形状を作る
//
//  後半 (NON_DIRECTIONAL_DIRECT_LIGHTING == 0 のときだけ。グラフィックス専用):
//    GetShadowTerms           : シャドウ係数 + コンタクトシャドウ
//    AccumulateDynamicLighting / GetDynamicLighting :
//                               減衰 -> シャドウ -> 面光源の積分 (IntegrateBxDF)
//
//  コンピュート (Lumen の Surface Cache / Volumetric Fog) は BxDF を伴わないので、
//  include の前に #define NON_DIRECTIONAL_DIRECT_LIGHTING 1 として前半だけを使い、
//  IntegrateLight (CapsuleLight.hlsl / RectLight.hlsl) でフォールオフを求める
//  (UE の VolumetricFog.usf / LumenSceneDirectLighting.usf と同じ使い方)。
//
//  コンタクトシャドウ (SUPPORT_CONTACT_SHADOWS 1) はシーン深度 (t3) を読むので
//  デファードライティングパスだけが有効にする。
// =============================================================

#ifndef NON_DIRECTIONAL_DIRECT_LIGHTING
#define NON_DIRECTIONAL_DIRECT_LIGHTING 0
#endif

#ifndef SUPPORT_CONTACT_SHADOWS
#define SUPPORT_CONTACT_SHADOWS 0
#endif

// カプセルライトの距離バイアスの 2 乗。UE は 1 (cm^2)。メートルでは 1e-4 [PORT]
#define LOCAL_LIGHT_DIST_BIAS_SQR 1e-4f

// -------------------------------------------------------------
//  ローカルライトの減衰マスク (GetLocalLightAttenuation)
//    ToLight : 受光点 -> ライト (出力)
//    L       : 同、正規化 (出力)
//  戻り値は 0..1。距離の逆二乗そのものは含まない (面光源の積分側が掛ける)。
// -------------------------------------------------------------
float GetLocalLightAttenuation(
    float3 WorldPosition,
    FDeferredLightData LightData,
    inout float3 ToLight,
    inout float3 L)
{
    ToLight = LightData.WorldPosition - WorldPosition;

    float DistanceSqr = dot(ToLight, ToLight);
    L = ToLight * rsqrt(max(DistanceSqr, 1e-8f)); // 距離 0 の NaN を避ける下限 [PORT]

    float LightMask;
    if (LightData.bInverseSquared)
    {
        // 逆二乗の裾を AttenuationRadius で滑らかに 0 へ落とす窓: (1 - (d^2 / r^2)^2)^2
        LightMask = Square(saturate(1.0f - Square(DistanceSqr * Square(LightData.InvRadius))));
    }
    else
    {
        LightMask = RadialAttenuation(ToLight * LightData.InvRadius, LightData.FalloffExponent);
    }

    if (LightData.bSpotLight)
    {
        LightMask *= SpotAttenuation(L, -LightData.Direction, LightData.SpotAngles);
    }

    if (LightData.bRectLight)
    {
        // 発光面の裏側は照らさない
        LightMask = dot(LightData.Direction, L) < 0.0f ? 0.0f : LightMask;
    }

    return LightMask;
}

// FDeferredLightData -> カプセルライト (GetCapsule)
FCapsuleLight GetCapsule(float3 ToLight, FDeferredLightData LightData)
{
    FCapsuleLight Capsule;
    Capsule.Length = LightData.SourceLength;
    Capsule.Radius = LightData.SourceRadius;
    Capsule.SoftRadius = LightData.SoftSourceRadius;
    // ディレクショナルライト (ToLight が単位ベクトル) は UE と同じ 1 を使う。
    // 見かけの半径 sin(半角) に対して同じ数値になるようにするため [PORT]
    Capsule.DistBiasSqr = LightData.bRadialLight ? LOCAL_LIGHT_DIST_BIAS_SQR : 1.0f;
    Capsule.LightPos[0] = ToLight - 0.5f * Capsule.Length * LightData.Tangent;
    Capsule.LightPos[1] = ToLight + 0.5f * Capsule.Length * LightData.Tangent;
    return Capsule;
}

// FDeferredLightData -> レクトライトの矩形 (GetRect)。バーンドアで見える範囲へ切り詰める
FRect GetRect(float3 ToLight, FDeferredLightData LightData)
{
    return GetRect(
        ToLight,
        LightData.Direction,
        LightData.Tangent,
        LightData.SourceRadius,
        LightData.SourceLength,
        LightData.RectLightData.BarnCosAngle,
        LightData.RectLightData.BarnLength,
        true);
}

#if !NON_DIRECTIONAL_DIRECT_LIGHTING

#include "ShadingModels.hlsl"
#include "CapsuleLightIntegrate.hlsl"
#include "RectLightIntegrate.hlsl"

#if SUPPORT_CONTACT_SHADOWS

// -------------------------------------------------------------
//  コンタクトシャドウのスクリーンスペースレイ (ShadowRayCast)
//  受光点からライト方向へ RayLength だけ、シーン深度に対して NumSteps 回
//  レイマーチする。シャドウマップの解像度では拾えない接地部分の影を補う。
//    戻り値 : 最初に当たった距離 [m] (当たらなければ負)
//    bOutHitCastContactShadow : 当たった先がコンタクトシャドウを落とすジオメトリか。
//                               UE はステンシルで判定するが、本エンジンは常に真 [PORT]
//  深度は標準 Z (近 0 / 遠 1)。UE (逆 Z) とは比較の向きが逆になる。
// -------------------------------------------------------------
float ShadowRayCast(
    float3 RayOriginWorld, float3 RayDirection, float RayLength,
    int NumSteps, float StepOffset, out bool bOutHitCastContactShadow)
{
    float4 RayStartClip = mul(mul(float4(RayOriginWorld, 1.0f), View), Projection);
    float4 RayDirClip = mul(mul(float4(RayDirection * RayLength, 0.0f), View), Projection);
    float4 RayEndClip = RayStartClip + RayDirClip;

    float3 RayStartScreen = RayStartClip.xyz / RayStartClip.w;
    float3 RayEndScreen = RayEndClip.xyz / RayEndClip.w;

    float3 RayStepScreen = RayEndScreen - RayStartScreen;

    // NDC -> UV (y 反転)
    float3 RayStartUVz = float3(RayStartScreen.xy * float2(0.5f, -0.5f) + 0.5f, RayStartScreen.z);
    float3 RayStepUVz = float3(RayStepScreen.xy * float2(0.5f, -0.5f), RayStepScreen.z);

    // 視線方向へ RayLength だけ進んだときのデバイス Z の変化 = 比較の許容幅の基準
    float4 RayDepthClip = RayStartClip + mul(float4(0.0f, 0.0f, RayLength, 0.0f), Projection);
    float3 RayDepthScreen = RayDepthClip.xyz / RayDepthClip.w;

    const float Step = 1.0f / NumSteps;

    // * 2 は極端な条件でのモアレを減らすため
    const float CompareTolerance = abs(RayDepthScreen.z - RayStartScreen.z) * Step * 2.0f;

    float SampleTime = StepOffset * Step + Step;

    float FirstHitTime = -1.0f;

    // 点サンプル相当 (テクセルを直接読む)
    const float StartDepth = TextureDepth.Load(int3(RayStartUVz.xy * ViewSizeAndInvSize.xy, 0)).r;

    [unroll]
    for (int i = 0; i < NumSteps; i++)
    {
        float3 SampleUVz = RayStartUVz + RayStepUVz * SampleTime;
        float SampleDepth = TextureDepth.Load(int3(clamp(SampleUVz.xy, 0.0f, 0.99999f) * ViewSizeAndInvSize.xy, 0)).r;

        // 開始ピクセル自身との交差を避ける (点サンプルなので厳密比較でよい)
        if (SampleDepth != StartDepth)
        {
            // レイがシーンの面より奥 (標準 Z では大きい) に、許容幅の 2 倍以内で潜っていればヒット
            float DepthDiff = SampleDepth - SampleUVz.z;
            bool Hit = abs(DepthDiff + CompareTolerance) < CompareTolerance;

            FirstHitTime = (Hit && FirstHitTime < 0.0f) ? SampleTime : FirstHitTime;
        }

        SampleTime += Step;
    }

    float HitDistance = -1.0f;
    bOutHitCastContactShadow = false;
    if (FirstHitTime > 0.0f)
    {
        // 画面外へ出たヒットは無効
        float3 HitUVz = RayStartUVz + RayStepUVz * FirstHitTime;
        bool bValidUV = all(HitUVz.xy > 0.0f) && all(HitUVz.xy < 1.0f);

        HitDistance = bValidUV ? (FirstHitTime * RayLength) : -1.0f;
        bOutHitCastContactShadow = true;
    }

    return HitDistance;
}

#endif // SUPPORT_CONTACT_SHADOWS

// -------------------------------------------------------------
//  シャドウ係数 (GetShadowTerms)
//    SceneDepth       : 受光点のビュー空間 Z [m]
//    LightAttenuation : 呼び出し側が求めたシャドウ係数 (シャドウマップ / CSM /
//                       Distance Field。1 = 影なし)。UE はライトごとの
//                       シャドウマスクテクスチャから読むが、本エンジンは
//                       ライトグリッドのループ内でインラインに求める [PORT]
//    Dither           : コンタクトシャドウのレイ開始位置のディザ (0..1)
// -------------------------------------------------------------
void GetShadowTerms(float SceneDepth, FDeferredLightData LightData, float3 WorldPosition, float3 L, float LightAttenuation, float Dither, inout FShadowTerms Shadow)
{
    float ContactShadowLength = 0.0f;

    [branch]
    if (LightData.ShadowedBits)
    {
        Shadow.SurfaceShadow = LightAttenuation;
        Shadow.TransmissionShadow = LightAttenuation;
        Shadow.TransmissionThickness = LightAttenuation;

#if SUPPORT_CONTACT_SHADOWS
        [flatten]
        if (LightData.ShadowedBits > 1 && LightData.ContactShadowLength > 0.0f)
        {
            // スクリーン空間の長さは「画面の高さに対する割合」。受光点の深度でワールドの長さへ直す
            // (UE: View.ClipToView[1][1] * SceneDepth。対角成分は転置の影響を受けない)
            const float ContactShadowLengthScreenScale = SceneDepth / Projection._22;
            ContactShadowLength = LightData.ContactShadowLength * (LightData.ContactShadowLengthInWS ? 1.0f : ContactShadowLengthScreenScale);
        }
#endif
    }

#if SUPPORT_CONTACT_SHADOWS
    [branch]
    if (ContactShadowLength > 0.0f)
    {
        float StepOffset = Dither - 0.5f;
        bool bHitCastContactShadow = false;
        float HitDistance = ShadowRayCast(WorldPosition, L, ContactShadowLength, 8, StepOffset, bHitCastContactShadow);

        if (HitDistance > 0.0f)
        {
            float ContactShadowOcclusion = bHitCastContactShadow ? LightData.ContactShadowCastingIntensity : LightData.ContactShadowNonCastingIntensity;
            float ContactShadow = 1.0f - ContactShadowOcclusion;

            Shadow.SurfaceShadow *= ContactShadow;
            Shadow.TransmissionShadow *= ContactShadow;
        }
    }
#endif
}

// -------------------------------------------------------------
//  ライト 1 灯の放射輝度寄与 (AccumulateDynamicLighting)
//    WorldPosition    : 受光点
//    CameraVector     : カメラ -> 受光点 (正規化)
//    AmbientOcclusion : 影を落とさないライトのシャドウ係数の初期値 (通常 1)
//    LightAttenuation : 呼び出し側が求めたシャドウ係数
//    SurfaceShadow    : 最終的なサーフェスのシャドウ係数 (出力)
//  戻り値は (拡散 + スペキュラ) x ライト色 x 減衰 x シャドウ。
// -------------------------------------------------------------
float3 AccumulateDynamicLighting(
    float3 WorldPosition, float3 CameraVector, FGBufferData GBuffer, float AmbientOcclusion,
    FDeferredLightData LightData, float LightAttenuation, float Dither,
    inout float SurfaceShadow)
{
    float3 OutLighting = float3(0.0f, 0.0f, 0.0f);

    float3 V = -CameraVector;
    float3 N = GBuffer.WorldNormal;

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
        Shadow.SurfaceShadow = AmbientOcclusion;
        Shadow.TransmissionShadow = 1.0f;
        Shadow.TransmissionThickness = 1.0f;
        GetShadowTerms(GBuffer.Depth, LightData, WorldPosition, L, LightAttenuation, Dither, Shadow);
        SurfaceShadow = Shadow.SurfaceShadow;

        [branch]
        if (Shadow.SurfaceShadow + Shadow.TransmissionShadow > 0.0f)
        {
            FDirectLighting Lighting;

            if (LightData.bRectLight)
            {
                FRect Rect = GetRect(ToLight, LightData);
                Lighting = IntegrateBxDF(GBuffer, N, V, Rect, Shadow);
            }
            else
            {
                FCapsuleLight Capsule = GetCapsule(ToLight, LightData);
                Lighting = IntegrateBxDF(GBuffer, N, V, Capsule, Shadow, LightData.bInverseSquared);
            }

            Lighting.Specular *= LightData.SpecularScale;
            Lighting.Diffuse *= LightData.DiffuseScale;
            Lighting.Transmission *= LightData.DiffuseScale;

            OutLighting = (Lighting.Diffuse + Lighting.Specular) * (MaskedLightColor * Shadow.SurfaceShadow)
                + Lighting.Transmission * (MaskedLightColor * Shadow.TransmissionShadow);
        }
    }

    return OutLighting;
}

// ライト 1 灯の放射輝度寄与 (GetDynamicLighting)
float3 GetDynamicLighting(
    float3 WorldPosition, float3 CameraVector, FGBufferData GBuffer, float AmbientOcclusion,
    FDeferredLightData LightData, float LightAttenuation, float Dither,
    inout float SurfaceShadow)
{
    return AccumulateDynamicLighting(WorldPosition, CameraVector, GBuffer, AmbientOcclusion, LightData, LightAttenuation, Dither, SurfaceShadow);
}

#endif // !NON_DIRECTIONAL_DIRECT_LIGHTING

#endif
