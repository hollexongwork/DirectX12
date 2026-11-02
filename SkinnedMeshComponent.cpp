#include "Main.h"
#include "RenderManager.h"
#include "AssetManager.h"
#include "SkinnedMeshComponent.h"
#include "PrimitiveSceneProxy.h"
#include "Scene.h"

// ============================================================
//  FSkeletalMeshSceneProxy
//  USkinnedMeshComponent のレンダー側ミラー (FSkeletalMeshSceneProxy)。
//  FStaticMeshSceneProxy (StaticMeshComponent.cpp) と同じ構造で、
//    - メッシュ / マテリアル / テクスチャは生成時スナップショット
//    - ボーン行列は FSkeletalMeshObjectGPUSkin (コンポーネントと共有) を
//      描画時に t39 (今) / t40 (前フレーム) へバインド
//    - PSO は "<既存名>_GPUSkin" (GPUSkinVertexFactory のインプットレイアウト)
//    - AlwaysHasVelocity = true (動かないフレームでもボーンの速度を描く)
//    - Distance Field / Lumen カードには参加しない
// ============================================================

class FSkeletalMeshSceneProxy : public FPrimitiveSceneProxy
{
private:
	using FSlot = FMaterialSlot;

	std::shared_ptr<USkeletalMesh>              m_Mesh;
	std::shared_ptr<FSkeletalMeshObjectGPUSkin> m_MeshObject;
	std::vector<FSlot>                          m_Slots;

	const FSlot& ResolveSlot(unsigned int SectionIndex) const
	{
		unsigned int slotIndex = m_Mesh->GetSectionMaterialIndex(SectionIndex);
		if (slotIndex >= m_Slots.size())
		{
			slotIndex = static_cast<unsigned int>(m_Slots.size()) - 1;
		}
		return m_Slots[slotIndex];
	}

	void BindSlot(RenderManager* RM, const FSlot& Slot) const
	{
		if (Slot.BaseColor) RM->SetTexture(RenderManager::TEXTURE_TYPE::BASE_COLOR, Slot.BaseColor.get());
		if (Slot.Normal)    RM->SetTexture(RenderManager::TEXTURE_TYPE::NORMAL, Slot.Normal.get());
		if (Slot.ARM)       RM->SetTexture(RenderManager::TEXTURE_TYPE::MSRA, Slot.ARM.get());

		Slot.Mat.Bind(RM);
	}

	// t39 (今フレームのボーン行列)。ベロシティは t40 も (BindPreviousBones)
	void BindBones(RenderManager* RM) const
	{
		RM->BindRootTableBySRVIndex((unsigned int)RenderManager::TEXTURE_TYPE::BONE_MATRICES,
			m_MeshObject->GetBoneBufferSRVIndex());
	}

	void BindPreviousBones(RenderManager* RM) const
	{
		RM->BindRootTableBySRVIndex((unsigned int)RenderManager::TEXTURE_TYPE::PREVIOUS_BONE_MATRICES,
			m_MeshObject->GetPreviousBoneBufferSRVIndex(FScene::GetFrameNumber()));
	}

	bool IsMeshValid() const
	{
		return m_Mesh != nullptr && m_Mesh->IsLoaded() && m_MeshObject != nullptr && m_MeshObject->IsValid()
			&& !m_Slots.empty();
	}

	static bool IsResponsiveAASlot(const FSlot& Slot, bool bForceAll)
	{
		return IsTranslucentBlendMode(Slot.Mat.GetBlendMode()) && (bForceAll || Slot.Mat.ShouldEnableResponsiveAA());
	}

public:
	FSkeletalMeshSceneProxy(USkinnedMeshComponent* Component)
		: FPrimitiveSceneProxy(Component)
		, m_Mesh(Component->GetSkeletalMesh())
		, m_MeshObject(Component->GetMeshObject())
		, m_Slots(Component->GetMaterialSlots())
	{
		// スケルタルメッシュは Distance Field / Lumen Surface Cache を持たない
		m_bAffectDistanceField = false;
	}

	// 動かないフレームでもボーンが動くので毎フレーム速度を描く (FSkeletalMeshSceneProxy)
	bool AlwaysHasVelocity() const override { return true; }

