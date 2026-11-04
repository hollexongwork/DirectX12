#include "Main.h"
#include "SkeletalMesh.h"

#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#include <cmath>

// ============================================================
//  FSkeletalMeshImportVertex
// ============================================================

void FSkeletalMeshImportVertex::AddInfluence(int BoneIndex, float Weight)
{
	if (BoneIndex < 0 || Weight <= 0.0f) return;

	// 同じボーンがあれば加算
	for (unsigned int i = 0; i < MAX_TOTAL_INFLUENCES; ++i)
	{
		if (BoneIndices[i] == BoneIndex)
		{
			BoneWeights[i] += Weight;
			return;
		}
	}

	// 空きがあれば追加
	for (unsigned int i = 0; i < MAX_TOTAL_INFLUENCES; ++i)
	{
		if (BoneIndices[i] == INDEX_NONE)
		{
			BoneIndices[i] = BoneIndex;
			BoneWeights[i] = Weight;
			return;
		}
	}

	// 満杯: 最小の重みより大きければ入れ替える
	unsigned int minSlot = 0;
	for (unsigned int i = 1; i < MAX_TOTAL_INFLUENCES; ++i)
	{
		if (BoneWeights[i] < BoneWeights[minSlot]) minSlot = i;
	}
	if (Weight > BoneWeights[minSlot])
	{
		BoneIndices[minSlot] = BoneIndex;
		BoneWeights[minSlot] = Weight;
	}
}

void FSkeletalMeshImportVertex::NormalizeInfluences()
{
	// 重みの大きい順に並べる (UE の FSkinWeightInfo と同じ。先頭が最大)
	for (unsigned int i = 0; i < MAX_TOTAL_INFLUENCES; ++i)
	{
		for (unsigned int j = i + 1; j < MAX_TOTAL_INFLUENCES; ++j)
		{
			if (BoneWeights[j] > BoneWeights[i])
			{
				std::swap(BoneWeights[i], BoneWeights[j]);
				std::swap(BoneIndices[i], BoneIndices[j]);
			}
		}
	}

	float total = 0.0f;
	for (unsigned int i = 0; i < MAX_TOTAL_INFLUENCES; ++i)
	{
		if (BoneIndices[i] == INDEX_NONE) BoneWeights[i] = 0.0f;
		total += BoneWeights[i];
	}

	if (total <= 1.0e-6f)
	{
		// 影響無しの頂点はルートボーンに固定する
		BoneIndices[0] = 0;
		BoneWeights[0] = 1.0f;
		for (unsigned int i = 1; i < MAX_TOTAL_INFLUENCES; ++i)
		{
			BoneIndices[i] = INDEX_NONE;
			BoneWeights[i] = 0.0f;
		}
		return;
	}

	const float inv = 1.0f / total;
	for (unsigned int i = 0; i < MAX_TOTAL_INFLUENCES; ++i)
	{
		BoneWeights[i] *= inv;
	}
}

unsigned int FSkeletalMeshImportData::GetNumMaterials() const
{
	unsigned int num = 0;
	for (const FSkeletalMeshImportSection& section : Sections)
	{
		num = (std::max)(num, section.MaterialIndex + 1);
	}
	return num;
}

// ============================================================
//  USkeletalMesh::Build
//  FSkeletalMeshBuilder 相当: セクションごとに頂点を複製して
//  BoneMap (ローカルボーン添字) を作り、重みを R8 へ量子化して
//  GPU バッファ化する。
// ============================================================

namespace
{
	// 重みを 0..255 に量子化し、合計が 255 になるよう最大の重みで調整する
	void QuantizeWeights(const float* Weights, const int* Indices, uint8_t* OutWeights)
	{
		int total = 0;
		int maxSlot = 0;
		for (unsigned int i = 0; i < MAX_TOTAL_INFLUENCES; ++i)
		{
			const float w = (Indices[i] == INDEX_NONE) ? 0.0f : Weights[i];
			int q = static_cast<int>(std::lround(w * 255.0f));
			q = (std::max)(0, (std::min)(255, q));
			OutWeights[i] = static_cast<uint8_t>(q);
			total += q;
			if (OutWeights[i] > OutWeights[maxSlot]) maxSlot = static_cast<int>(i);
		}
		const int delta = 255 - total;
		const int fixed = (std::max)(0, (std::min)(255, static_cast<int>(OutWeights[maxSlot]) + delta));
		OutWeights[maxSlot] = static_cast<uint8_t>(fixed);
	}
}

