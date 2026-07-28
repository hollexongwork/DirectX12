#include "Main.h"
#include "RenderManager.h"
#include "StaticMeshComponent.h"
#include "PrimitiveSceneProxy.h"

// ============================================================
//  FStaticMeshSceneProxy
//  UStaticMeshComponent のレンダー側ミラー (
//  FStaticMeshSceneProxy 相当)。生成時にマテリアルスロットを
//  スナップショットする:
//    - Material   : 値コピー (以後コンポーネント側の編集は
//                    MarkRenderStateDirty による再生成で反映)
//    - テクスチャ : GPU リソースへの参照 (ロード後は不変)
//    - メッシュ   : 共有レンダーデータへの参照
//                    (UStaticMesh::RenderData 相当)
//
//  Blend Mode / Two Sided:
//    - Opaque / Masked サブセット -> DrawPrimitive (ベースパス)
//    - Translucent / Additive サブセット -> DrawTranslucency
//      (トランスルーセンシーパス, SceneColor へフォワード合成)
//    - PSO はサブセットごとにマテリアルから選択する
//      (BasePass / BasePassTwoSided / Translucency 系 / ShadowDepth 系)
// ============================================================

class FStaticMeshSceneProxy : public FPrimitiveSceneProxy
{
private:
	struct FSlot
	{
		Material       Mat;
		const TEXTURE* BaseColor = nullptr;
		const TEXTURE* Normal = nullptr;
		const TEXTURE* ARM = nullptr;
	};

	FBXModel*          m_Mesh = nullptr;
	std::vector<FSlot> m_Slots;

	// サブセット -> マテリアルスロット解決 (範囲外は末尾へクランプ)
	const FSlot& ResolveSlot(unsigned int SubsetIndex) const
	{
		unsigned int slotIndex = m_Mesh->GetMaterialIndex(SubsetIndex);
		if (slotIndex >= m_Slots.size())
		{
			slotIndex = static_cast<unsigned int>(m_Slots.size()) - 1;
		}
		return m_Slots[slotIndex];
	}

	// マテリアルスロットのテクスチャ (t0/t1/t2) + 定数 (b2) をバインド
	void BindSlot(RenderManager* RM, const FSlot& Slot) const
	{
		// テクスチャは存在するものだけバインド (従来挙動を踏襲)
		if (Slot.BaseColor) RM->SetTexture(RenderManager::TEXTURE_TYPE::BASE_COLOR, Slot.BaseColor);
		if (Slot.Normal)    RM->SetTexture(RenderManager::TEXTURE_TYPE::NORMAL, Slot.Normal);
		if (Slot.ARM)       RM->SetTexture(RenderManager::TEXTURE_TYPE::MSRA, Slot.ARM);

		Slot.Mat.Bind(RM);
	}

public:
	FStaticMeshSceneProxy(UStaticMeshComponent* Component)
		: FPrimitiveSceneProxy(Component)
		, m_Mesh(&Component->GetStaticMesh())
	{
		const unsigned int num = Component->GetNumMaterialSlots();
		m_Slots.resize(num);

		for (unsigned int i = 0; i < num; ++i)
		{
			const FMaterialSlot& src = Component->GetMaterialSlot(i);
			m_Slots[i].Mat = src.Mat;
			m_Slots[i].BaseColor = src.BaseColor.get();
			m_Slots[i].Normal = src.Normal.get();
			m_Slots[i].ARM = src.ARM.get();
		}
	}

	// Distance Field Shadows: 有効な SDF を持つメッシュを返す
	const FBXModel* GetDistanceFieldMesh() const override
	{
		return (m_Mesh != nullptr && m_Mesh->IsLoaded() && m_Mesh->GetDistanceField().bValid)
			? m_Mesh : nullptr;
	}

	// ---- FPrimitiveViewRelevance (全マテリアルスロットの OR 集約) ----
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

