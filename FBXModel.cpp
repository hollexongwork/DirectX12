#include "Main.h"
#include "RenderManager.h"
#include "FBXModel.h"
#include "ProceduralMeshGenerator.h"

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

	// TLAS のインスタンスになるのは Lumen スロットを持つメッシュ =
	// 有効な SDF を持つものだけ (FLumenSceneData::UpdateTLAS)。
	// SDF 無しのメッシュに BLAS を作ってもトレースされず、
	// ビルドごとの GPU フラッシュ (ロード時間) が無駄になる。
	if (!m_DistanceField.bValid)
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
}


// ================================================================
//  公開 API
// ================================================================

FBXModel::~FBXModel()
{
	// アトラススロットを空きリストへ戻す (アトラスが無ければ何もしない)
	if (m_DistanceField.bValid)
	{
		if (FDistanceFieldAtlas* atlas = FDistanceFieldAtlas::GetIfExists())
		{
			atlas->ReleaseMesh(m_DistanceField);
		}
	}
}

void FBXModel::ResetLoadState()
{
	m_Subsets.clear();
	m_DFTriangles.clear();
	if (m_DistanceField.bValid)
	{
		if (FDistanceFieldAtlas* atlas = FDistanceFieldAtlas::GetIfExists())
		{
			atlas->ReleaseMesh(m_DistanceField);
		}
	}
	m_DistanceField = FDistanceFieldMeshInfo{};
	m_BLAS.Reset();
	m_Loaded = false;

	// ローカル境界のリセット (CreateSubset の頂点ループで蓄積する)
	m_LocalBoundsMin = { 1.0e30f,  1.0e30f,  1.0e30f };
	m_LocalBoundsMax = { -1.0e30f, -1.0e30f, -1.0e30f };
	m_bHasLocalBounds = false;
}


