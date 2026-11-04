#pragma once
#include <memory>
#include <vector>
#include <string>
#include "PrimitiveComponent.h"
#include "Material.h"
#include "SkeletalMesh.h"
#include "SkeletalRenderGPUSkin.h"

// ============================================================
//  USkinnedMeshComponent
//  USkinnedMeshComponent (Components/SkinnedMeshComponent.h) に相当。
//  USkeletalMesh (FAssetManager で共有) とマテリアルスロット、
//  コンポーネント空間のボーン変換 (ComponentSpaceTransforms) を持ち、
//  レンダー側 (FSkeletalMeshSceneProxy / FSkeletalMeshObjectGPUSkin) へ
//  ボーン行列を送る。ポーズの更新 (アニメーション) は派生の
//  USkeletalMeshComponent が行う。
//
//  更新経路:
//    ポーズ確定 (FinalizeBoneTransform) -> MarkRenderDynamicDataDirty
//      -> FScene::UpdateAllPrimitiveSceneInfos
//      -> SendRenderDynamicData_Concurrent
//         (ReferenceToLocal[i] = RefBasesInvMatrix[i] * ComponentSpaceTransforms[i])
//      -> FSkeletalMeshObjectGPUSkin::Update (t39 / t40 のボーン行列)
//
//  境界: 現在のポーズのボーン位置の AABB を、メッシュの「頂点から
//  ボーンまでの最大距離」で広げたもの (USkeletalMesh::GetMaxBoneInfluenceRadius)。
//  UE のリファレンスポーズ境界 (USkeletalMesh::GetBounds) より曲げた
//  ポーズに追従する [PORT]。BoundsScale で更に広げられる。
//
//  Lumen / Distance Field には参加しない (m_bAffectDistanceFieldLighting = false。
//  UE と同じくスケルタルメッシュは Surface Cache / MDF を持たない)。
// ============================================================

class USkinnedMeshComponent : public UPrimitiveComponent
{
protected:
	std::shared_ptr<USkeletalMesh>   m_SkeletalMesh;
	std::vector<FMaterialSlot>       m_MaterialSlots;

	// コンポーネント空間のボーン変換 (USkinnedMeshComponent::GetComponentSpaceTransforms)
	std::vector<FTransform>          m_ComponentSpaceTransforms;

	// レンダー側のボーン行列バッファ (プロキシと共有。メッシュ差し替えで作り直す)
	std::shared_ptr<FSkeletalMeshObjectGPUSkin> m_MeshObject;

	// SendRenderDynamicData_Concurrent 用の作業配列 (ReferenceToLocal)
	std::vector<XMFLOAT4X4>          m_ReferenceToLocal;

	// メッシュに合わせてレンダー側オブジェクト / ポーズ配列を作り直す
	void CreateMeshObject();

public:
	// 境界の倍率 (UPrimitiveComponent::BoundsScale)
	float BoundsScale = 1.0f;

	USkinnedMeshComponent();

	// ---- メッシュ (USkinnedMeshComponent::SetSkeletalMesh / SetSkinnedAssetAndUpdate) ----
	// FAssetManager のキャッシュから共有メッシュを取得して差し替える
	bool SetSkeletalMesh(const char* FilePath);
	virtual void SetSkeletalMesh(std::shared_ptr<USkeletalMesh> Mesh);
	const std::shared_ptr<USkeletalMesh>& GetSkeletalMesh() const { return m_SkeletalMesh; }

	// ---- ボーン ----
	int GetNumBones() const { return m_SkeletalMesh ? m_SkeletalMesh->GetNumBones() : 0; }
	int GetBoneIndex(const std::string& BoneName) const;
	std::string GetBoneName(int BoneIndex) const;
	int GetParentBone(int BoneIndex) const;

	const std::vector<FTransform>& GetComponentSpaceTransforms() const { return m_ComponentSpaceTransforms; }

	// ボーンのワールド変換 (USkinnedMeshComponent::GetBoneTransform / GetBoneMatrix)。
	// 範囲外は単位変換
	FTransform GetBoneTransform(int BoneIndex) const;
	XMMATRIX   GetBoneMatrix(int BoneIndex) const;
	// ボーンのワールド位置 (GetBoneLocation)
	XMFLOAT3   GetBoneLocation(int BoneIndex) const;

	// ---- マテリアルスロット (UStaticMeshComponent と同じ API) ----
	void         SetNumMaterialSlots(unsigned int Num);
	unsigned int GetNumMaterialSlots() const { return static_cast<unsigned int>(m_MaterialSlots.size()); }

	// 直接書き換えた場合は MarkRenderStateDirty() を呼ぶこと
	Material& GetMaterial(unsigned int SlotIndex = 0);
	const std::vector<FMaterialSlot>& GetMaterialSlots() const { return m_MaterialSlots; }

	void SetBaseColorTexture(unsigned int SlotIndex, const char* FilePath);
	void SetNormalTexture(unsigned int SlotIndex, const char* FilePath);
	void SetARMTexture(unsigned int SlotIndex, const char* FilePath);

	// ---- 境界 ----
	FBoxSphereBounds CalcBounds(const XMMATRIX& LocalToWorld) const override;

	// ---- レンダー側 ----
	FPrimitiveSceneProxy* CreateSceneProxy() override;
	void SendRenderDynamicData_Concurrent() override;

	const std::shared_ptr<FSkeletalMeshObjectGPUSkin>& GetMeshObject() const { return m_MeshObject; }
};