	// ---- ベースパス (Opaque / Masked サブセット -> G-Buffer) ----
	void DrawPrimitive(RenderManager* RM) const override
	{
		if (!m_Mesh->IsLoaded()) return;

		// ---- PRIMITIVE 定数 (プロキシに積まれたワールド行列) ----
		UploadPrimitiveConstant(RM);

		// ---- サブセットごとにマテリアルスロットをバインドして描画 ----
		unsigned int subsetCount = m_Mesh->GetSubsetCount();

		for (unsigned int i = 0; i < subsetCount; ++i)
		{
			const FSlot& slot = ResolveSlot(i);

			// Translucent / Additive はトランスルーセンシーパスが描く
			if (IsTranslucentBlendMode(slot.Mat.GetBlendMode()))
			{
				continue;
			}

			// Two Sided はカリング無効 PSO (Masked の clip は
			// GeometryPS 内の動的分岐なので PSO は共通)
			RM->SetPipelineState(slot.Mat.IsTwoSided() ? "BasePassTwoSided" : "BasePass");

			BindSlot(RM, slot);

			m_Mesh->DrawSubset(i);
		}
	}

	// ---- トランスルーセンシーパス (Translucent / Additive サブセット) ----
	// FSceneRenderer::RenderTranslucency から後→前ソート順で呼ばれ、
	// 確定済み SceneColor へフォワードシェーディングで合成する。
	void DrawTranslucency(RenderManager* RM,
		ETranslucencyDrawMode Mode = ETranslucencyDrawMode::Standard) const override
	{
		if (!m_Mesh->IsLoaded()) return;

		UploadPrimitiveConstant(RM);

		unsigned int subsetCount = m_Mesh->GetSubsetCount();

		for (unsigned int i = 0; i < subsetCount; ++i)
		{
			const FSlot& slot = ResolveSlot(i);

			const EBlendMode blendMode = slot.Mat.GetBlendMode();
			if (!IsTranslucentBlendMode(blendMode))
			{
				continue;
			}

			const bool bTwoSided = slot.Mat.IsTwoSided();
			const bool bAdditive = (blendMode == EBlendMode::BLEND_Additive);

			// ---- 描画モード x Blend Mode x Two Sided -> PSO ----
			// Additive は加算合成で順序非依存のため深度プリパス不要:
			//   DepthPrepass では何も描かず、ColorEqual で従来 PSO のまま描く
			const char* pipeline = nullptr;
			switch (Mode)
			{
			case ETranslucencyDrawMode::DepthPrepass:
				if (bAdditive)
					continue;
				pipeline = bTwoSided ? "TranslucencyDepthPrepassTwoSided" : "TranslucencyDepthPrepass";
				break;

			case ETranslucencyDrawMode::ColorEqual:
				if (bAdditive)
					pipeline = bTwoSided ? "TranslucencyAdditiveTwoSided" : "TranslucencyAdditive";
				else
					pipeline = bTwoSided ? "TranslucencyEqualTwoSided" : "TranslucencyEqual";
				break;

			case ETranslucencyDrawMode::Standard:
			default:
				if (bAdditive)
					pipeline = bTwoSided ? "TranslucencyAdditiveTwoSided" : "TranslucencyAdditive";
				else
					pipeline = bTwoSided ? "TranslucencyTwoSided" : "Translucency";
				break;
			}
			RM->SetPipelineState(pipeline);

			BindSlot(RM, slot);

			m_Mesh->DrawSubset(i);
		}
	}

