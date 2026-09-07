#include "Main.h"
#include "RenderManager.h"
#include "FBXModel.h"

#include "D3DX12.h"

#include <filesystem>
#include <cassert>

namespace fs = std::filesystem;


// ================================================================
//  DXR BLAS 構築
//  全サブセットの三角形ジオメトリから Bottom Level AS を一度だけ
//  ビルドする。頂点はアップロードヒープの VERTEX_3D (Position が
//  先頭 / ストライド 60B)、インデックスは R32_UINT。
//  ビルドは即時フラッシュ (DF ベイク / IBL と同じ初期化時パターン)
//  なのでスクラッチはこのスコープで解放できる。
// ================================================================
void FBXModel::BuildRayTracingGeometry()
{
	RenderManager* rm = RenderManager::GetInstance();
	if (rm == nullptr || !rm->IsRayTracingSupported() || m_Subsets.empty())
	{
		return;
	}

	ID3D12Device5* device5 = rm->GetDevice5();
	ID3D12GraphicsCommandList4* cl4 = rm->GetGraphicsCommandList4();
	if (device5 == nullptr || cl4 == nullptr)
	{
		return;
	}

	// ---- サブセット -> ジオメトリ記述 ----
	std::vector<D3D12_RAYTRACING_GEOMETRY_DESC> geometries;
	geometries.reserve(m_Subsets.size());

	for (const FBX_SUBSET& subset : m_Subsets)
	{
		if (!subset.VertexBuffer || !subset.IndexBuffer || subset.IndexCount == 0)
		{
			continue;
		}

		D3D12_RAYTRACING_GEOMETRY_DESC geometry{};
		geometry.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
		// マテリアルの Masked / TwoSided はトレース側では扱わない
		// (SDF 経路と同じく不透明ジオメトリとして遮蔽させる)
		geometry.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
		geometry.Triangles.Transform3x4 = 0;
		geometry.Triangles.IndexFormat = DXGI_FORMAT_R32_UINT;
		geometry.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
		geometry.Triangles.IndexCount = subset.IndexCount;
		geometry.Triangles.VertexCount = subset.VertexBuffer->Size;
		geometry.Triangles.IndexBuffer = subset.IndexBuffer->Resource->GetGPUVirtualAddress();
		geometry.Triangles.VertexBuffer.StartAddress = subset.VertexBuffer->Resource->GetGPUVirtualAddress();
		geometry.Triangles.VertexBuffer.StrideInBytes = subset.VertexBuffer->Stride;

		geometries.push_back(geometry);
	}

	if (geometries.empty())
	{
		return;
	}

	// ---- プレビルド情報 ----
	D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs{};
	inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
	inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
	inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
	inputs.NumDescs = (UINT)geometries.size();
	inputs.pGeometryDescs = geometries.data();

	D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO prebuild{};
	device5->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &prebuild);
	if (prebuild.ResultDataMaxSizeInBytes == 0)
	{
		return;
	}

	// ---- スクラッチ / 結果バッファ ----
	ComPtr<ID3D12Resource> scratch;
	HRESULT hr = rm->GetDevice()->CreateCommittedResource(
		&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
		D3D12_HEAP_FLAG_NONE,
		&CD3DX12_RESOURCE_DESC::Buffer(prebuild.ScratchDataSizeInBytes,
			D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),
		D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
		nullptr,
		IID_PPV_ARGS(&scratch));
	assert(SUCCEEDED(hr));

	hr = rm->GetDevice()->CreateCommittedResource(
		&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
		D3D12_HEAP_FLAG_NONE,
		&CD3DX12_RESOURCE_DESC::Buffer(prebuild.ResultDataMaxSizeInBytes,
			D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),
		D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,
		nullptr,
		IID_PPV_ARGS(&m_BLAS));
	assert(SUCCEEDED(hr));
	m_BLAS->SetName(L"LumenBLAS");

	// ---- ビルド + 即時フラッシュ ----
	D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build{};
	build.Inputs = inputs;
	build.ScratchAccelerationStructureData = scratch->GetGPUVirtualAddress();
	build.DestAccelerationStructureData = m_BLAS->GetGPUVirtualAddress();

	cl4->BuildRaytracingAccelerationStructure(&build, 0, nullptr);

	cl4->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::UAV(m_BLAS.Get()));

	// スクラッチをこのスコープで解放するため即時完了させる
	rm->FlushAndResetCommandList();

	m_bBLASBuilt = true;
}


