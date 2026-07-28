#pragma once
#include "RenderManager.h"
#include "FBXModel.h"
#include "Material.h"
#include "PrimitiveComponent.h"

// ============================================================
//  UStaticMeshComponent
//  UStaticMeshComponent に相当。FBX メッシュと
//  マテリアルスロット (サブセットの MaterialIndex に対応) を持つ。
//  描画本体は FStaticMeshSceneProxy (StaticMeshComponent.cpp 内)
//  へ移設済み。マテリアル / メッシュ / テクスチャの変更は
//  MarkRenderStateDirty 経由で次フレームのプロキシ再生成に反映される。
// ============================================================

struct FMaterialSlot
{
	Material                 Mat;
	std::unique_ptr<TEXTURE> BaseColor;
	std::unique_ptr<TEXTURE> Normal;
	std::unique_ptr<TEXTURE> ARM;
};

class UStaticMeshComponent : public UPrimitiveComponent
{
protected:
	FBXModel                   m_Mesh;
	std::vector<FMaterialSlot> m_MaterialSlots;

public:
	UStaticMeshComponent();

	// ---- メッシュ ----
	bool SetStaticMesh(const char* FilePath, bool FlipUV = true);
	FBXModel& GetStaticMesh() { return m_Mesh; }

	// ---- マテリアルスロット ----
	void         SetNumMaterialSlots(unsigned int Num);
	unsigned int GetNumMaterialSlots() const { return static_cast<unsigned int>(m_MaterialSlots.size()); }

	// 直接書き換えた場合は MarkRenderStateDirty() を呼ぶこと
	Material&      GetMaterial(unsigned int SlotIndex = 0);
	FMaterialSlot& GetMaterialSlot(unsigned int SlotIndex = 0);

	void SetBaseColorTexture(unsigned int SlotIndex, const char* FilePath);
	void SetNormalTexture(unsigned int SlotIndex, const char* FilePath);
	void SetARMTexture(unsigned int SlotIndex, const char* FilePath);

	// ---- 境界 (UStaticMeshComponent::CalcBounds) ----
	// メッシュのローカル AABB をワールドへ変換して返す
	FBoxSphereBounds CalcBounds(const XMMATRIX& LocalToWorld) const override;

	// ---- レンダー側ミラー生成 ----
	FPrimitiveSceneProxy* CreateSceneProxy() override;
};
