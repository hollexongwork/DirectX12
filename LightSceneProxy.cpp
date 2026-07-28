#include "Main.h"
#include "LightSceneProxy.h"
#include "LightComponent.h"

// ------------------------------------------------------------
//  共通プロパティのスナップショット。
//  GetLightType() / GetColoredLightBrightness() は完全に構築済みの
//  コンポーネントに対する仮想呼び出しなので、種別ごとの
//  ComputeLightBrightness (単位換算) が正しくディスパッチされる。
// ------------------------------------------------------------
FLightSceneProxy::FLightSceneProxy(const ULightComponent* Component)
{
	m_Type = Component->GetLightType();
	m_Color = Component->GetColoredLightBrightness();
	m_SpecularScale = Component->GetSpecularScale();
	m_bAffectsWorld = Component->GetAffectsWorld();
	m_bCastShadows = Component->GetCastShadows();
	m_bUseRTDFShadows = Component->GetUseRayTracedDistanceFieldShadows();

	m_Position = Component->GetComponentLocation();
	m_Direction = Component->GetForwardVector();
	m_Tangent = Component->GetRightVector();
}
