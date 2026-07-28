#include "Main.h"
#include "RenderManager.h"
#include "PrimitiveSceneProxy.h"
#include "PrimitiveComponent.h"

FPrimitiveSceneProxy::FPrimitiveSceneProxy(const UPrimitiveComponent* Component)
{
	XMMATRIX localToWorld = Component->GetComponentToWorld();
	XMStoreFloat4x4(&m_LocalToWorld, localToWorld);

	// 境界は生成時点のトランスフォームで初期化する
	// (以後は SendRenderTransform -> SetTransform が毎フレーム更新)
	m_Bounds = Component->CalcBounds(localToWorld);

	m_Visible = Component->IsVisible();
	m_bCastShadow = Component->GetCastShadow();
	m_bAffectDistanceField = Component->GetAffectDistanceFieldLighting();

	// 描画距離カリング (0 = 無制限)
	m_MinDrawDistance = Component->GetMinDrawDistance();
	m_MaxDrawDistance = Component->GetCachedMaxDrawDistance();
}

void FPrimitiveSceneProxy::UploadPrimitiveConstant(RenderManager* RHI) const
{
	XMMATRIX localToWorld = XMLoadFloat4x4(&m_LocalToWorld);

	PRIMITIVE_CONSTANT constant{};
	XMStoreFloat4x4(&constant.LocalToWorld, XMMatrixTranspose(localToWorld));

	RHI->SetConstant(RenderManager::CONSTANT_TYPE::PRIMITIVE, &constant, sizeof(constant));
}
