#include "Main.h"
#include "LightRendering.h"
#include "SceneRenderer.h"
#include "Scene.h"

#include <algorithm>

// ============================================================
//  LightRendering : ライトの可視判定 / 収集 / ソート
//  (ComputeLightVisibility / GatherAndSortLights / GetLightFadeFactor)
// ============================================================

float GMinScreenRadiusForLights = 0.03f;	// ライトを描く最小スクリーン半径
float GLightMaxDrawDistanceScale = 1.0f;	// MaxDrawDistance の倍率

// 「画面上のサイズ」係数 = min(0.02 [1/m], GMinScreenRadiusForLights / 半径 [m])。
// LODDistanceFactor (FOV / 既定 FOV) は 1 とする。
// 係数 x 距離 が 1 に達する距離 (= max(半径 / 0.03, 50 m)) より遠いライトは描かない
static float ComputeLightScreenSizeFactor(float BoundingRadius)
{
	return fminf(0.02f, GMinScreenRadiusForLights / BoundingRadius);
}

static float DistanceSquaredToSphereCenter(const XMFLOAT3& ViewOrigin, const FSphere& Sphere)
{
	const float dx = Sphere.Center.x - ViewOrigin.x;
	const float dy = Sphere.Center.y - ViewOrigin.y;
	const float dz = Sphere.Center.z - ViewOrigin.z;
	return dx * dx + dy * dy + dz * dz;
}

// GetLightFadeFactor 相当
float GetLightFadeFactor(const XMFLOAT3& ViewOrigin, const FLightSceneProxy* Proxy)
{
	const FSphere Bounds = Proxy->GetBoundingSphere();
	const float DistanceSquared = DistanceSquaredToSphereCenter(ViewOrigin, Bounds);

	// 画面上で小さくなるにつれて消す (描かなくなる距離の手前 1/6 でフェード)
	const float SizeFactor = ComputeLightScreenSizeFactor(Bounds.W);
	float SizeFade = SizeFactor * SizeFactor * DistanceSquared;
	SizeFade = fmaxf(fminf(6.0f - 6.0f * SizeFade, 1.0f), 0.0f);

	// MaxDrawDistance の手前 FadeRange でフェードアウト
	const float MaxDist = Proxy->GetMaxDrawDistance() * GLightMaxDrawDistanceScale;
	const float Range = Proxy->GetFadeRange();
	float DistanceFade = 1.0f;
	if (MaxDist != 0.0f && Range > 0.0f)
	{
		DistanceFade = (MaxDist - sqrtf(DistanceSquared)) / Range;
		DistanceFade = fmaxf(fminf(DistanceFade, 1.0f), 0.0f);
	}

	return SizeFade * DistanceFade;
}

// ============================================================
//  ComputeLightVisibility
//  FSceneRenderer::ComputeLightVisibility 相当。FScene::Lights の各ライトを
//  ビューフラスタムと描画距離で判定し、FViewInfo::VisibleLightInfos
//  (添字 = FLightSceneInfo::Id) に書く。
//    - ディレクショナルライトは常に可視
//    - ローカルライトは境界球がフラスタムと交差し、描画距離内のときだけ可視
//  ComputeViewVisibility (フラスタム構築) の後に呼ぶこと。
// ============================================================
void FSceneRenderer::ComputeLightVisibility(FScene* Scene)
{
	const std::vector<FLightSceneInfoCompact>& Lights = Scene->GetLights();

	m_ViewInfo.VisibleLightInfos.assign(Lights.size(), FVisibleLightViewInfo());
	m_LightStats = FLightStats{};

	// プリミティブと同じフラスタム / 視点 (カリング凍結を含む) を使う
	const FConvexVolume& frustum = m_bHasFrozenView ? m_FrozenViewFrustum : m_ViewFrustum;
	const XMFLOAT3 viewOrigin = m_bHasFrozenView
		? m_FrozenViewOrigin
		: XMFLOAT3(m_ViewConstant.WorldCameraOrigin.x, m_ViewConstant.WorldCameraOrigin.y, m_ViewConstant.WorldCameraOrigin.z);

	for (size_t LightIndex = 0; LightIndex < Lights.size(); ++LightIndex)
	{
		const FLightSceneInfoCompact& LightSceneInfoCompact = Lights[LightIndex];
		const FLightSceneInfo* LightSceneInfo = LightSceneInfoCompact.LightSceneInfo;
		if (LightSceneInfo == nullptr)
		{
			continue;	// スパース配列の空きスロット
		}

		m_LightStats.NumSceneLights++;

		const FLightSceneProxy* Proxy = LightSceneInfo->Proxy;
		FVisibleLightViewInfo& VisibleLightViewInfo = m_ViewInfo.VisibleLightInfos[LightIndex];

		if (Proxy->GetLightType() == LightType_Point
			|| Proxy->GetLightType() == LightType_Spot
			|| Proxy->GetLightType() == LightType_Rect)
		{
			const FSphere BoundingSphere = Proxy->GetBoundingSphere();

			// デバッグでフラスタムカリングを切っているときは全て視界内として扱う
			const bool bInViewFrustum = !m_CullingParams.bEnableFrustumCulling
				|| frustum.IntersectSphere(BoundingSphere.Center, BoundingSphere.W);

			// 透視投影: 画面上で小さすぎるライトと MaxDrawDistance より遠いライトは描かない
			const float DistanceSquared = DistanceSquaredToSphereCenter(viewOrigin, BoundingSphere);
			const float MaxDistance = Proxy->GetMaxDrawDistance() * GLightMaxDrawDistanceScale;
			const float MaxDistSquared = MaxDistance * MaxDistance;
			const float SizeFactor = ComputeLightScreenSizeFactor(BoundingSphere.W);
			const bool bDrawLight = (SizeFactor * SizeFactor * DistanceSquared < 1.0f)
				&& (MaxDistSquared == 0.0f || DistanceSquared < MaxDistSquared);

			VisibleLightViewInfo.bInViewFrustum = (bDrawLight && bInViewFrustum) ? 1u : 0u;
			VisibleLightViewInfo.bInDrawRange = bDrawLight ? 1u : 0u;

			if (!bDrawLight)
			{
				m_LightStats.NumDistanceCulled++;
			}
			else if (!bInViewFrustum)
			{
				m_LightStats.NumFrustumCulled++;
			}
		}
		else
		{
			// ディレクショナルライトは常に可視
			VisibleLightViewInfo.bInViewFrustum = 1u;
			VisibleLightViewInfo.bInDrawRange = 1u;
		}
	}
}

