#include "Main.h"
#include "ExponentialHeightFogComponent.h"
#include "World.h"
#include "Scene.h"

// ============================================================
//  UExponentialHeightFogComponent
//  FScene への登録 / 解除とダーティ通知 (ULightComponent と同型)。
// ============================================================

void UExponentialHeightFogComponent::AddToScene()
{
	if (!m_bAddedToScene && IsRegistered() && GetWorld() && m_bVisible)
	{
		GetWorld()->GetScene()->AddExponentialHeightFog(this);
		m_bAddedToScene = true;
	}
}

void UExponentialHeightFogComponent::RemoveFromScene()
{
	if (m_bAddedToScene && GetWorld())
	{
		GetWorld()->GetScene()->RemoveExponentialHeightFog(this);
	}
	m_bAddedToScene = false;
}

void UExponentialHeightFogComponent::OnRegister()
{
	AddToScene();
}

void UExponentialHeightFogComponent::OnUnregister()
{
	RemoveFromScene();
}

// ------------------------------------------------------------
//  可視性 (SetVisibility)
//  bVisible の変更でレンダーステートが再生成される
//  (= FScene から外れる / 入る)。AExponentialHeightFog::bEnabled が
//  ここへ委譲する。
// ------------------------------------------------------------
void UExponentialHeightFogComponent::SetVisibility(bool bNewVisibility)
{
	if (m_bVisible == bNewVisibility)
	{
		return;
	}

	m_bVisible = bNewVisibility;

	if (m_bVisible)
	{
		AddToScene();
	}
	else
	{
		RemoveFromScene();
	}
}

// ------------------------------------------------------------
//  ダーティ通知 (プッシュ型更新)
//  フラグの立ち上がり (false -> true) のときだけ FScene の
//  ダーティリストへ自分を積む (二重登録防止)。未登録時はフラグのみ
//  立て、FScene::AddExponentialHeightFog が登録時にクリアする。
// ------------------------------------------------------------
void UExponentialHeightFogComponent::MarkRenderStateDirty()
{
	const bool bWasDirty = m_RenderStateDirty;
	m_RenderStateDirty = true;

	if (!bWasDirty && m_bAddedToScene && GetWorld())
	{
		GetWorld()->GetScene()->AddExponentialHeightFogRenderStateDirty(this);
	}
}

void UExponentialHeightFogComponent::MarkRenderTransformDirty()
{
	// フォグの基準高さ (FogData[0].Height) はコンポーネントのワールド Y
	// なので、移動はレンダーステートの再スナップショットで反映する
	MarkRenderStateDirty();

	// アタッチ子への再帰伝搬
	USceneComponent::MarkRenderTransformDirty();
}
