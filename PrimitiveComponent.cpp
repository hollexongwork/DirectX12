#include "Main.h"
#include "RenderManager.h"
#include "PrimitiveComponent.h"
#include "PrimitiveSceneProxy.h"
#include "World.h"

void UPrimitiveComponent::OnRegister()
{
	if (GetWorld())
	{
		GetWorld()->GetScene()->AddPrimitive(this);
	}
}

void UPrimitiveComponent::OnUnregister()
{
	if (GetWorld())
	{
		GetWorld()->GetScene()->RemovePrimitive(this);
	}
}

void UPrimitiveComponent::SendRenderTransform()
{
	// ワールド境界を再計算してからプロキシへプッシュする
	// (UpdateBounds -> SendRenderTransform の順)
	UpdateBounds();

	if (m_SceneProxy)
	{
		m_SceneProxy->SetTransform(GetComponentToWorld(), GetBounds());
		m_SceneProxy->SetVisibility(m_Visible);
		m_SceneProxy->SetTranslucencySortPriority(m_TranslucencySortPriority);
	}
}
