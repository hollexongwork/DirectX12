#pragma once
#include <vector>
#include <DirectXMath.h>
#include "LightSceneInfo.h"

using namespace DirectX;

// ============================================================
//  LightRendering
//  ライトの可視判定 / 収集 / ソート。
//
//    FSceneRenderer::ComputeLightVisibility  : ライトの可視判定 (フラスタム / 描画距離)
//                                              -> FViewInfo::VisibleLightInfos
//    FSceneRenderer::GatherAndSortLights     : 描くライトを集めてソート
//                                              -> FSortedLightSetSceneInfo
//    GetLightFadeFactor                      : ローカルライトの距離フェード
//
//  実装は LightRendering.cpp。ライトバッファへの詰め込みは
//  FSceneRenderer::ComputeLightGrid (LightGridInjection.cpp)。
// ============================================================

// これより画面上で小さくなったローカルライトは描かない
extern float GMinScreenRadiusForLights;
// ULightComponent::MaxDrawDistance に掛けるスケール
extern float GLightMaxDrawDistanceScale;

// ------------------------------------------------------------
//  FSortedLightSceneInfo
//  FSortedLightSceneInfo に相当。ライトとソートキーの組。
//  キーのビット順がそのままライトの並び順を決める
//  (LightType が最下位、bClusteredDeferredNotSupported が最上位)。
// ------------------------------------------------------------
struct FSortedLightSceneInfo
{
	union
	{
		struct
		{
			// 注意: メンバの並びがソート順を決める
			unsigned int LightType : LightType_NumBits;			// ライト種別
			unsigned int bTextureProfile : 1;					// IES プロファイルを持つ (本エンジンは常に 0)
			unsigned int bLightFunction : 1;					// ライトファンクションを持つ (常に 0)
			unsigned int bUsesLightingChannels : 1;				// 既定以外のライティングチャンネル (常に 0)
			unsigned int bShadowed : 1;							// 動的シャドウを落とす
			unsigned int bIsNotSimpleLight : 1;					// シンプルライト (パーティクル) でない (常に 1)
			unsigned int bClusteredDeferredNotSupported : 1;	// ライトグリッドに入らない (= ディレクショナルライト)
		} Fields;

		// ビットをまとめた整数 (ソートに使う)
		int Packed;
	} SortKey;

	const FLightSceneInfo* LightSceneInfo;
	int SimpleLightIndex;

	explicit FSortedLightSceneInfo(const FLightSceneInfo* InLightSceneInfo)
		: LightSceneInfo(InLightSceneInfo)
		, SimpleLightIndex(-1)
	{
		SortKey.Packed = 0;
		SortKey.Fields.bIsNotSimpleLight = 1;
	}
};

// ------------------------------------------------------------
//  FSortedLightSetSceneInfo
//  FSortedLightSetSceneInfo に相当。ソート済みのライト列と範囲の境界。
// ------------------------------------------------------------
struct FSortedLightSetSceneInfo
{
	int SimpleLightsEnd = 0;		// シンプルライトの終端 (本エンジンは常に 0)
	int ClusteredSupportedEnd = 0;	// ライトグリッドに入るライト (ローカルライト) の終端
	int UnbatchedLightStart = 0;	// 個別に扱うライト (ディレクショナルライト) の先頭

	std::vector<FSortedLightSceneInfo> SortedLights;
};

// ローカルライトの距離フェード係数 (GetLightFadeFactor)。
//   SizeFade     : 画面上で小さくなったライトを消す (GMinScreenRadiusForLights)
//   DistanceFade : MaxDrawDistance の手前 MaxDistanceFadeRange でフェードアウト
// ViewOrigin はカメラのワールド位置 [m]
float GetLightFadeFactor(const XMFLOAT3& ViewOrigin, const FLightSceneProxy* Proxy);
