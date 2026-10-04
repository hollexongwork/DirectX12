#ifndef AREA_LIGHT_COMMON_HLSL
#define AREA_LIGHT_COMMON_HLSL

#include "RectLight.hlsl"

// =============================================================
//  AreaLightCommon
//  面光源 1 灯のセットアップ結果を
//  BxDF へ渡すための構造体。BxDF 非依存なので、レガシー経路
//  (DefaultLitBxDF) と Substrate Slab が共有する。
//  レジスタを宣言しない。
// =============================================================

struct FAreaLight
{
    float SphereSinAlpha; // 球光源の見かけの半角の sin (GGX のエネルギー正規化 / NoH の最大化に使う)
    float SphereSinAlphaSoft; // SoftSourceRadius 分の見かけ角 (ラフネスを広げるだけ。正規化しない)
    float LineCosSubtended; // チューブが張る角度の cos (1 = チューブ無し)

    float3 FalloffColor; // フォールオフに掛かる色 (レクトライトのテクスチャ用。本エンジンは常に 1)

    FRect Rect; // レクトライトのときの矩形 (受光点基準)
    bool bIsRect;
};

// 面光源 1 灯の積分に必要な値 (FAreaLightIntegrateContext)
struct FAreaLightIntegrateContext
{
    FAreaLight AreaLight;
    float3 L; // 代表方向 (受光点 -> 光源、正規化)
    float NoL; // 光源形状を考慮した N・L (0..1)
    float Falloff; // 距離フォールオフ (レクトライトでは放射照度)
};

bool IsRectLight(FAreaLight AreaLight)
{
    return AreaLight.bIsRect;
}

FAreaLightIntegrateContext InitAreaLightIntegrateContext()
{
    FAreaLightIntegrateContext Out;
    Out.AreaLight.SphereSinAlpha = 0.0f;
    Out.AreaLight.SphereSinAlphaSoft = 0.0f;
    Out.AreaLight.LineCosSubtended = 1.0f;
    Out.AreaLight.FalloffColor = float3(1.0f, 1.0f, 1.0f);
    Out.AreaLight.Rect = (FRect) 0;
    Out.AreaLight.bIsRect = false;
    Out.L = float3(0.0f, 0.0f, 1.0f);
    Out.NoL = 0.0f;
    Out.Falloff = 0.0f;
    return Out;
}

#endif