bool USkeletalMesh::Build(const std::string& Name, const FSkeletalMeshImportData& Data, std::shared_ptr<USkeleton> Skeleton)
{
	m_Name = Name;
	m_bLoaded = false;
	m_RenderData.LODRenderData.clear();
	m_RefBasesInvMatrix.clear();

	if (Data.IsEmpty())
	{
		char msg[256];
		sprintf_s(msg, "[USkeletalMesh] Build: empty import data (%s)\n", Name.c_str());
		OutputDebugStringA(msg);
		return false;
	}

	// ---- スケルトン ----
	if (Skeleton)
	{
		if (!Skeleton->IsCompatible(Data.RefSkeleton))
		{
			char msg[256];
			sprintf_s(msg, "[USkeletalMesh] Build: skeleton '%s' is not compatible with the mesh bones (%s)\n",
				Skeleton->GetName().c_str(), Name.c_str());
			OutputDebugStringA(msg);
			assert(false && "USkeletalMesh::Build: incompatible skeleton");
			return false;
		}
		m_Skeleton = std::move(Skeleton);
	}
	else
	{
		m_Skeleton = std::make_shared<USkeleton>(Name + "_Skeleton", Data.RefSkeleton);
	}
	m_RefSkeleton = Data.RefSkeleton;

	const int numBones = m_RefSkeleton.GetNum();

	// ---- 逆バインド行列 (RefBasesInvMatrix) ----
	std::vector<FTransform> componentSpaceRefPose;
	m_RefSkeleton.ComputeComponentSpaceRefPose(componentSpaceRefPose);

	m_RefBasesInvMatrix.resize(numBones);
	if (Data.RefBasesInvMatrix.size() == (size_t)numBones)
	{
		m_RefBasesInvMatrix = Data.RefBasesInvMatrix;
	}
	else
	{
		for (int i = 0; i < numBones; ++i)
		{
			const XMMATRIX refPose = componentSpaceRefPose[i].ToMatrixWithScale();
			XMStoreFloat4x4(&m_RefBasesInvMatrix[i], XMMatrixInverse(nullptr, refPose));
		}
	}

	// ---- 頂点の検証 / 正規化 ----
	std::vector<FSkeletalMeshImportVertex> vertices = Data.Vertices;
	for (FSkeletalMeshImportVertex& v : vertices)
	{
		for (unsigned int i = 0; i < MAX_TOTAL_INFLUENCES; ++i)
		{
			if (v.BoneIndices[i] != INDEX_NONE && !m_RefSkeleton.IsValidIndex(v.BoneIndices[i]))
			{
				v.BoneIndices[i] = INDEX_NONE;
				v.BoneWeights[i] = 0.0f;
			}
		}
		v.NormalizeInfluences();
	}

	const unsigned int numSourceVertices = static_cast<unsigned int>(vertices.size());
	for (unsigned int index : Data.Indices)
	{
		if (index >= numSourceVertices)
		{
			char msg[256];
			sprintf_s(msg, "[USkeletalMesh] Build: index out of range (%s)\n", Name.c_str());
			OutputDebugStringA(msg);
			assert(false && "USkeletalMesh::Build: index out of range");
			return false;
		}
	}

	// ---- セクション分割 (チャンク化) ----
	// セクションの三角形を順に見て、使用ボーンが MAX_GPUSKIN_BONES_PER_SECTION を
	// 越えたら新しいレンダーセクションを始める。頂点はセクションごとに複製する
	// (同じ入力頂点でも BoneMap が違えば別の GPU 頂点になる)。
	FSkeletalMeshLODRenderData lod;
	std::vector<VERTEX_SKINNED> gpuVertices;
	std::vector<unsigned int>   gpuIndices;

	for (const FSkeletalMeshImportSection& importSection : Data.Sections)
	{
		if (importSection.NumTriangles == 0) continue;
		if (importSection.FirstIndex + importSection.NumTriangles * 3 > Data.Indices.size())
		{
			assert(false && "USkeletalMesh::Build: section range out of indices");
			continue;
		}

		FSkelMeshRenderSection* section = nullptr;
		std::unordered_map<int, unsigned short> boneToLocal;		// スケルトン添字 -> ローカル添字
		std::unordered_map<unsigned int, unsigned int> vertexRemap;	// 入力頂点 -> GPU 頂点

		auto BeginSection = [&]()
			{
				lod.RenderSections.emplace_back();
				section = &lod.RenderSections.back();
				section->MaterialIndex = importSection.MaterialIndex;
				section->BaseIndex = static_cast<unsigned int>(gpuIndices.size());
				section->BaseVertexIndex = static_cast<unsigned int>(gpuVertices.size());
				section->NumTriangles = 0;
				section->NumVertices = 0;
				section->BoneMap.clear();
				boneToLocal.clear();
				vertexRemap.clear();
			};
		BeginSection();

		for (unsigned int tri = 0; tri < importSection.NumTriangles; ++tri)
		{
			const unsigned int* triIndices = &Data.Indices[importSection.FirstIndex + tri * 3];

			// この三角形が新たに必要とするボーン数
			std::unordered_set<int> newBones;
			for (unsigned int k = 0; k < 3; ++k)
			{
				const FSkeletalMeshImportVertex& v = vertices[triIndices[k]];
				for (unsigned int i = 0; i < MAX_TOTAL_INFLUENCES; ++i)
				{
					const int bone = v.BoneIndices[i];
					if (bone != INDEX_NONE && boneToLocal.find(bone) == boneToLocal.end())
					{
						newBones.insert(bone);
					}
				}
			}
			if (boneToLocal.size() + newBones.size() > MAX_GPUSKIN_BONES_PER_SECTION && !boneToLocal.empty())
			{
				BeginSection();
			}

			for (unsigned int k = 0; k < 3; ++k)
			{
				const unsigned int sourceIndex = triIndices[k];
				auto it = vertexRemap.find(sourceIndex);
				if (it == vertexRemap.end())
				{
					const FSkeletalMeshImportVertex& v = vertices[sourceIndex];

					VERTEX_SKINNED gv{};
					gv.Position = v.Position;
					gv.Normal = v.Normal;
					gv.Tangent = v.Tangent;
					gv.TexCoord = v.TexCoord;
					gv.Color = v.Color;

					for (unsigned int i = 0; i < MAX_TOTAL_INFLUENCES; ++i)
					{
						const int bone = v.BoneIndices[i];
						if (bone == INDEX_NONE)
						{
							gv.BlendIndices[i] = 0;
							continue;
						}
						auto boneIt = boneToLocal.find(bone);
						if (boneIt == boneToLocal.end())
						{
							const unsigned short local = static_cast<unsigned short>(section->BoneMap.size());
							section->BoneMap.push_back(static_cast<unsigned short>(bone));
							boneIt = boneToLocal.emplace(bone, local).first;
						}
						gv.BlendIndices[i] = static_cast<uint8_t>(boneIt->second);
					}
					QuantizeWeights(v.BoneWeights, v.BoneIndices, gv.BlendWeights);

					const unsigned int gpuIndex = static_cast<unsigned int>(gpuVertices.size());
					gpuVertices.push_back(gv);
					it = vertexRemap.emplace(sourceIndex, gpuIndex).first;
					section->NumVertices++;
				}
				// インデックスは頂点バッファ全体の添字 (DrawIndexedInstanced の BaseVertexLocation = 0)
				gpuIndices.push_back(it->second);
			}
			section->NumTriangles++;
		}

		// 空のセクション (最後の BeginSection が三角形を持たない) は捨てる
		if (section->NumTriangles == 0)
		{
			lod.RenderSections.pop_back();
		}
	}

	if (lod.RenderSections.empty() || gpuVertices.empty() || gpuIndices.empty())
	{
		char msg[256];
		sprintf_s(msg, "[USkeletalMesh] Build: no renderable section (%s)\n", Name.c_str());
		OutputDebugStringA(msg);
		return false;
	}

	// ---- GPU バッファ ----
	RenderManager* rm = RenderManager::GetInstance();
	lod.NumVertices = static_cast<unsigned int>(gpuVertices.size());
	lod.NumIndices = static_cast<unsigned int>(gpuIndices.size());

	lod.VertexBuffer = rm->CreateVertexBuffer(sizeof(VERTEX_SKINNED), lod.NumVertices);
	{
		VERTEX_SKINNED* dst = nullptr;
		HRESULT hr = lod.VertexBuffer->Resource->Map(0, nullptr, reinterpret_cast<void**>(&dst));
		assert(SUCCEEDED(hr));
		memcpy(dst, gpuVertices.data(), sizeof(VERTEX_SKINNED) * lod.NumVertices);
		lod.VertexBuffer->Resource->Unmap(0, nullptr);
		lod.VertexBuffer->Resource->SetName(L"SkeletalMeshVertexBuffer");
	}

	lod.IndexBuffer = rm->CreateIndexBuffer(lod.NumIndices);
	{
		unsigned int* dst = nullptr;
		HRESULT hr = lod.IndexBuffer->Resource->Map(0, nullptr, reinterpret_cast<void**>(&dst));
		assert(SUCCEEDED(hr));
		memcpy(dst, gpuIndices.data(), sizeof(unsigned int) * lod.NumIndices);
		lod.IndexBuffer->Resource->Unmap(0, nullptr);
		lod.IndexBuffer->Resource->SetName(L"SkeletalMeshIndexBuffer");
	}

	m_RenderData.LODRenderData.push_back(std::move(lod));

	// ---- マテリアル数 / 境界 ----
	m_NumMaterials = (std::max)(1u, Data.GetNumMaterials());
	CalculateBounds(Data, componentSpaceRefPose);

	m_bLoaded = true;

	char msg[256];
	sprintf_s(msg, "[USkeletalMesh] built: %s (%d bones, %u sections, %u verts, %u tris)\n",
		Name.c_str(), numBones, static_cast<unsigned int>(m_RenderData.LODRenderData[0].RenderSections.size()),
		m_RenderData.LODRenderData[0].NumVertices, m_RenderData.LODRenderData[0].NumIndices / 3);
	OutputDebugStringA(msg);
	return true;
}

