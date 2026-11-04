#pragma once
#include <string>
#include <vector>
#include <memory>
#include "RenderManager.h"
#include "BoxSphereBounds.h"
#include "ReferenceSkeleton.h"
#include "Skeleton.h"

// ============================================================
//  USkeletalMesh
//  USkeletalMesh (Engine/SkeletalMesh.h) + FSkeletalMeshRenderData
//  (SkeletalMeshRenderData.h) に相当。
//    - Skeleton         : 共有する USkeleton (アニメーションとの互換判定)
//    - RefSkeleton      : メッシュ側のリファレンススケルトン (USkeleton のコピー)
//    - RefBasesInvMatrix: ボーンごとの逆バインド行列 (コンポーネント空間 -> ボーン空間)。
//                         ReferenceToLocal[i] = RefBasesInvMatrix[i] * ComponentSpaceTransforms[i]
//    - RenderData       : LOD 0 のみ。セクション (FSkelMeshRenderSection, BoneMap 付き) と
//                         1 本の VERTEX_SKINNED 頂点バッファ / R32 インデックスバッファ
//    - Bounds           : リファレンスポーズでの CPU スキン結果のローカル AABB
//                         (USkeletalMesh::GetBounds。USkinnedMeshComponent::CalcBounds が使う)
//
//  構築は FSkeletalMeshImportData (FBX インポート / プロシージャル生成が作る CPU 側データ) から
//  Build() で行う (FSkeletalMeshBuilder 相当)。セクションごとに使用ボーンを BoneMap へ圧縮し、
//  1 セクションが 256 ボーンを越える場合は三角形を分割する (UE のチャンク分割)。
//  描画は FSkeletalMeshSceneProxy (SkeletalMeshComponent.cpp) が DrawSection で行う。
//  スケルタルメッシュは Mesh Distance Field / Lumen Surface Cache / BLAS を持たない
//  (UE と同じ。スクリーントレース以外の GI には参加しない)。
// ============================================================

// 1 頂点あたりの最大ボーン影響数 (MAX_INFLUENCES_PER_STREAM)
constexpr unsigned int MAX_TOTAL_INFLUENCES = 4;
// 1 セクション (チャンク) が参照できるボーン数の上限 (BLENDINDICES が R8 のため)
constexpr unsigned int MAX_GPUSKIN_BONES_PER_SECTION = 256;

// ------------------------------------------------------------
//  FSkeletalMeshImportData
//  CPU 側の構築入力 (FSkeletalMeshImportData の最小形)。
//  頂点はスケルトンのボーン添字で影響を持ち、インデックスは
//  全体で 1 本、セクションは三角形範囲 + MaterialIndex。
// ------------------------------------------------------------
struct FSkeletalMeshImportVertex
{
	XMFLOAT3 Position = { 0.0f, 0.0f, 0.0f };
	XMFLOAT3 Normal = { 0.0f, 1.0f, 0.0f };
	XMFLOAT3 Tangent = { 1.0f, 0.0f, 0.0f };
	XMFLOAT2 TexCoord = { 0.0f, 0.0f };
	XMFLOAT4 Color = { 1.0f, 1.0f, 1.0f, 1.0f };

	// ボーン影響 (スケルトンのボーン添字 / 重み)。未使用は BoneIndex = INDEX_NONE, Weight = 0
	int   BoneIndices[MAX_TOTAL_INFLUENCES] = { INDEX_NONE, INDEX_NONE, INDEX_NONE, INDEX_NONE };
	float BoneWeights[MAX_TOTAL_INFLUENCES] = { 0.0f, 0.0f, 0.0f, 0.0f };

	// 影響を追加する (満杯なら最小の重みと入れ替える)
	void AddInfluence(int BoneIndex, float Weight);
	// 重みの合計を 1 にそろえ、重みの大きい順に並べる (影響無しなら BoneIndex 0 / 重み 1)
	void NormalizeInfluences();
};

struct FSkeletalMeshImportSection
{
	unsigned int MaterialIndex = 0;
	unsigned int FirstIndex = 0;		// Indices 内の先頭 (3 の倍数)
	unsigned int NumTriangles = 0;
};

struct FSkeletalMeshImportData
{
	std::vector<FSkeletalMeshImportVertex>  Vertices;
	std::vector<unsigned int>               Indices;
	std::vector<FSkeletalMeshImportSection> Sections;
	FReferenceSkeleton                      RefSkeleton;

	// 逆バインド行列 (コンポーネント空間 -> ボーン空間、行ベクトル規約)。
	// 空ならリファレンスポーズのコンポーネント空間変換の逆から求める
	// (FBX の aiBone::mOffsetMatrix を持つときはインポータが埋める)
	std::vector<XMFLOAT4X4> RefBasesInvMatrix;

	bool IsEmpty() const { return Vertices.empty() || Indices.size() < 3 || RefSkeleton.GetNum() == 0; }
	unsigned int GetNumMaterials() const;
};

