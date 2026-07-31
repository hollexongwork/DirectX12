#include "Main.h"
#include <algorithm>
#include "SceneComponent.h"

USceneComponent::~USceneComponent()
{
	// 親の子リストから自分を除去 (親側に解放済みポインタを残さない)
	DetachFromParent();

	// 子の親ポインタを無効化 (子側が解放済みの親を辿らないようにする)。
	// 以後、子の GetComponentToWorld はローカル行列をワールドとして扱う。
	for (USceneComponent* child : m_AttachChildren)
	{
		child->m_AttachParent = nullptr;
	}
	m_AttachChildren.clear();
}

void USceneComponent::SetupAttachment(USceneComponent* Parent)
{
	if (Parent == nullptr || Parent == this) return;

	// 循環アタッチ防止: Parent の祖先チェーンに自分がいたら無視する
	// (許すと GetComponentToWorld が無限再帰する)
	for (USceneComponent* ancestor = Parent; ancestor != nullptr; ancestor = ancestor->m_AttachParent)
	{
		if (ancestor == this) return;
	}

	// 付け替え: 旧親の子リストに残留エントリを作らない
	DetachFromParent();

	m_AttachParent = Parent;
	Parent->m_AttachChildren.push_back(this);
}

void USceneComponent::DetachFromParent()
{
	if (m_AttachParent == nullptr) return;

	std::vector<USceneComponent*>& siblings = m_AttachParent->m_AttachChildren;
	siblings.erase(std::remove(siblings.begin(), siblings.end(), this), siblings.end());

	m_AttachParent = nullptr;
}

XMMATRIX USceneComponent::GetLocalMatrix() const
{
	XMMATRIX local = XMMatrixIdentity();
	local *= XMMatrixScaling(m_RelativeScale3D.x, m_RelativeScale3D.y, m_RelativeScale3D.z);
	local *= XMMatrixRotationRollPitchYaw(m_RelativeRotation.x, m_RelativeRotation.y, m_RelativeRotation.z);
	local *= XMMatrixTranslation(m_RelativeLocation.x, m_RelativeLocation.y, m_RelativeLocation.z);
	return local;
}

XMMATRIX USceneComponent::GetComponentToWorld() const
{
	if (m_AttachParent)
	{
		return GetLocalMatrix() * m_AttachParent->GetComponentToWorld();
	}
	return GetLocalMatrix();
}

XMFLOAT3 USceneComponent::GetComponentLocation() const
{
	XMMATRIX world = GetComponentToWorld();

	XMFLOAT3 location;
	XMStoreFloat3(&location, world.r[3]);
	return location;
}

XMFLOAT3 USceneComponent::GetForwardVector() const
{
	XMMATRIX world = GetComponentToWorld();

	XMFLOAT3 forward;
	XMStoreFloat3(&forward, XMVector3Normalize(world.r[2]));
	return forward;
}

XMFLOAT3 USceneComponent::GetRightVector() const
{
	XMMATRIX world = GetComponentToWorld();

	XMFLOAT3 right;
	XMStoreFloat3(&right, XMVector3Normalize(world.r[0]));
	return right;
}

XMFLOAT3 USceneComponent::GetUpVector() const
{
	XMMATRIX world = GetComponentToWorld();

	XMFLOAT3 up;
	XMStoreFloat3(&up, XMVector3Normalize(world.r[1]));
	return up;
}

// ============================================================
//  境界 (フラスタムカリング用)
// ============================================================

FBoxSphereBounds USceneComponent::CalcBounds(const XMMATRIX& LocalToWorld) const
{
	// 既定: トランスフォーム原点の点境界 (USceneComponent::CalcBounds と同じ)。
	// 描画されるコンポーネントは各自オーバーライドすること。
	FBoxSphereBounds bounds;
	XMStoreFloat3(&bounds.Origin, LocalToWorld.r[3]);
	bounds.BoxExtent = { 0.0f, 0.0f, 0.0f };
	bounds.SphereRadius = 0.0f;
	return bounds;
}

void USceneComponent::UpdateBounds()
{
	m_Bounds = CalcBounds(GetComponentToWorld());
}
