#pragma once

#include "assimp/Importer.hpp"
#include "assimp/scene.h"
#include "assimp/postprocess.h"
#pragma comment (lib, "assimp-vc143-mt.lib")

#include <string>
#include <vector>
#include <memory>

#include "DistanceFieldAtlas.h"
#include "BoxSphereBounds.h"

struct FProceduralMeshData;


struct FBX_SUBSET
{
	std::unique_ptr<VERTEX_BUFFER>	VertexBuffer;
	std::unique_ptr<INDEX_BUFFER>	IndexBuffer;
	unsigned int					IndexCount = 0;
	unsigned int					MaterialIndex = 0;
};


class FBXModel
{
private:
	// DXR BLAS 構築 (Load 末尾から呼ばれる。非対応環境では即リターン)
	void BuildRayTracingGeometry();

	void ProcessNode(const aiNode* node, const aiScene* scene,
		const std::string& directory);

	FBX_SUBSET ProcessMesh(const aiMesh* mesh, const aiScene* scene,
		const std::string& directory);

	std::unique_ptr<TEXTURE> LoadTextureFromMaterial(
		const aiMaterial* mat, aiTextureType type,
		const std::string& directory);

	// CPU 側の頂点 / インデックス列から 1 サブセット分の GPU バッファを
	// 作る共通経路 (FBX / プロシージャル両方が通る)。ローカル境界と
	// SDF ベイク用三角形 (bAffectDistanceField のときだけ) もここで蓄積する
	FBX_SUBSET CreateSubset(const VERTEX_3D* Vertices, unsigned int VertexCount,
		const unsigned int* Indices, unsigned int IndexCount,
		unsigned int MaterialIndex, bool bAffectDistanceField);

	// ロード状態 / 境界 / DF の初期化と、サブセット構築後の共通後処理
	// (SDF ベイク + BLAS 構築 + Loaded フラグ)。
	// DistanceFieldCachePath は <path>.mdf のキャッシュ元 (null / 空で非キャッシュ)
	void ResetLoadState();
	void FinalizeLoad(const char* DistanceFieldCachePath);

	std::vector<FBX_SUBSET> m_Subsets;
	bool					m_Loaded = false;

	// ---- Mesh Distance Field ----
	// ロード中に三角形頂点を蓄積し、Load 末尾で SDF ベイクへ渡す
	// (登録後は解放される)。
	std::vector<XMFLOAT3>  m_DFTriangles;
	FDistanceFieldMeshInfo m_DistanceField;

	// ---- ローカル境界 ----
	// ProcessMesh の頂点ループで min/max を蓄積する
	XMFLOAT3 m_LocalBoundsMin = { 0.0f, 0.0f, 0.0f };
	XMFLOAT3 m_LocalBoundsMax = { 0.0f, 0.0f, 0.0f };
	bool     m_bHasLocalBounds = false;

	// ---- DXR BLAS ----
	ComPtr<ID3D12Resource> m_BLAS;

public:
	FBXModel() = default;
	// SDF のアトラススロットを返却する (プロシージャルメッシュの再生成で
	// スロットを使い回せるように)。GPU バッファは RenderManager の遅延削除へ
	~FBXModel();

	FBXModel(const FBXModel&) = delete;
	FBXModel& operator=(const FBXModel&) = delete;

	bool Load(const char* filePath, bool flipUV = true);

	// プロシージャル生成したメッシュ (ProceduralMeshGenerator.h) から
	// 構築する。各セクションが 1 サブセット (MaterialIndex はセクションの値)。
	// Name はデバッグ表示と SDF キャッシュ名
	// (Saved/ProceduralMesh/<Name>_<ジオメトリハッシュ>.mdf) に使う。
	// FBX と同じく SDF ベイク / BLAS 構築 / ローカル境界の算出を行う。
	bool BuildFromProceduralMesh(const char* Name, const FProceduralMeshData& Data);

	void Draw();
	void DrawSubset(unsigned int index);

	unsigned int GetSubsetCount() const { return static_cast<unsigned int>(m_Subsets.size()); }
	unsigned int GetMaterialIndex(unsigned int index) const { return m_Subsets[index].MaterialIndex; }

	bool IsLoaded() const { return m_Loaded; }

	// Mesh Distance Field (Load 時に FDistanceFieldAtlas へ登録される)
	const FDistanceFieldMeshInfo& GetDistanceField() const { return m_DistanceField; }

	// ---- DXR BLAS (Bottom Level Acceleration Structure) ----
	// Load 末尾で一度だけ構築される (DXR 非対応環境では何もしない)。
	// Lumen の HWRT トレースが TLAS のインスタンスとして参照する
	// (LumenHardwareRayTracing.h)。
	bool HasBLAS() const { return m_BLAS != nullptr; }
	D3D12_GPU_VIRTUAL_ADDRESS GetBLASAddress() const
	{
		return m_BLAS ? m_BLAS->GetGPUVirtualAddress() : 0;
	}

	// ---- ローカル境界 (フラスタムカリング用) ----
	// ProcessMesh が全頂点から蓄積したローカル AABB。
	// UStaticMesh の Bounds (ExtendedBounds) に相当し、
	// UStaticMeshComponent::CalcBounds がワールドへ変換する。
	FBoxSphereBounds GetLocalBounds() const
	{
		return m_bHasLocalBounds
			? FBoxSphereBounds::FromMinMax(m_LocalBoundsMin, m_LocalBoundsMax)
			: FBoxSphereBounds{};
	}
};