// ============================================================
//  CalculateBounds
//  リファレンスポーズで CPU スキンした頂点からローカル AABB を求める
//  (頂点が「メッシュ空間」(FBX のメッシュノード空間) にあっても、
//   RefBasesInvMatrix * コンポーネント空間リファレンスポーズ で
//   コンポーネント空間へ写るので、描画と同じ空間の境界になる)
// ============================================================
void USkeletalMesh::CalculateBounds(const FSkeletalMeshImportData& Data, const std::vector<FTransform>& ComponentSpaceRefPose)
{
	const int numBones = m_RefSkeleton.GetNum();
	std::vector<XMMATRIX> refToLocal(numBones);
	for (int i = 0; i < numBones; ++i)
	{
		refToLocal[i] = XMLoadFloat4x4(&m_RefBasesInvMatrix[i]) * ComponentSpaceRefPose[i].ToMatrixWithScale();
	}

	XMFLOAT3 boundsMin = { 1.0e30f, 1.0e30f, 1.0e30f };
	XMFLOAT3 boundsMax = { -1.0e30f, -1.0e30f, -1.0e30f };
	bool bAny = false;
	float maxBoneRadiusSq = 0.0f;

	for (const FSkeletalMeshImportVertex& v : Data.Vertices)
	{
		FSkeletalMeshImportVertex n = v;
		n.NormalizeInfluences();

		const XMVECTOR p = XMVectorSet(v.Position.x, v.Position.y, v.Position.z, 1.0f);
		XMVECTOR skinned = XMVectorZero();
		for (unsigned int i = 0; i < MAX_TOTAL_INFLUENCES; ++i)
		{
			const int bone = n.BoneIndices[i];
			if (bone == INDEX_NONE || n.BoneWeights[i] <= 0.0f || bone >= numBones) continue;
			skinned = XMVectorAdd(skinned, XMVectorScale(XMVector4Transform(p, refToLocal[bone]), n.BoneWeights[i]));
		}

		// 影響ボーンのうち最も近いものまでの距離 (境界の拡張半径)
		float nearestSq = 1.0e30f;
		for (unsigned int i = 0; i < MAX_TOTAL_INFLUENCES; ++i)
		{
			const int bone = n.BoneIndices[i];
			if (bone == INDEX_NONE || n.BoneWeights[i] <= 0.0f || bone >= numBones) continue;
			const XMVECTOR d = XMVectorSubtract(skinned, ComponentSpaceRefPose[bone].GetTranslation());
			nearestSq = fminf(nearestSq, XMVectorGetX(XMVector3LengthSq(d)));
		}
		if (nearestSq < 1.0e30f)
		{
			maxBoneRadiusSq = fmaxf(maxBoneRadiusSq, nearestSq);
		}

		XMFLOAT3 s;
		XMStoreFloat3(&s, skinned);
		boundsMin.x = fminf(boundsMin.x, s.x); boundsMin.y = fminf(boundsMin.y, s.y); boundsMin.z = fminf(boundsMin.z, s.z);
		boundsMax.x = fmaxf(boundsMax.x, s.x); boundsMax.y = fmaxf(boundsMax.y, s.y); boundsMax.z = fmaxf(boundsMax.z, s.z);
		bAny = true;
	}

	m_Bounds = bAny ? FBoxSphereBounds::FromMinMax(boundsMin, boundsMax) : FBoxSphereBounds{};
	m_MaxBoneInfluenceRadius = sqrtf(maxBoneRadiusSq);
}

// ============================================================
//  DrawSection
// ============================================================
void USkeletalMesh::DrawSection(RenderManager* RHI, unsigned int SectionIndex) const
{
	if (!m_bLoaded) return;
	const FSkeletalMeshLODRenderData* lod = GetLODRenderData(0);
	if (lod == nullptr || SectionIndex >= lod->RenderSections.size()) return;

	const FSkelMeshRenderSection& section = lod->RenderSections[SectionIndex];

	RHI->SetVertexBuffer(lod->VertexBuffer.get());
	RHI->SetIndexBuffer(lod->IndexBuffer.get());

	ID3D12GraphicsCommandList* cmdList = RHI->GetGraphicsCommandList();
	cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	cmdList->DrawIndexedInstanced(section.NumTriangles * 3, 1, section.BaseIndex, 0, 0);
}
