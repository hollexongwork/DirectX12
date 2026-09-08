#include "Main.h"
#include "RenderManager.h"
#include "AssetManager.h"
#include "StaticMeshComponent.h"
#include "PrimitiveSceneProxy.h"

// ============================================================
//  FStaticMeshSceneProxy
//  UStaticMeshComponent のレンダー側ミラー (
//  FStaticMeshSceneProxy 相当)。生成時に全てを値スナップショット
//  するため、以後コンポーネントには一切触れない:
//    - Material   : 値コピー (以後コンポーネント側の編集は
//                    MarkRenderStateDirty による再生成で反映)
//    - テクスチャ : shared_ptr のコピー (FAssetManager のキャッシュと
//                    共有。プロキシ自身が参照カウントで生存を保証する)
//    - メッシュ   : shared_ptr のコピー (FStaticMeshSceneProxy が
//                    レンダーデータを参照カウントで保持するのに相当。
//                    コンポーネント側でメッシュが差し替えられても、
//                    旧プロキシが in-flight の間は旧メッシュが生存する)
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
		Material                 Mat;
		std::shared_ptr<TEXTURE> BaseColor;
		std::shared_ptr<TEXTURE> Normal;
		std::shared_ptr<TEXTURE> ARM;
	};

	// 共有メッシュレンダーデータ (UStaticMesh::RenderData 相当)。
	// メッシュ未設定のコンポーネントでは nullptr。
	std::shared_ptr<FBXModel> m_Mesh;
	std::vector<FSlot>        m_Slots;

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
		if (Slot.BaseColor) RM->SetTexture(RenderManager::TEXTURE_TYPE::BASE_COLOR, Slot.BaseColor.get());
		if (Slot.Normal)    RM->SetTexture(RenderManager::TEXTURE_TYPE::NORMAL, Slot.Normal.get());
		if (Slot.ARM)       RM->SetTexture(RenderManager::TEXTURE_TYPE::MSRA, Slot.ARM.get());

		Slot.Mat.Bind(RM);
	}

	bool IsMeshValid() const { return m_Mesh != nullptr && m_Mesh->IsLoaded(); }

public:
	FStaticMeshSceneProxy(UStaticMeshComponent* Component)
		: FPrimitiveSceneProxy(Component)
		, m_Mesh(Component->GetStaticMesh())
	{
		const unsigned int num = Component->GetNumMaterialSlots();
		m_Slots.resize(num);

		for (unsigned int i = 0; i < num; ++i)
		{
			const FMaterialSlot& src = Component->GetMaterialSlot(i);
			m_Slots[i].Mat = src.Mat;
			m_Slots[i].BaseColor = src.BaseColor;	// shared_ptr コピー (参照カウント +1)
			m_Slots[i].Normal = src.Normal;
			m_Slots[i].ARM = src.ARM;
		}
	}

	// Distance Field Shadows: 有効な SDF を持つメッシュを返す
	const FBXModel* GetDistanceFieldMesh() const override
	{
		return (IsMeshValid() && m_Mesh->GetDistanceField().bValid)
			? m_Mesh.get() : nullptr;
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
		if (!IsMeshValid()) return;

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
		if (!IsMeshValid()) return;

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
	// Translucent / Additive : シャドウマップに描かない
	void DrawShadowDepth(RenderManager* RM) const override
	{
		if (!IsMeshValid()) return;

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
				RM->SetTexture(RenderManager::TEXTURE_TYPE::BASE_COLOR, slot.BaseColor.get());
				slot.Mat.Bind(RM);	// b2 (OpacityMaskClipValue)
			}
			else
			{
				RM->SetPipelineState(bTwoSided ? "ShadowDepthTwoSided" : "ShadowDepth");
			}

			m_Mesh->DrawSubset(i);
		}
	}

	// ---- Lumen カードキャプチャパス (FLumenSceneData::RenderCardCaptures) ----
	// b0 = カードビュー (ローカル空間オルソ) は呼び出し側が積み済み。
	// b1 へ単位行列を積み、マテリアル付きでローカル空間のまま描く。
	// PSO は常にカリング無効 (LumenCardCapture)。裏面の法線反転と
	// Masked の clip は LumenCardCapturePS 内の動的分岐が担う。
	// Translucent / Additive サブセットは Surface Cache に参加しない。
	void DrawCardCapture(RenderManager* RM) const override
	{
		if (!IsMeshValid()) return;

		// b1: 単位行列 (ローカル空間描画。単位行列は転置不要)
		PRIMITIVE_CONSTANT primitiveConstant{};
		XMStoreFloat4x4(&primitiveConstant.LocalToWorld, XMMatrixIdentity());
		RM->SetConstant(RenderManager::CONSTANT_TYPE::PRIMITIVE,
			&primitiveConstant, sizeof(primitiveConstant));

		RM->SetPipelineState("LumenCardCapture");

		unsigned int subsetCount = m_Mesh->GetSubsetCount();
		for (unsigned int i = 0; i < subsetCount; ++i)
		{
			const FSlot& slot = ResolveSlot(i);

			if (IsTranslucentBlendMode(slot.Mat.GetBlendMode()))
			{
				continue;
			}

			BindSlot(RM, slot);

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
	// FAssetManager のキャッシュから共有メッシュを取得する
	// (同じパスを使う他コンポーネントと GPU バッファを共有する)
	m_Mesh = FAssetManager::GetInstance()->LoadStaticMesh(FilePath, FlipUV);
	MarkRenderStateDirty();
	return m_Mesh != nullptr && m_Mesh->IsLoaded();
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

	// BaseColor は sRGB として読む (DDS_LOADER_FORCE_SRGB)。
	// FAssetManager のキャッシュ経由で共有テクスチャを取得する。
	m_MaterialSlots[SlotIndex].BaseColor = FAssetManager::GetInstance()->LoadTexture(FilePath, true);
	MarkRenderStateDirty();
}

void UStaticMeshComponent::SetNormalTexture(unsigned int SlotIndex, const char* FilePath)
{
	assert(SlotIndex < m_MaterialSlots.size());
	m_MaterialSlots[SlotIndex].Normal = FAssetManager::GetInstance()->LoadTexture(FilePath);
	MarkRenderStateDirty();
}

void UStaticMeshComponent::SetARMTexture(unsigned int SlotIndex, const char* FilePath)
{
	assert(SlotIndex < m_MaterialSlots.size());
	m_MaterialSlots[SlotIndex].ARM = FAssetManager::GetInstance()->LoadTexture(FilePath);
	MarkRenderStateDirty();
}

FBoxSphereBounds UStaticMeshComponent::CalcBounds(const XMMATRIX& LocalToWorld) const
{
	// メッシュのローカル AABB をワールドへ変換して返す
	// (UStaticMeshComponent::CalcBounds 相当)
	if (m_Mesh && m_Mesh->IsLoaded())
	{
		return m_Mesh->GetLocalBounds().TransformBy(LocalToWorld);
	}

	// メッシュ未設定時は基底の点境界
	return UPrimitiveComponent::CalcBounds(LocalToWorld);
}

FPrimitiveSceneProxy* UStaticMeshComponent::CreateSceneProxy()
{
	return new FStaticMeshSceneProxy(this);
}