	FPrimitiveViewRelevance GetViewRelevance() const override
	{
		FPrimitiveViewRelevance relevance;
		relevance.bOpaque = false;

		for (const FSlot& slot : m_Slots)
		{
			switch (slot.Mat.GetBlendMode())
			{
			case EBlendMode::BLEND_Masked:
				relevance.bMasked = true;
				break;
			case EBlendMode::BLEND_Translucent:
			case EBlendMode::BLEND_Additive:
				relevance.bNormalTranslucency = true;
				break;
			case EBlendMode::BLEND_Opaque:
			default:
				relevance.bOpaque = true;
				break;
			}
		}
		return relevance;
	}

	// ---- ベースパス ----
	void DrawPrimitive(RenderManager* RM) const override
	{
		if (!IsMeshValid()) return;

		UploadPrimitiveConstant(RM);
		BindBones(RM);

		const unsigned int numSections = m_Mesh->GetNumSections();
		for (unsigned int i = 0; i < numSections; ++i)
		{
			const FSlot& slot = ResolveSlot(i);
			if (IsTranslucentBlendMode(slot.Mat.GetBlendMode()))
			{
				continue;
			}

			RM->SetPipelineState(slot.Mat.IsTwoSided() ? "BasePassTwoSided_GPUSkin" : "BasePass_GPUSkin");
			BindSlot(RM, slot);
			m_Mesh->DrawSection(RM, i);
		}
	}

	// ---- トランスルーセンシー ----
	void DrawTranslucency(RenderManager* RM, ETranslucencyDrawMode Mode = ETranslucencyDrawMode::Standard) const override
	{
		if (!IsMeshValid()) return;

		UploadPrimitiveConstant(RM);
		BindBones(RM);

		const unsigned int numSections = m_Mesh->GetNumSections();
		for (unsigned int i = 0; i < numSections; ++i)
		{
			const FSlot& slot = ResolveSlot(i);
			const EBlendMode blendMode = slot.Mat.GetBlendMode();
			if (!IsTranslucentBlendMode(blendMode))
			{
				continue;
			}

			const bool bTwoSided = slot.Mat.IsTwoSided();
			const bool bAdditive = (blendMode == EBlendMode::BLEND_Additive);

			const char* pipeline = nullptr;
			switch (Mode)
			{
			case ETranslucencyDrawMode::DepthPrepass:
				if (bAdditive)
					continue;
				pipeline = bTwoSided ? "TranslucencyDepthPrepassTwoSided_GPUSkin" : "TranslucencyDepthPrepass_GPUSkin";
				break;

			case ETranslucencyDrawMode::ColorEqual:
				if (bAdditive)
					pipeline = bTwoSided ? "TranslucencyAdditiveTwoSided_GPUSkin" : "TranslucencyAdditive_GPUSkin";
				else
					pipeline = bTwoSided ? "TranslucencyEqualTwoSided_GPUSkin" : "TranslucencyEqual_GPUSkin";
				break;

			case ETranslucencyDrawMode::Standard:
			default:
				if (bAdditive)
					pipeline = bTwoSided ? "TranslucencyAdditiveTwoSided_GPUSkin" : "TranslucencyAdditive_GPUSkin";
				else
					pipeline = bTwoSided ? "TranslucencyTwoSided_GPUSkin" : "Translucency_GPUSkin";
				break;
			}
			RM->SetPipelineState(pipeline);
			BindSlot(RM, slot);
			m_Mesh->DrawSection(RM, i);
		}
	}