// ================================================================
//  公開 API
// ================================================================

bool FBXModel::Load(const char* filePath, bool flipUV)
{
	m_Subsets.clear();
	m_DFTriangles.clear();
	m_DistanceField = FDistanceFieldMeshInfo{};
	m_Loaded = false;

	// ローカル境界のリセット (ProcessMesh の頂点ループで蓄積する)
	m_LocalBoundsMin = { 1.0e30f,  1.0e30f,  1.0e30f };
	m_LocalBoundsMax = { -1.0e30f, -1.0e30f, -1.0e30f };
	m_bHasLocalBounds = false;

	Assimp::Importer importer;

	unsigned int flags =
		aiProcess_Triangulate |   // 三角形化
		aiProcess_CalcTangentSpace |   // 接線・従法線生成
		aiProcess_GenSmoothNormals |   // 法線がないメッシュに自動生成
		aiProcess_JoinIdenticalVertices |   // 重複頂点マージ
		aiProcess_SortByPType |   // プリミティブ種別ソート
		aiProcess_ConvertToLeftHanded;      // DirectX 左手座標系へ変換

	if (flipUV)
		flags |= aiProcess_FlipUVs;

	const aiScene* scene = importer.ReadFile(filePath, flags);

	if (!scene || (scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE) || !scene->mRootNode)
	{
		char msg[512];
		sprintf_s(msg, "[FBXModel] Load failed: %s\n  file: %s\n",
			importer.GetErrorString(), filePath);
		OutputDebugStringA(msg);
		assert(false && "FBXModel::Load failed");
		return false;
	}

	std::string directory = fs::path(filePath).parent_path().string();

	ProcessNode(scene->mRootNode, scene, directory);

	// ---- Mesh Distance Field 生成 (MDF ベイク) ----
	// 蓄積した三角形からローカル境界を求め、アトラスへ登録する。
	// ベイク結果は <filePath>.mdf にキャッシュされる。
	if (!m_DFTriangles.empty())
	{
		XMFLOAT3 boundsMin{ 1.0e30f, 1.0e30f, 1.0e30f };
		XMFLOAT3 boundsMax{ -1.0e30f, -1.0e30f, -1.0e30f };
		for (const XMFLOAT3& v : m_DFTriangles)
		{
			boundsMin.x = fminf(boundsMin.x, v.x);
			boundsMin.y = fminf(boundsMin.y, v.y);
			boundsMin.z = fminf(boundsMin.z, v.z);
			boundsMax.x = fmaxf(boundsMax.x, v.x);
			boundsMax.y = fmaxf(boundsMax.y, v.y);
			boundsMax.z = fmaxf(boundsMax.z, v.z);
		}

		FDistanceFieldAtlas::Get().AddMesh(filePath, m_DFTriangles,
			boundsMin, boundsMax, m_DistanceField);
	}
	m_DFTriangles.clear();
	m_DFTriangles.shrink_to_fit();

	// ---- DXR BLAS 構築 (対応環境のみ) ----
	// Lumen の HWRT トレース (LumenHardwareRayTracing.h) が TLAS の
	// インスタンスとして参照する。頂点 / インデックスバッファは
	// アップロードヒープ (GENERIC_READ) のままビルド入力にできる。
	BuildRayTracingGeometry();

	m_Loaded = true;
	return true;
}