// ============================================================
//  GatherAndSortLights
//  FSceneRenderer::GatherAndSortLights 相当。このビューで描くライトを集め、
//  ソートキー (FSortedLightSceneInfo::SortKey) の昇順に並べる。
//  並びは「ライトグリッドに入るローカルライト (影無し -> 影付き、種別順)」
//  -> 「ディレクショナルライト」になり、ライトバッファ (t13) /
//  ローカルシャドウパラメータ (t16) の添字はこの順で決まる。
// ============================================================
void FSceneRenderer::GatherAndSortLights(FScene* Scene, FSortedLightSetSceneInfo& OutSortedLights)
{
	std::vector<FSortedLightSceneInfo>& SortedLights = OutSortedLights.SortedLights;
	SortedLights.clear();
	SortedLights.reserve(Scene->GetLights().size());

	// 視界内のライトの一覧を作る
	for (const FLightSceneInfoCompact& LightSceneInfoCompact : Scene->GetLights())
	{
		const FLightSceneInfo* const LightSceneInfo = LightSceneInfoCompact.LightSceneInfo;
		if (LightSceneInfo == nullptr)
		{
			continue;
		}

		if (LightSceneInfo->ShouldRenderLightViewIndependent()
			&& LightSceneInfo->ShouldRenderLight(m_ViewInfo))
		{
			FSortedLightSceneInfo SortedLightInfo(LightSceneInfo);

			SortedLightInfo.SortKey.Fields.LightType = LightSceneInfoCompact.LightType;
			SortedLightInfo.SortKey.Fields.bTextureProfile = 0;
			// シャドウの割り当ては RenderShadowDepths (この後) なので、
			// 「動的シャドウを落とす設定か」で判定する
			SortedLightInfo.SortKey.Fields.bShadowed = LightSceneInfoCompact.bCastDynamicShadow;
			SortedLightInfo.SortKey.Fields.bLightFunction = 0;
			SortedLightInfo.SortKey.Fields.bUsesLightingChannels = 0;

			// シンプルライト (パーティクルライト) ではない
			SortedLightInfo.SortKey.Fields.bIsNotSimpleLight = 1;

			// ライトグリッド (クラスタードデファード) に入るのはローカルライト。
			// 本エンジンはグリッド内のライトもシャドウマップをインラインで参照するので、
			// 影付きのライトも入る。
			// ディレクショナルライトは全セルに入れても意味が無いので入れない
			const bool bClusteredDeferredSupported = LightSceneInfoCompact.LightType != LightType_Directional;
			SortedLightInfo.SortKey.Fields.bClusteredDeferredNotSupported = bClusteredDeferredSupported ? 0u : 1u;

			SortedLights.push_back(SortedLightInfo);
		}
	}

	// ソート。同じキーのライトは FScene::Lights の順 (= 登録順) を保つ (結果を決定的にするため stable)
	std::stable_sort(SortedLights.begin(), SortedLights.end(),
		[](const FSortedLightSceneInfo& A, const FSortedLightSceneInfo& B)
		{
			return A.SortKey.Packed < B.SortKey.Packed;
		});

	// 範囲の境界を求める
	const int NumLights = (int)SortedLights.size();
	OutSortedLights.SimpleLightsEnd = NumLights;
	OutSortedLights.ClusteredSupportedEnd = NumLights;
	OutSortedLights.UnbatchedLightStart = NumLights;

	for (int LightIndex = 0; LightIndex < NumLights; LightIndex++)
	{
		const FSortedLightSceneInfo& SortedLightInfo = SortedLights[LightIndex];
		const bool bDrawShadows = SortedLightInfo.SortKey.Fields.bShadowed;
		const bool bDrawLightFunction = SortedLightInfo.SortKey.Fields.bLightFunction;
		const bool bLightingChannels = SortedLightInfo.SortKey.Fields.bUsesLightingChannels;

		if (SortedLightInfo.SortKey.Fields.bIsNotSimpleLight && OutSortedLights.SimpleLightsEnd == NumLights)
		{
			// シンプルライトでない最初の位置
			OutSortedLights.SimpleLightsEnd = LightIndex;
		}

		if (SortedLightInfo.SortKey.Fields.bClusteredDeferredNotSupported && OutSortedLights.ClusteredSupportedEnd == NumLights)
		{
			// ライトグリッドに入らない最初の位置
			OutSortedLights.ClusteredSupportedEnd = LightIndex;
		}

		if ((bDrawShadows || bDrawLightFunction || bLightingChannels) && SortedLightInfo.SortKey.Fields.bClusteredDeferredNotSupported)
		{
			// 個別に扱う影付きライトを見つけたら終わり
			OutSortedLights.UnbatchedLightStart = LightIndex;
			break;
		}
	}
}