// ================================================================
//  FinalizeLoad
//  サブセット構築後の共通後処理。FBX ロードとプロシージャル構築の
//  両方がここを通る。
// ================================================================
void FBXModel::FinalizeLoad(const char* DistanceFieldCachePath)
{
	// ---- Mesh Distance Field 生成 (MDF ベイク) ----
	// 蓄積した三角形からローカル境界を求め、アトラスへ登録する。
	// ベイク結果は <DistanceFieldCachePath>.mdf にキャッシュされる
	// (null / 空文字なら毎回ベイク)。
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

		FDistanceFieldAtlas::Get().AddMesh(DistanceFieldCachePath, m_DFTriangles,
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
}


bool FBXModel::Load(const char* filePath, bool flipUV)
{
	ResetLoadState();

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

	// SDF ベイク (<filePath>.mdf にキャッシュ) + BLAS 構築
	FinalizeLoad(filePath);
	return true;
}


// ================================================================
//  BuildFromProceduralMesh
//  ProceduralMeshGenerator が生成した CPU メッシュから構築する。
//  FBX ロードと同じ CreateSubset / FinalizeLoad を通るため、
//  Distance Field Shadow / Lumen (SDF + BLAS) / フラスタムカリングが
//  そのまま効く。
// ================================================================
bool FBXModel::BuildFromProceduralMesh(const char* Name, const FProceduralMeshData& Data)
{
	ResetLoadState();

	if (Data.IsEmpty())
	{
		char msg[256];
		sprintf_s(msg, "[FBXModel] BuildFromProceduralMesh: empty mesh data (%s)\n", Name ? Name : "");
		OutputDebugStringA(msg);
		return false;
	}

	for (const FProceduralMeshSection& section : Data.Sections)
	{
		if (section.IsEmpty())
		{
			continue;
		}

		// インデックス範囲の検証 (生成コードのバグで GPU 側が不定読みしないよう弾く)
		const unsigned int vertexCount = static_cast<unsigned int>(section.Vertices.size());
		bool bValid = true;
		for (unsigned int index : section.Indices)
		{
			if (index >= vertexCount) { bValid = false; break; }
		}
		if (!bValid)
		{
			char msg[256];
			sprintf_s(msg, "[FBXModel] BuildFromProceduralMesh: index out of range, section skipped (%s)\n", Name ? Name : "");
			OutputDebugStringA(msg);
			assert(false && "procedural mesh index out of range");
			continue;
		}

		// 端数インデックスは切り捨てて三角形単位にする
		const unsigned int indexCount = static_cast<unsigned int>(section.Indices.size() / 3 * 3);

		m_Subsets.emplace_back(CreateSubset(section.Vertices.data(), vertexCount,
			section.Indices.data(), indexCount, section.MaterialIndex, section.bAffectDistanceField));
	}

	if (m_Subsets.empty())
	{
		return false;
	}

	// ---- SDF キャッシュパス ----
	// 名前 + ジオメトリハッシュで同定する (パラメータ変更で再ベイクされる)。
	// Saved/ProceduralMesh/ は無ければ作る (作れなければ非キャッシュで続行)。
	std::string cachePath;
	{
		std::string safeName = Name ? Name : "ProceduralMesh";
		for (char& c : safeName)
		{
			const bool bAlnum = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
			if (!bAlnum && c != '_' && c != '-') c = '_';
		}

		char hashText[32];
		sprintf_s(hashText, "%016llx", Data.ComputeGeometryHash());

		std::error_code ec;
		const fs::path cacheDir = fs::path("Saved") / "ProceduralMesh";
		fs::create_directories(cacheDir, ec);
		if (!ec)
		{
			cachePath = (cacheDir / (safeName + "_" + hashText)).string();
		}
	}

	FinalizeLoad(cachePath.empty() ? nullptr : cachePath.c_str());

	char msg[256];
	sprintf_s(msg, "[FBXModel] procedural mesh built: %s (%u subsets, %u tris)\n",
		Name ? Name : "", static_cast<unsigned int>(m_Subsets.size()), Data.GetNumTriangles());
	OutputDebugStringA(msg);

	return true;
}


// ================================================================
//  描画 (Draw)
//  全サブセットを DrawSubset で順に描画する。
//  PSO / 定数 / テクスチャのバインドは呼び出し元の責務で、
//  サブセットごとのマテリアル切り替えは行わない。
// ================================================================

void FBXModel::Draw()
{
	if (!m_Loaded) return;

	for (unsigned int i = 0; i < m_Subsets.size(); ++i)
		DrawSubset(i);

}


// ================================================================
//  描画 (DrawSubset)
//  呼び出し元 (FStaticMeshSceneProxy) が PSO / PRIMITIVE 定数と、
//  そのパスが必要とするマテリアル定数・テクスチャをバインド済みであること。
//  ここでは VB / IB / トポロジのみ設定して描画する。
// ================================================================

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
//  aiMesh を VERTEX_3D / uint32 インデックス列へ展開し、
//  CreateSubset で GPU バッファ化する。
// ================================================================

FBX_SUBSET FBXModel::ProcessMesh(const aiMesh* mesh, const aiScene* scene,
	const std::string& directory)
{
	(void)scene;
	(void)directory;

	const unsigned int vertexCount = mesh->mNumVertices;
	const unsigned int indexCount = mesh->mNumFaces * 3;

	// ----------------------------------------------------------------
	// 頂点（VERTEX_3D は既存プロジェクトの構造体）
	// ----------------------------------------------------------------
	std::vector<VERTEX_3D> vertices(vertexCount);

	for (unsigned int i = 0; i < vertexCount; ++i)
	{
		VERTEX_3D& dst = vertices[i];

		// 位置
		dst.Position = { mesh->mVertices[i].x,
						 mesh->mVertices[i].y,
						 mesh->mVertices[i].z };

		// 法線
		dst.Normal = mesh->HasNormals()
			? XMFLOAT3{ mesh->mNormals[i].x,
						mesh->mNormals[i].y,
						mesh->mNormals[i].z }
		: XMFLOAT3{ 0.0f, 1.0f, 0.0f };

		// 接線（aiProcess_CalcTangentSpace で生成済み）
		dst.Tangent = mesh->HasTangentsAndBitangents()
			? XMFLOAT3{ mesh->mTangents[i].x,
						mesh->mTangents[i].y,
						mesh->mTangents[i].z }
		: XMFLOAT3{ 1.0f, 0.0f, 0.0f };

		// UV（チャンネル 0）
		dst.TexCoord = mesh->HasTextureCoords(0)
			? XMFLOAT2{ mesh->mTextureCoords[0][i].x,
						mesh->mTextureCoords[0][i].y }
		: XMFLOAT2{ 0.0f, 0.0f };

		// 頂点カラー（チャンネル 0、なければ白）
		dst.Color = mesh->HasVertexColors(0)
			? XMFLOAT4{ mesh->mColors[0][i].r,
						mesh->mColors[0][i].g,
						mesh->mColors[0][i].b,
						mesh->mColors[0][i].a }
		: XMFLOAT4{ 1.0f, 1.0f, 1.0f, 1.0f };
	}

	// ----------------------------------------------------------------
	// インデックス（uint32）
	// ----------------------------------------------------------------
	std::vector<unsigned int> indices;
	indices.reserve(indexCount);

	for (unsigned int f = 0; f < mesh->mNumFaces; ++f)
	{
		const aiFace& face = mesh->mFaces[f];
		assert(face.mNumIndices == 3);
		indices.push_back(face.mIndices[0]);
		indices.push_back(face.mIndices[1]);
		indices.push_back(face.mIndices[2]);
	}

	return CreateSubset(vertices.data(), vertexCount,
		indices.data(), static_cast<unsigned int>(indices.size()), mesh->mMaterialIndex, true);
}


// ================================================================
//  プライベート：頂点 / インデックス列 → FBX_SUBSET (GPU バッファ化)
//  ローカル境界 (フラスタムカリング用 AABB) と Distance Field ベイク用
//  三角形もここで蓄積する。
// ================================================================

FBX_SUBSET FBXModel::CreateSubset(const VERTEX_3D* Vertices, unsigned int VertexCount,
	const unsigned int* Indices, unsigned int IndexCount, unsigned int MaterialIndex,
	bool bAffectDistanceField)
{
	RenderManager* rm = RenderManager::GetInstance();

	// ---- 頂点バッファ ----
	auto vb = rm->CreateVertexBuffer(sizeof(VERTEX_3D), VertexCount);
	{
		VERTEX_3D* dst = nullptr;
		HRESULT hr = vb->Resource->Map(0, nullptr, reinterpret_cast<void**>(&dst));
		assert(SUCCEEDED(hr));

		memcpy(dst, Vertices, sizeof(VERTEX_3D) * VertexCount);

		vb->Resource->Unmap(0, nullptr);
	}

	for (unsigned int i = 0; i < VertexCount; ++i)
	{
		const XMFLOAT3& p = Vertices[i].Position;
		m_LocalBoundsMin.x = fminf(m_LocalBoundsMin.x, p.x);
		m_LocalBoundsMin.y = fminf(m_LocalBoundsMin.y, p.y);
		m_LocalBoundsMin.z = fminf(m_LocalBoundsMin.z, p.z);
		m_LocalBoundsMax.x = fmaxf(m_LocalBoundsMax.x, p.x);
		m_LocalBoundsMax.y = fmaxf(m_LocalBoundsMax.y, p.y);
		m_LocalBoundsMax.z = fmaxf(m_LocalBoundsMax.z, p.z);
		m_bHasLocalBounds = true;
	}

	// ---- インデックスバッファ ----
	auto ib = rm->CreateIndexBuffer(IndexCount);
	{
		unsigned int* dst = nullptr;
		HRESULT hr = ib->Resource->Map(0, nullptr, reinterpret_cast<void**>(&dst));
		assert(SUCCEEDED(hr));

		memcpy(dst, Indices, sizeof(unsigned int) * IndexCount);

		ib->Resource->Unmap(0, nullptr);
	}

	// Distance Field ベイク用に三角形頂点を蓄積 (ローカル空間)。
	// 開いた面のセクション (葉カード等) は除外する
	if (bAffectDistanceField)
	{
		m_DFTriangles.reserve(m_DFTriangles.size() + IndexCount);
		for (unsigned int i = 0; i + 2 < IndexCount; i += 3)
		{
			m_DFTriangles.push_back(Vertices[Indices[i]].Position);
			m_DFTriangles.push_back(Vertices[Indices[i + 1]].Position);
			m_DFTriangles.push_back(Vertices[Indices[i + 2]].Position);
		}
	}

	// ---- サブセット組み立て ----
	FBX_SUBSET subset;
	subset.VertexBuffer = std::move(vb);
	subset.IndexBuffer = std::move(ib);
	subset.IndexCount = IndexCount;
	subset.MaterialIndex = MaterialIndex;

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

	// BaseColor / Diffuse は sRGB で作成されている。その他のマップ
	// (法線 / ORM / メタリック・ラフネス / AO) はリニア。
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