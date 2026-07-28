#pragma once

#include "assimp/Importer.hpp"
#include "assimp/cimport.h"
#include "assimp/scene.h"
#include "assimp/postprocess.h"
#include "assimp/matrix4x4.h"
#pragma comment (lib, "assimp-vc143-mt.lib")

#include <string>
#include <vector>
#include <memory>

#include "DistanceFieldAtlas.h"
#include "BoxSphereBounds.h"


struct FBX_SUBSET
{
	std::unique_ptr<VERTEX_BUFFER>	VertexBuffer;
	std::unique_ptr<INDEX_BUFFER>	IndexBuffer;
	unsigned int					IndexCount = 0;
	unsigned int					MaterialIndex = 0;
};


class FBXModel
{
public:
	FBXModel() = default;
	~FBXModel() = default;

	FBXModel(const FBXModel&) = delete;
	FBXModel& operator=(const FBXModel&) = delete;

	bool Load(const char* filePath, bool flipUV = true);

	void Draw();
	void DrawSubset(unsigned int index);

	unsigned int GetSubsetCount() const { return static_cast<unsigned int>(m_Subsets.size()); }
	unsigned int GetMaterialIndex(unsigned int index) const { return m_Subsets[index].MaterialIndex; }

	bool IsLoaded() const { return m_Loaded; }

	// Mesh Distance Field (Load 時に FDistanceFieldAtlas へ登録される)
	const FDistanceFieldMeshInfo& GetDistanceField() const { return m_DistanceField; }

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

private:
	void ProcessNode(const aiNode* node, const aiScene* scene,
		const std::string& directory);

	FBX_SUBSET ProcessMesh(const aiMesh* mesh, const aiScene* scene,
		const std::string& directory);

	std::unique_ptr<TEXTURE> LoadTextureFromMaterial(
		const aiMaterial* mat, aiTextureType type,
		const std::string& directory);

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
};
