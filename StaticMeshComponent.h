#pragma once
#include "RenderManager.h"
#include "FBXModel.h"
#include "Material.h"
#include "PrimitiveComponent.h"

// ============================================================
//  UStaticMeshComponent
//  UStaticMeshComponent に相当。FBX メッシュと
//  マテリアルスロット (サブセットの MaterialIndex に対応) を持つ。
//  メッシュ / テクスチャは FAssetManager のキャッシュと共有する
//  shared_ptr (UStaticMesh / UTexture2D アセット共有に相当) で、
//  コンポーネントごとの GPU リソース重複生成はしない。
//  描画本体は FStaticMeshSceneProxy (StaticMeshComponent.cpp 内)
//  へ移設済み。マテリアル / メッシュ / テクスチャの変更は
//  MarkRenderStateDirty 経由で次フレームのプロキシ再生成に反映される。
// ============================================================

struct FMaterialSlot
{
	Material                 Mat;

	// テクスチャは FAssetManager のキャッシュと共有する
	// (UTexture2D 相当。プロキシも同じ shared_ptr を保持するため、
	//  実行時差し替え後も旧テクスチャは旧プロキシが生かし続ける)
	std::shared_ptr<TEXTURE> BaseColor;
	std::shared_ptr<TEXTURE> Normal;
	std::shared_ptr<TEXTURE> ARM;
};

class UStaticMeshComponent : public UPrimitiveComponent
{
protected:
	// FAssetManager 経由の共有メッシュアセット (UStaticMesh 相当)。
	// 未設定時は nullptr。
	std::shared_ptr<FBXModel>  m_Mesh;
	std::vector<FMaterialSlot> m_MaterialSlots;

public:
	UStaticMeshComponent();

	// ---- メッシュ ----
	// FAssetManager のキャッシュから共有メッシュを取得して差し替える
	bool SetStaticMesh(const char* FilePath, bool FlipUV = true);
	const std::shared_ptr<FBXModel>& GetStaticMesh() const { return m_Mesh; }

	// ---- マテリアルスロット ----
	void         SetNumMaterialSlots(unsigned int Num);
	unsigned int GetNumMaterialSlots() const { return static_cast<unsigned int>(m_MaterialSlots.size()); }

	// 直接書き換えた場合は MarkRenderStateDirty() を呼ぶこと
	Material& GetMaterial(unsigned int SlotIndex = 0);
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