	// ---- シャドウ深度パス ----
	// Opaque   : マテリアルなしの深度のみ (従来通り)
	// Masked   : t0 + b2 をバインドし ShadowDepthMaskedPS が
	//            OpacityMask を clip する
	// Two Sided: カリング無効 PSO (両面が影を落とす)
	// Translucent / Additive : シャドウマップに描かない (UE5 既定)
	void DrawShadowDepth(RenderManager* RM) const override
	{
		if (!m_Mesh->IsLoaded()) return;

		UploadPrimitiveConstant(RM);

		unsigned int subsetCount = m_Mesh->GetSubsetCount();
		for (unsigned int i = 0; i < subsetCount; ++i)
		{
			const FSlot& slot = ResolveSlot(i);

			const EBlendMode blendMode = slot.Mat.GetBlendMode();
			if (IsTranslucentBlendMode(blendMode))
			{
				continue;
			}

			const bool bTwoSided = slot.Mat.IsTwoSided();

			// Masked は BaseColor テクスチャがある場合のみ clip 経路
			// (テクスチャ未設定時は不透明として扱う)
			if (IsMaskedBlendMode(blendMode) && slot.BaseColor != nullptr)
			{
				RM->SetPipelineState(bTwoSided ? "ShadowDepthMaskedTwoSided" : "ShadowDepthMasked");
				RM->SetTexture(RenderManager::TEXTURE_TYPE::BASE_COLOR, slot.BaseColor);
				slot.Mat.Bind(RM);	// b2 (OpacityMaskClipValue)
			}
			else
			{
				RM->SetPipelineState(bTwoSided ? "ShadowDepthTwoSided" : "ShadowDepth");
			}

			m_Mesh->DrawSubset(i);
		}
	}
};

// ============================================================
//  UStaticMeshComponent
// ============================================================

UStaticMeshComponent::UStaticMeshComponent()
{
	// 最低 1 スロットは常に確保しておく
	m_MaterialSlots.resize(1);
}

bool UStaticMeshComponent::SetStaticMesh(const char* FilePath, bool FlipUV)
{
	bool result = m_Mesh.Load(FilePath, FlipUV);
	MarkRenderStateDirty();
	return result;
}

void UStaticMeshComponent::SetNumMaterialSlots(unsigned int Num)
{
	if (Num < 1) Num = 1;
	m_MaterialSlots.resize(Num);
	MarkRenderStateDirty();
}

Material& UStaticMeshComponent::GetMaterial(unsigned int SlotIndex)
{
	assert(SlotIndex < m_MaterialSlots.size());
	return m_MaterialSlots[SlotIndex].Mat;
}

FMaterialSlot& UStaticMeshComponent::GetMaterialSlot(unsigned int SlotIndex)
{
	assert(SlotIndex < m_MaterialSlots.size());
	return m_MaterialSlots[SlotIndex];
}

void UStaticMeshComponent::SetBaseColorTexture(unsigned int SlotIndex, const char* FilePath)
{
	assert(SlotIndex < m_MaterialSlots.size());

	// BaseColor は sRGB として読む (DDS_LOADER_FORCE_SRGB)
	m_MaterialSlots[SlotIndex].BaseColor = RenderManager::GetInstance()->LoadTexture(FilePath, true);
	MarkRenderStateDirty();
}

void UStaticMeshComponent::SetNormalTexture(unsigned int SlotIndex, const char* FilePath)
{
	assert(SlotIndex < m_MaterialSlots.size());
	m_MaterialSlots[SlotIndex].Normal = RenderManager::GetInstance()->LoadTexture(FilePath);
	MarkRenderStateDirty();
}

void UStaticMeshComponent::SetARMTexture(unsigned int SlotIndex, const char* FilePath)
{
	assert(SlotIndex < m_MaterialSlots.size());
	m_MaterialSlots[SlotIndex].ARM = RenderManager::GetInstance()->LoadTexture(FilePath);
	MarkRenderStateDirty();
}

FBoxSphereBounds UStaticMeshComponent::CalcBounds(const XMMATRIX& LocalToWorld) const
{
	// メッシュのローカル AABB をワールドへ変換して返す
	// (UStaticMeshComponent::CalcBounds 相当)
	if (m_Mesh.IsLoaded())
	{
		return m_Mesh.GetLocalBounds().TransformBy(LocalToWorld);
	}

	// メッシュ未設定時は基底の点境界
	return UPrimitiveComponent::CalcBounds(LocalToWorld);
}

FPrimitiveSceneProxy* UStaticMeshComponent::CreateSceneProxy()
{
	return new FStaticMeshSceneProxy(this);
}