	// ---- シャドウ深度 ----
	void DrawShadowDepth(RenderManager* RM) const override
	{
		if (!IsMeshValid()) return;

		UploadPrimitiveConstant(RM);
		BindBones(RM);

		const unsigned int numSections = m_Mesh->GetNumSections();
		for (unsigned int i = 0; i < numSections; ++i)
		{
			const FSlot& slot = ResolveSlot(i);
			const EBlendMode blendMode = slot.Mat.GetBlendMode();
			if (IsTranslucentBlendMode(blendMode))
			{
				continue;
			}

			const bool bTwoSided = slot.Mat.IsTwoSided();
			if (IsMaskedBlendMode(blendMode) && slot.BaseColor != nullptr)
			{
				RM->SetPipelineState(bTwoSided ? "ShadowDepthMaskedTwoSided_GPUSkin" : "ShadowDepthMasked_GPUSkin");
				RM->SetTexture(RenderManager::TEXTURE_TYPE::BASE_COLOR, slot.BaseColor.get());
				slot.Mat.Bind(RM);
			}
			else
			{
				RM->SetPipelineState(bTwoSided ? "ShadowDepthTwoSided_GPUSkin" : "ShadowDepth_GPUSkin");
			}
			m_Mesh->DrawSection(RM, i);
		}
	}

	// ---- ベロシティ (b1 = LocalToWorld + PreviousLocalToWorld, t39 + t40) ----
	void DrawVelocity(RenderManager* RM, const XMFLOAT4X4& PreviousLocalToWorld) const override
	{
		if (!IsMeshValid()) return;

		UploadPrimitiveConstant(RM, &PreviousLocalToWorld);
		BindBones(RM);
		BindPreviousBones(RM);

		const unsigned int numSections = m_Mesh->GetNumSections();
		for (unsigned int i = 0; i < numSections; ++i)
		{
			const FSlot& slot = ResolveSlot(i);
			const EBlendMode blendMode = slot.Mat.GetBlendMode();
			if (IsTranslucentBlendMode(blendMode))
			{
				continue;
			}

			const bool bTwoSided = slot.Mat.IsTwoSided();
			if (IsMaskedBlendMode(blendMode) && slot.BaseColor != nullptr)
			{
				RM->SetPipelineState(bTwoSided ? "VelocityMaskedTwoSided_GPUSkin" : "VelocityMasked_GPUSkin");
				RM->SetTexture(RenderManager::TEXTURE_TYPE::BASE_COLOR, slot.BaseColor.get());
				slot.Mat.Bind(RM);
			}
			else
			{
				RM->SetPipelineState(bTwoSided ? "VelocityTwoSided_GPUSkin" : "Velocity_GPUSkin");
			}
			m_Mesh->DrawSection(RM, i);
		}
	}

	// ---- Responsive AA マスク ----
	bool HasResponsiveAATranslucency(bool bForceAll) const override
	{
		if (!IsMeshValid()) return false;

		const unsigned int numSections = m_Mesh->GetNumSections();
		for (unsigned int i = 0; i < numSections; ++i)
		{
			if (IsResponsiveAASlot(ResolveSlot(i), bForceAll))
				return true;
		}
		return false;
	}

	void DrawResponsiveAA(RenderManager* RM, bool bForceAll) const override
	{
		if (!IsMeshValid()) return;

		UploadPrimitiveConstant(RM);
		BindBones(RM);

		const unsigned int numSections = m_Mesh->GetNumSections();
		for (unsigned int i = 0; i < numSections; ++i)
		{
			const FSlot& slot = ResolveSlot(i);
			if (!IsResponsiveAASlot(slot, bForceAll))
				continue;

			RM->SetPipelineState(slot.Mat.IsTwoSided() ? "ResponsiveAATwoSided_GPUSkin" : "ResponsiveAA_GPUSkin");
			m_Mesh->DrawSection(RM, i);
		}
	}

	// Lumen カードキャプチャ: スケルタルメッシュは Surface Cache に参加しない
	// (GetDistanceFieldMesh が nullptr なのでそもそも呼ばれない)
	void DrawCardCapture(RenderManager* RM) const override { (void)RM; }
};

// ============================================================
//  USkinnedMeshComponent
// ============================================================

USkinnedMeshComponent::USkinnedMeshComponent()
{
	m_MaterialSlots.resize(1);

	// スケルタルメッシュは Mesh Distance Field を持たない
	m_bAffectDistanceFieldLighting = false;
}

bool USkinnedMeshComponent::SetSkeletalMesh(const char* FilePath)
{
	SetSkeletalMesh(FAssetManager::GetInstance()->LoadSkeletalMesh(FilePath));
	return m_SkeletalMesh != nullptr && m_SkeletalMesh->IsLoaded();
}

