#pragma once
#include <DirectXMath.h>
#include <vector>
#include "ActorComponent.h"
#include "BoxSphereBounds.h"

using namespace DirectX;

// ============================================================
//  USceneComponent
//  USceneComponent に相当。トランスフォーム
//  (RelativeLocation / RelativeRotation / RelativeScale3D) を持ち、
//  SetupAttachment による親子階層を組める。
//  ※ 回転はラジアンの XMFLOAT3(XMMatrixRotationRollPitchYaw 準拠) を採用。
//
//  境界 (Bounds): USceneComponent::Bounds に相当するワールド境界を
//  保持する。派生クラスは CalcBounds をオーバーライドして
//  ローカル境界をワールドへ変換して返す (フラスタムカリング用)。
// ============================================================

class USceneComponent : public UActorComponent
{
protected:
	XMFLOAT3 m_RelativeLocation = { 0.0f, 0.0f, 0.0f };
	XMFLOAT3 m_RelativeRotation = { 0.0f, 0.0f, 0.0f };	// ラジアン
	XMFLOAT3 m_RelativeScale3D = { 1.0f, 1.0f, 1.0f };

	USceneComponent* m_AttachParent = nullptr;
	std::vector<USceneComponent*> m_AttachChildren;

	// ワールド空間の境界 (USceneComponent::Bounds)。
	// UpdateBounds() が CalcBounds(GetComponentToWorld()) で更新する。
	FBoxSphereBounds m_Bounds;

	// レンダートランスフォームダーティフラグ (プッシュ型更新)。
	// 立っている = FScene のトランスフォームダーティリストに
	// エンキュー済み (登録中のみ)。二重登録防止に使う。
	// UPrimitiveComponent / ULightComponent の
	// MarkRenderTransformDirty オーバーライドが管理する。
	bool m_RenderTransformDirty = false;

public:
	// デストラクタで親子リンクを双方向に解除する
	// (親の子リスト残留 / 子の親ポインタのダングリング防止)
	virtual ~USceneComponent();

	// ---- アタッチメント ----
	// 既に親がいる場合は付け替え (旧親の子リストから自動除去)。
	// 自分自身・自分の子孫へのアタッチ (循環) は無視される。
	void SetupAttachment(USceneComponent* Parent);

	// 親から切り離す (UE5 の DetachFromComponent に相当)。
	// 親の子リストからも自分を除去する。未アタッチなら何もしない。
	void DetachFromParent();

	USceneComponent* GetAttachParent() const { return m_AttachParent; }
	const std::vector<USceneComponent*>& GetAttachChildren() const { return m_AttachChildren; }

	// ---- 相対トランスフォーム ----
	// セッターはレンダートランスフォームダーティを自身 + 全アタッチ子へ
	// 伝搬する (親の移動は子のワールドトランスフォームも変える)。
	// AActor::SetActorLocation 等もここへ委譲されるため漏れなく捕捉される。
	// ※ m_RelativeLocation 等を直接書き換えず必ずセッターを通すこと
	//   (プッシュ型更新のダーティ通知が漏れる)
	void SetRelativeLocation(const XMFLOAT3& Location) { m_RelativeLocation = Location; MarkRenderTransformDirty(); }
	void SetRelativeRotation(const XMFLOAT3& Rotation) { m_RelativeRotation = Rotation; MarkRenderTransformDirty(); }
	void SetRelativeScale3D(const XMFLOAT3& Scale) { m_RelativeScale3D = Scale; MarkRenderTransformDirty(); }

	// ---- レンダートランスフォームダーティ (MarkRenderTransformDirty) ----
	// 既定はアタッチ子への再帰伝搬のみ (自身は描画に関与しない)。
	// UPrimitiveComponent / ULightComponent がオーバーライドして
	// FScene のダーティリストへ自分を積む (フラグで二重登録防止)。
	virtual void MarkRenderTransformDirty();

	bool IsRenderTransformDirty() const { return m_RenderTransformDirty; }
	void ClearRenderTransformDirty() { m_RenderTransformDirty = false; }

	XMFLOAT3 GetRelativeLocation() const { return m_RelativeLocation; }
	XMFLOAT3 GetRelativeRotation() const { return m_RelativeRotation; }
	XMFLOAT3 GetRelativeScale3D() const { return m_RelativeScale3D; }

	// ---- ワールドトランスフォーム ----
	XMMATRIX GetLocalMatrix() const;		// S * R * T
	XMMATRIX GetComponentToWorld() const;	// 親チェーンを合成
	XMFLOAT3 GetComponentLocation() const;

	XMFLOAT3 GetForwardVector() const;
	XMFLOAT3 GetRightVector() const;
	XMFLOAT3 GetUpVector() const;

	// ---- 境界 (フラスタムカリング用) ----
	// ローカル境界をワールドへ変換して返す (USceneComponent::CalcBounds)。
	// 既定はトランスフォーム原点の点境界 (半径 0)。描画されるコンポーネントは
	// 各自オーバーライドすること (UStaticMeshComponent はメッシュ AABB、
	// UPolygon2DComponent は「無限」境界 = 常に可視)。
	virtual FBoxSphereBounds CalcBounds(const XMMATRIX& LocalToWorld) const;

	// m_Bounds = CalcBounds(GetComponentToWorld()) (UpdateBounds)。
	// UPrimitiveComponent::SendRenderTransform が毎フレーム呼ぶ。
	void UpdateBounds();

	const FBoxSphereBounds& GetBounds() const { return m_Bounds; }
};
