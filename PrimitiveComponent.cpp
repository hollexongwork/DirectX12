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

// ============================================================
//  ダーティ通知 (プッシュ型更新)
//  フラグの立ち上がり (false -> true) のときだけ FScene の
//  ダーティリストへ自分を積む (二重登録防止)。未登録時はフラグのみ
//  立て、FScene::AddPrimitive が登録時に処理する。
// ============================================================

void UPrimitiveComponent::MarkRenderStateDirty()
{
	const bool bWasDirty = m_RenderStateDirty;
	m_RenderStateDirty = true;

	if (!bWasDirty && IsRegistered() && GetWorld())
	{
		GetWorld()->GetScene()->AddPrimitiveRenderStateDirty(this);
	}
}

void UPrimitiveComponent::MarkRenderTransformDirty()
{
	if (!m_RenderTransformDirty)
	{
		m_RenderTransformDirty = true;

		if (IsRegistered() && GetWorld())
		{
			GetWorld()->GetScene()->AddPrimitiveTransformDirty(this);
		}
	}

	// アタッチ子への再帰伝搬 (子が Primitive / Light なら各自エンキューする)
	USceneComponent::MarkRenderTransformDirty();
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