// ================================================================
//  描画
//  呼び出し元で PRIMITIVE_CONSTANT をセット済みであること。
//  MATERIAL_CONSTANT / テクスチャはサブセットごとにここでセット。
// ================================================================

void FBXModel::Draw()
{
	if (!m_Loaded) return;

	for (unsigned int i = 0; i < m_Subsets.size(); ++i)
		DrawSubset(i);

}

void FBXModel::DrawSubset(unsigned int index)
{
	if (!m_Loaded || index >= m_Subsets.size()) return;

	RenderManager* rm = RenderManager::GetInstance();
	const FBX_SUBSET& subset = m_Subsets[index];

	rm->SetVertexBuffer(subset.VertexBuffer.get());
	rm->SetIndexBuffer(subset.IndexBuffer.get());

	ID3D12GraphicsCommandList* cmdList = rm->GetGraphicsCommandList();
	cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	cmdList->DrawIndexedInstanced(subset.IndexCount, 1, 0, 0, 0);

}


// ================================================================
//  プライベート：ノード再帰処理
// ================================================================

void FBXModel::ProcessNode(const aiNode* node, const aiScene* scene,
	const std::string& directory)
{
	for (unsigned int i = 0; i < node->mNumMeshes; ++i)
	{
		const aiMesh* mesh = scene->mMeshes[node->mMeshes[i]];
		m_Subsets.emplace_back(ProcessMesh(mesh, scene, directory));
	}

	for (unsigned int i = 0; i < node->mNumChildren; ++i)
		ProcessNode(node->mChildren[i], scene, directory);
}


// ================================================================
//  プライベート：メッシュ → FBX_SUBSET 変換
// ================================================================