// ------------------------------------------------------------
//  FSkelMeshRenderSection / FSkeletalMeshLODRenderData / FSkeletalMeshRenderData
// ------------------------------------------------------------
struct FSkelMeshRenderSection
{
	unsigned int MaterialIndex = 0;
	unsigned int BaseIndex = 0;			// インデックスバッファ内の先頭
	unsigned int NumTriangles = 0;
	unsigned int BaseVertexIndex = 0;	// 頂点バッファ内の先頭 (このセクションの頂点は連続)
	unsigned int NumVertices = 0;

	// ローカルボーン添字 (頂点の BLENDINDICES) -> スケルトンのボーン添字
	std::vector<unsigned short> BoneMap;
};

struct FSkeletalMeshLODRenderData
{
	std::vector<FSkelMeshRenderSection> RenderSections;
	std::unique_ptr<VERTEX_BUFFER>      VertexBuffer;	// VERTEX_SKINNED
	std::unique_ptr<INDEX_BUFFER>       IndexBuffer;	// R32_UINT
	unsigned int                        NumVertices = 0;
	unsigned int                        NumIndices = 0;
};

struct FSkeletalMeshRenderData
{
	std::vector<FSkeletalMeshLODRenderData> LODRenderData;	// 本実装は LOD 0 のみ
};

class USkeletalMesh
{
private:
	std::string                m_Name;
	std::shared_ptr<USkeleton> m_Skeleton;
	FReferenceSkeleton         m_RefSkeleton;
	std::vector<XMFLOAT4X4>    m_RefBasesInvMatrix;	// 行ベクトル規約 (転置前)
	FSkeletalMeshRenderData    m_RenderData;
	FBoxSphereBounds           m_Bounds;				// ローカル (コンポーネント) 空間
	// 各頂点から「その頂点に影響するボーンのうち最も近いもの」までの距離の最大値 [m]
	// (リファレンスポーズ)。USkinnedMeshComponent::CalcBounds が現在のボーン位置 AABB を
	// これで広げてポーズに追従する境界を作る
	float                      m_MaxBoneInfluenceRadius = 0.0f;
	unsigned int               m_NumMaterials = 0;
	bool                       m_bLoaded = false;

	// リファレンスポーズでの CPU スキンからローカル境界を求める
	void CalculateBounds(const FSkeletalMeshImportData& Data, const std::vector<FTransform>& ComponentSpaceRefPose);

public:
	USkeletalMesh() = default;
	~USkeletalMesh() = default;

	USkeletalMesh(const USkeletalMesh&) = delete;
	USkeletalMesh& operator=(const USkeletalMesh&) = delete;

	// インポートデータから構築する。Skeleton が null なら Data.RefSkeleton から新しい
	// USkeleton を作る。Skeleton を渡す場合は Data.RefSkeleton と同じボーン列であること
	bool Build(const std::string& Name, const FSkeletalMeshImportData& Data,
		std::shared_ptr<USkeleton> Skeleton = nullptr);

	// ---- 描画 (FSkeletalMeshSceneProxy から) ----
	// 呼び出し側が PSO / 定数 / テクスチャ / ボーン行列 (t39 / t40) をバインド済みであること。
	// VB / IB / トポロジを設定してセクションの三角形を描く
	void DrawSection(RenderManager* RHI, unsigned int SectionIndex) const;

	// ---- アクセサ ----
	bool IsLoaded() const { return m_bLoaded; }
	const std::string& GetName() const { return m_Name; }

	const std::shared_ptr<USkeleton>& GetSkeleton() const { return m_Skeleton; }
	const FReferenceSkeleton& GetRefSkeleton() const { return m_RefSkeleton; }
	const std::vector<XMFLOAT4X4>& GetRefBasesInvMatrix() const { return m_RefBasesInvMatrix; }
	int GetNumBones() const { return m_RefSkeleton.GetNum(); }

	const FSkeletalMeshRenderData& GetRenderData() const { return m_RenderData; }
	const FSkeletalMeshLODRenderData* GetLODRenderData(unsigned int LODIndex = 0) const
	{
		return (LODIndex < m_RenderData.LODRenderData.size()) ? &m_RenderData.LODRenderData[LODIndex] : nullptr;
	}
	unsigned int GetNumSections(unsigned int LODIndex = 0) const
	{
		const FSkeletalMeshLODRenderData* lod = GetLODRenderData(LODIndex);
		return lod ? static_cast<unsigned int>(lod->RenderSections.size()) : 0;
	}
	unsigned int GetSectionMaterialIndex(unsigned int SectionIndex, unsigned int LODIndex = 0) const
	{
		const FSkeletalMeshLODRenderData* lod = GetLODRenderData(LODIndex);
		return (lod && SectionIndex < lod->RenderSections.size()) ? lod->RenderSections[SectionIndex].MaterialIndex : 0;
	}

	// マテリアルスロット数 (USkeletalMesh::GetMaterials().Num())
	unsigned int GetNumMaterials() const { return m_NumMaterials; }

	// ローカル境界 (USkeletalMesh::GetBounds)
	const FBoxSphereBounds& GetBounds() const { return m_Bounds; }
	float GetMaxBoneInfluenceRadius() const { return m_MaxBoneInfluenceRadius; }
};