void USkinnedMeshComponent::SetSkeletalMesh(std::shared_ptr<USkeletalMesh> Mesh)
{
	m_SkeletalMesh = std::move(Mesh);

	// マテリアルスロットはメッシュのスロット数まで広げる (既存の設定は保つ)
	if (m_SkeletalMesh && m_SkeletalMesh->IsLoaded() && m_MaterialSlots.size() < m_SkeletalMesh->GetNumMaterials())
	{
		m_MaterialSlots.resize(m_SkeletalMesh->GetNumMaterials());
	}

	CreateMeshObject();

	MarkRenderStateDirty();			// プロキシ再生成 (新しいメッシュ / メッシュオブジェクト)
	MarkRenderTransformDirty();		// 境界
	MarkRenderDynamicDataDirty();	// 初期ポーズのボーン行列
}

void USkinnedMeshComponent::CreateMeshObject()
{
	m_MeshObject.reset();
	m_ComponentSpaceTransforms.clear();
	m_ReferenceToLocal.clear();

	if (!m_SkeletalMesh || !m_SkeletalMesh->IsLoaded())
	{
		return;
	}

	// リファレンスポーズで初期化 (USkinnedMeshComponent::AllocateTransformData)
	m_SkeletalMesh->GetRefSkeleton().ComputeComponentSpaceRefPose(m_ComponentSpaceTransforms);
	m_ReferenceToLocal.resize(m_ComponentSpaceTransforms.size());

	// 旧オブジェクトは旧プロキシが shared_ptr で生かし、プロキシ再生成時に解放される
	m_MeshObject = std::make_shared<FSkeletalMeshObjectGPUSkin>(static_cast<unsigned int>(m_SkeletalMesh->GetNumBones()));
}

// ---- ボーン ----

int USkinnedMeshComponent::GetBoneIndex(const std::string& BoneName) const
{
	return m_SkeletalMesh ? m_SkeletalMesh->GetRefSkeleton().FindBoneIndex(BoneName) : INDEX_NONE;
}

std::string USkinnedMeshComponent::GetBoneName(int BoneIndex) const
{
	return m_SkeletalMesh ? m_SkeletalMesh->GetRefSkeleton().GetBoneName(BoneIndex) : std::string();
}

int USkinnedMeshComponent::GetParentBone(int BoneIndex) const
{
	return m_SkeletalMesh ? m_SkeletalMesh->GetRefSkeleton().GetParentIndex(BoneIndex) : INDEX_NONE;
}

FTransform USkinnedMeshComponent::GetBoneTransform(int BoneIndex) const
{
	if (BoneIndex < 0 || BoneIndex >= static_cast<int>(m_ComponentSpaceTransforms.size()))
	{
		return FTransform::Identity();
	}
	return m_ComponentSpaceTransforms[BoneIndex] * FTransform::FromMatrix(GetComponentToWorld());
}

XMMATRIX USkinnedMeshComponent::GetBoneMatrix(int BoneIndex) const
{
	if (BoneIndex < 0 || BoneIndex >= static_cast<int>(m_ComponentSpaceTransforms.size()))
	{
		return XMMatrixIdentity();
	}
	return m_ComponentSpaceTransforms[BoneIndex].ToMatrixWithScale() * GetComponentToWorld();
}

XMFLOAT3 USkinnedMeshComponent::GetBoneLocation(int BoneIndex) const
{
	XMFLOAT3 location = { 0.0f, 0.0f, 0.0f };
	XMStoreFloat3(&location, GetBoneMatrix(BoneIndex).r[3]);
	return location;
}

// ---- マテリアルスロット ----

void USkinnedMeshComponent::SetNumMaterialSlots(unsigned int Num)
{
	if (Num < 1) Num = 1;
	m_MaterialSlots.resize(Num);
	MarkRenderStateDirty();
}

Material& USkinnedMeshComponent::GetMaterial(unsigned int SlotIndex)
{
	assert(SlotIndex < m_MaterialSlots.size());
	return m_MaterialSlots[SlotIndex].Mat;
}

