#include "Main.h"
#include "LightSceneInfo.h"
#include "SceneViewState.h"

// ============================================================
//  FLightSceneInfoCompact
// ============================================================

void FLightSceneInfoCompact::Init(FLightSceneInfo* InLightSceneInfo)
{
	LightSceneInfo = InLightSceneInfo;

	FSphere BoundingSphere = InLightSceneInfo->Proxy->GetBoundingSphere();
	BoundingSphere.W = (BoundingSphere.W > 0.0f) ? BoundingSphere.W : FLT_MAX;
	BoundingSphereVector = { BoundingSphere.Center.x, BoundingSphere.Center.y, BoundingSphere.Center.z, BoundingSphere.W };

	Color = InLightSceneInfo->Proxy->GetColor();
	LightType = InLightSceneInfo->Proxy->GetLightType();

	bCastDynamicShadow = InLightSceneInfo->Proxy->CastsDynamicShadow() ? 1u : 0u;
	bAffectGlobalIllumination = InLightSceneInfo->Proxy->AffectGlobalIllumination() ? 1u : 0u;
}

// ============================================================
//  FLightSceneInfo
// ============================================================

FLightSceneInfo::FLightSceneInfo(FLightSceneProxy* InProxy, bool InbVisible)
	: Proxy(InProxy)
	, bVisible(InbVisible)
{
}

FLightSceneInfo::~FLightSceneInfo()
{
	delete Proxy;
	Proxy = nullptr;
}

// FLightSceneInfo::ShouldRenderLight 相当
bool FLightSceneInfo::ShouldRenderLight(const FViewInfo& View, bool bOffscreen) const
{
	// 可視のライトはビューの可視判定に従う。Id が範囲外 (今フレームの可視判定より後に
	// 追加されたライト) は描かない
	if (bVisible)
	{
		if (Id < 0 || (size_t)Id >= View.VisibleLightInfos.size())
		{
			return false;
		}

		const FVisibleLightViewInfo& info = View.VisibleLightInfos[Id];
		return bOffscreen ? (info.bInDrawRange != 0) : (info.bInViewFrustum != 0);
	}

	return true;
}

// FLightSceneInfo::ShouldRenderLightViewIndependent 相当
bool FLightSceneInfo::ShouldRenderLightViewIndependent() const
{
	return !IsLightColorAlmostBlack(Proxy->GetColor());
}