FBX_SUBSET FBXModel::ProcessMesh(const aiMesh* mesh, const aiScene* scene,
	const std::string& directory)
{
	RenderManager* rm = RenderManager::GetInstance();

	const unsigned int vertexCount = mesh->mNumVertices;
	const unsigned int indexCount = mesh->mNumFaces * 3;

	// ----------------------------------------------------------------
	// 頂点バッファ（VERTEX_3D は既存プロジェクトの構造体）
	// ----------------------------------------------------------------
	auto vb = rm->CreateVertexBuffer(sizeof(VERTEX_3D), vertexCount);

	{
		VERTEX_3D* dst = nullptr;
		HRESULT hr = vb->Resource->Map(0, nullptr, reinterpret_cast<void**>(&dst));
		assert(SUCCEEDED(hr));

		for (unsigned int i = 0; i < vertexCount; ++i)
		{
			// 位置
			dst[i].Position = { mesh->mVertices[i].x,
								mesh->mVertices[i].y,
								mesh->mVertices[i].z };

			// ローカル境界の蓄積 (フラスタムカリング用 AABB)
			m_LocalBoundsMin.x = fminf(m_LocalBoundsMin.x, dst[i].Position.x);
			m_LocalBoundsMin.y = fminf(m_LocalBoundsMin.y, dst[i].Position.y);
			m_LocalBoundsMin.z = fminf(m_LocalBoundsMin.z, dst[i].Position.z);
			m_LocalBoundsMax.x = fmaxf(m_LocalBoundsMax.x, dst[i].Position.x);
			m_LocalBoundsMax.y = fmaxf(m_LocalBoundsMax.y, dst[i].Position.y);
			m_LocalBoundsMax.z = fmaxf(m_LocalBoundsMax.z, dst[i].Position.z);
			m_bHasLocalBounds = true;

			// 法線
			dst[i].Normal = mesh->HasNormals()
				? XMFLOAT3{ mesh->mNormals[i].x,
							mesh->mNormals[i].y,
							mesh->mNormals[i].z }
			: XMFLOAT3{ 0.0f, 1.0f, 0.0f };

			// 接線（aiProcess_CalcTangentSpace で生成済み）
			dst[i].Tangent = mesh->HasTangentsAndBitangents()
				? XMFLOAT3{ mesh->mTangents[i].x,
							mesh->mTangents[i].y,
							mesh->mTangents[i].z }
			: XMFLOAT3{ 1.0f, 0.0f, 0.0f };

			// UV（チャンネル 0）
			dst[i].TexCoord = mesh->HasTextureCoords(0)
				? XMFLOAT2{ mesh->mTextureCoords[0][i].x,
							mesh->mTextureCoords[0][i].y }
			: XMFLOAT2{ 0.0f, 0.0f };

			// 頂点カラー（チャンネル 0、なければ白）
			dst[i].Color = mesh->HasVertexColors(0)
				? XMFLOAT4{ mesh->mColors[0][i].r,
							mesh->mColors[0][i].g,
							mesh->mColors[0][i].b,
							mesh->mColors[0][i].a }
			: XMFLOAT4{ 1.0f, 1.0f, 1.0f, 1.0f };
		}

		vb->Resource->Unmap(0, nullptr);
	}

	// ----------------------------------------------------------------
	// インデックスバッファ（uint32）
	// ----------------------------------------------------------------
	auto ib = rm->CreateIndexBuffer(indexCount);

	{
		unsigned int* dst = nullptr;
		HRESULT hr = ib->Resource->Map(0, nullptr, reinterpret_cast<void**>(&dst));
		assert(SUCCEEDED(hr));

		unsigned int idx = 0;
		for (unsigned int f = 0; f < mesh->mNumFaces; ++f)
		{
			const aiFace& face = mesh->mFaces[f];
			assert(face.mNumIndices == 3);
			dst[idx++] = face.mIndices[0];
			dst[idx++] = face.mIndices[1];
			dst[idx++] = face.mIndices[2];

			// Distance Field ベイク用に三角形頂点を蓄積 (ローカル空間)
			const aiVector3D& dfA = mesh->mVertices[face.mIndices[0]];
			const aiVector3D& dfB = mesh->mVertices[face.mIndices[1]];
			const aiVector3D& dfC = mesh->mVertices[face.mIndices[2]];
			m_DFTriangles.push_back({ dfA.x, dfA.y, dfA.z });
			m_DFTriangles.push_back({ dfB.x, dfB.y, dfB.z });
			m_DFTriangles.push_back({ dfC.x, dfC.y, dfC.z });
		}

		ib->Resource->Unmap(0, nullptr);
	}

	// ----------------------------------------------------------------
	// サブセット組み立て
	// ----------------------------------------------------------------
	FBX_SUBSET subset;
	subset.VertexBuffer = std::move(vb);
	subset.IndexBuffer = std::move(ib);
	subset.IndexCount = indexCount;
	subset.MaterialIndex = mesh->mMaterialIndex;

	return subset;
}


std::unique_ptr<TEXTURE> FBXModel::LoadTextureFromMaterial(
	const aiMaterial* mat, aiTextureType type, const std::string& directory)
{
	if (mat->GetTextureCount(type) == 0) return nullptr;

	aiString aiPath;
	if (AI_SUCCESS != mat->GetTexture(type, 0, &aiPath)) return nullptr;

	fs::path fullPath = fs::path(directory) / aiPath.C_Str();
	std::string pathStr = fullPath.string();

	for (char& c : pathStr) if (c == '/') c = '\\';

	// BaseColor/Diffuse are sRGB-authored; all other maps (normal, ORM,
	// metallic-roughness, AO) are linear.
	bool sRGB = (type == aiTextureType_DIFFUSE) || (type == aiTextureType_BASE_COLOR);
	auto tex = RenderManager::GetInstance()->LoadTexture(pathStr.c_str(), sRGB);
	if (!tex)
	{
		char msg[512];
		sprintf_s(msg, "[FBXModel] Texture not found: %s\n", pathStr.c_str());
		OutputDebugStringA(msg);
	}

	return tex;
}