void USkinnedMeshComponent::SetBaseColorTexture(unsigned int SlotIndex, const char* FilePath)
{
	assert(SlotIndex < m_MaterialSlots.size());
	m_MaterialSlots[SlotIndex].BaseColor = FAssetManager::GetInstance()->LoadTexture(FilePath, true);
	MarkRenderStateDirty();
}

void USkinnedMeshComponent::SetNormalTexture(unsigned int SlotIndex, const char* FilePath)
{
	assert(SlotIndex < m_MaterialSlots.size());
	m_MaterialSlots[SlotIndex].Normal = FAssetManager::GetInstance()->LoadTexture(FilePath);
	MarkRenderStateDirty();
}

void USkinnedMeshComponent::SetARMTexture(unsigned int SlotIndex, const char* FilePath)
{
	assert(SlotIndex < m_MaterialSlots.size());
	m_MaterialSlots[SlotIndex].ARM = FAssetManager::GetInstance()->LoadTexture(FilePath);
	MarkRenderStateDirty();
}

// ---- 境界 ----

FBoxSphereBounds USkinnedMeshComponent::CalcBounds(const XMMATRIX& LocalToWorld) const
{
	if (!m_SkeletalMesh || !m_SkeletalMesh->IsLoaded())
	{
		return UPrimitiveComponent::CalcBounds(LocalToWorld);
	}

	FBoxSphereBounds local;
	if (!m_ComponentSpaceTransforms.empty())
	{
		// 現在のポーズのボーン位置 AABB + 頂点からボーンまでの最大距離 (ポーズに追従する境界)
		XMFLOAT3 boundsMin = { 1.0e30f, 1.0e30f, 1.0e30f };
		XMFLOAT3 boundsMax = { -1.0e30f, -1.0e30f, -1.0e30f };
		for (const FTransform& bone : m_ComponentSpaceTransforms)
		{
			const XMFLOAT3& p = bone.Translation;
			boundsMin.x = fminf(boundsMin.x, p.x); boundsMin.y = fminf(boundsMin.y, p.y); boundsMin.z = fminf(boundsMin.z, p.z);
			boundsMax.x = fmaxf(boundsMax.x, p.x); boundsMax.y = fmaxf(boundsMax.y, p.y); boundsMax.z = fmaxf(boundsMax.z, p.z);
		}
		const float r = m_SkeletalMesh->GetMaxBoneInfluenceRadius();
		boundsMin = { boundsMin.x - r, boundsMin.y - r, boundsMin.z - r };
		boundsMax = { boundsMax.x + r, boundsMax.y + r, boundsMax.z + r };
		local = FBoxSphereBounds::FromMinMax(boundsMin, boundsMax);
	}
	else
	{
		local = m_SkeletalMesh->GetBounds();
	}

	const float scale = fmaxf(BoundsScale, 0.0f);
	local.BoxExtent = { local.BoxExtent.x * scale, local.BoxExtent.y * scale, local.BoxExtent.z * scale };
	local.SphereRadius *= scale;

	return local.TransformBy(LocalToWorld);
}

// ---- レンダー側 ----

FPrimitiveSceneProxy* USkinnedMeshComponent::CreateSceneProxy()
{
	return new FSkeletalMeshSceneProxy(this);
}

void USkinnedMeshComponent::SendRenderDynamicData_Concurrent()
{
	if (!m_MeshObject || !m_SkeletalMesh || !m_SkeletalMesh->IsLoaded())
	{
		return;
	}

	// UE: UpdateRefToLocalMatrices -> ReferenceToLocal[i] = RefBasesInvMatrix[i] * ComponentSpaceTransforms[i]
	const std::vector<XMFLOAT4X4>& invRef = m_SkeletalMesh->GetRefBasesInvMatrix();
	const size_t num = (std::min)(invRef.size(), m_ComponentSpaceTransforms.size());
	m_ReferenceToLocal.resize(num);
	for (size_t i = 0; i < num; ++i)
	{
		XMStoreFloat4x4(&m_ReferenceToLocal[i],
			XMLoadFloat4x4(&invRef[i]) * m_ComponentSpaceTransforms[i].ToMatrixWithScale());
	}

	m_MeshObject->Update(m_ReferenceToLocal, FScene::GetFrameNumber());
}
