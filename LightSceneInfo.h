#pragma once
#include <DirectXMath.h>
#include "LightSceneProxy.h"

using namespace DirectX;

class FScene;
class FLightSceneInfo;
struct FViewInfo;

// ============================================================
//  FLightSceneInfoCompact
//  FLightSceneInfoCompact (ScenePrivate.h) に相当。FScene::Lights が
//  値で持つ、ライト 1 灯の要約 (境界球 / 色 / 種別 / フラグ)。
//  ライトの巡回でプロキシを辿らずに済ませるためのもの。
// ============================================================
class FLightSceneInfoCompact
{
public:
	// xyz = 境界球の中心、w = 半径 (ディレクショナルライトは FLT_MAX)
	XMFLOAT4 BoundingSphereVector = { 0.0f, 0.0f, 0.0f, 0.0f };
	XMFLOAT3 Color = { 0.0f, 0.0f, 0.0f };

	// null = スパース配列の空きスロット
	FLightSceneInfo* LightSceneInfo = nullptr;

	unsigned int LightType : LightType_NumBits;
	unsigned int bCastDynamicShadow : 1;
	unsigned int bAffectGlobalIllumination : 1;

	FLightSceneInfoCompact()
		: LightType(LightType_Point)
		, bCastDynamicShadow(0)
		, bAffectGlobalIllumination(0)
	{
	}

	explicit FLightSceneInfoCompact(FLightSceneInfo* InLightSceneInfo)
		: FLightSceneInfoCompact()
	{
		Init(InLightSceneInfo);
	}

	// プロキシから要約を取り直す
	void Init(FLightSceneInfo* InLightSceneInfo);
};

// ============================================================
//  FLightSceneInfo
//  FLightSceneInfo (LightSceneInfo.h) に相当。レンダラ内部で持つライトの状態。
//  プロキシ (FLightSceneProxy) を所有し、FScene::Lights 内の位置 (Id) を覚える。
//  Id は FViewInfo::VisibleLightInfos を引く添字にもなる。
//
//  UE のライト - プリミティブの相互作用 (ライトオクツリー / FLightPrimitiveInteraction) は
//  持たない [PORT]。本エンジンはライトグリッドとシャドウビュー別のフラスタムカリングで代替している。
// ============================================================
class FLightSceneInfo
{
public:
	// レンダー側ミラー。この FLightSceneInfo が所有する (デストラクタで破棄)
	FLightSceneProxy* Proxy = nullptr;

	// FScene::Lights 内の添字 (シーンに居ない間は -1)
	int Id = -1;

	FScene* Scene = nullptr;

	// 可視のライトか。false は「見えないがプレビュー等のためにシーンが持つライト」(UE のエディタ用)。
	// 本エンジンは常に true
	bool bVisible = true;

	FLightSceneInfo(FLightSceneProxy* InProxy, bool InbVisible);
	~FLightSceneInfo();

	FLightSceneInfo(const FLightSceneInfo&) = delete;
	FLightSceneInfo& operator=(const FLightSceneInfo&) = delete;

	// このビューで描くべきか。
	//   bOffscreen = false : ビューフラスタム内 (FVisibleLightViewInfo::bInViewFrustum)
	//   bOffscreen = true  : 描画距離内 (bInDrawRange)。Lumen の Surface Cache のように
	//                        画面外のライトも要る用途向け
	bool ShouldRenderLight(const FViewInfo& View, bool bOffscreen = false) const;

	// ビューに依らない描画要否 (色がほぼ黒のライトは描かない)
	bool ShouldRenderLightViewIndependent() const;

	FSphere GetBoundingSphere() const { return Proxy->GetBoundingSphere(); }
};
