#include "Main.h"
#include "RenderManager.h"
#include "AssetManager.h"
#include "FBXModel.h"
#include "ProceduralMeshGenerator.h"
#include "SkeletalMesh.h"
#include "AnimSequence.h"
#include "SkeletalMeshImporter.h"

FAssetManager* FAssetManager::m_Instance = nullptr;

FAssetManager::FAssetManager()
{
	m_Instance = this;
}

FAssetManager::~FAssetManager()
{
	// キャッシュ (メンバ) の破棄で残存アセットが解放される。
	// GPU リソースは TEXTURE / FBXModel のデストラクタが
	// RenderManager の遅延削除キューへ回すため、この時点で
	// RenderManager が生存していること (GameManager のメンバ宣言順で保証)。
	m_Instance = nullptr;
}

std::shared_ptr<FBXModel> FAssetManager::LoadStaticMesh(const std::string& Path, bool FlipUV)
{
	auto it = m_StaticMeshCache.find(Path);
	if (it != m_StaticMeshCache.end())
	{
		return it->second;
	}

	// 未キャッシュ: ロードして登録する。ロード失敗もキャッシュし、
	// 同じ欠損パスの再ロード試行を防ぐ (IsLoaded() で判定できる)
	std::shared_ptr<FBXModel> mesh = std::make_shared<FBXModel>();
	mesh->Load(Path.c_str(), FlipUV);

	m_StaticMeshCache.emplace(Path, mesh);
	return mesh;
}

std::shared_ptr<FBXModel> FAssetManager::CreateProceduralMesh(const std::string& Name, const FProceduralMeshData& Data)
{
	if (std::shared_ptr<FBXModel> alive = FindProceduralMesh(Name))
	{
		return alive;
	}

	// 解放済みのエントリを掃除してから登録する (表が無限に増えないように)
	for (auto it = m_ProceduralMeshCache.begin(); it != m_ProceduralMeshCache.end(); )
	{
		it = it->second.expired() ? m_ProceduralMeshCache.erase(it) : std::next(it);
	}

	std::shared_ptr<FBXModel> mesh = std::make_shared<FBXModel>();
	mesh->BuildFromProceduralMesh(Name.c_str(), Data);

	m_ProceduralMeshCache[Name] = mesh;
	return mesh;
}

std::shared_ptr<FBXModel> FAssetManager::FindProceduralMesh(const std::string& Name) const
{
	auto it = m_ProceduralMeshCache.find(Name);
	return (it != m_ProceduralMeshCache.end()) ? it->second.lock() : nullptr;
}

// ============================================================
//  スケルタルメッシュ / アニメーション
// ============================================================

std::shared_ptr<USkeletalMesh> FAssetManager::LoadSkeletalMesh(const std::string& Path, bool FlipUV)
{
	auto it = m_SkeletalMeshCache.find(Path);
	if (it != m_SkeletalMeshCache.end())
	{
		return it->second;
	}

	std::shared_ptr<USkeletalMesh> mesh = std::make_shared<USkeletalMesh>();

	FSkeletalMeshImporter::FImportOptions options;
	options.bFlipUV = FlipUV;
	FSkeletalMeshImportData importData;
	if (FSkeletalMeshImporter::ImportSkeletalMesh(Path.c_str(), importData, options))
	{
		mesh->Build(Path, importData);
	}

	m_SkeletalMeshCache.emplace(Path, mesh);
	return mesh;
}

std::shared_ptr<USkeletalMesh> FAssetManager::CreateSkeletalMesh(const std::string& Name, const FSkeletalMeshImportData& Data,
	std::shared_ptr<USkeleton> Skeleton)
{
	if (std::shared_ptr<USkeletalMesh> existing = FindSkeletalMesh(Name))
	{
		return existing;
	}

	std::shared_ptr<USkeletalMesh> mesh = std::make_shared<USkeletalMesh>();
	mesh->Build(Name, Data, std::move(Skeleton));

	m_SkeletalMeshCache.emplace(Name, mesh);
	return mesh;
}

std::shared_ptr<USkeletalMesh> FAssetManager::FindSkeletalMesh(const std::string& Name) const
{
	auto it = m_SkeletalMeshCache.find(Name);
	return (it != m_SkeletalMeshCache.end()) ? it->second : nullptr;
}

const std::vector<std::shared_ptr<UAnimSequence>>& FAssetManager::LoadAnimSequences(const std::string& Path,
	const std::shared_ptr<USkeleton>& Skeleton, bool FlipUV)
{
	auto it = m_AnimSequenceFileCache.find(Path);
	if (it != m_AnimSequenceFileCache.end())
	{
		return it->second;
	}

	std::vector<std::shared_ptr<UAnimSequence>> sequences;
	FSkeletalMeshImporter::FImportOptions options;
	options.bFlipUV = FlipUV;
	FSkeletalMeshImporter::ImportAnimSequences(Path.c_str(), Skeleton, sequences, options);

	for (const std::shared_ptr<UAnimSequence>& sequence : sequences)
	{
		m_AnimSequenceCache.emplace(Path + "#" + sequence->GetName(), sequence);
	}

	return m_AnimSequenceFileCache.emplace(Path, std::move(sequences)).first->second;
}

std::shared_ptr<UAnimSequence> FAssetManager::LoadAnimSequence(const std::string& Path, unsigned int Index,
	const std::shared_ptr<USkeleton>& Skeleton, bool FlipUV)
{
	const std::vector<std::shared_ptr<UAnimSequence>>& sequences = LoadAnimSequences(Path, Skeleton, FlipUV);
	return (Index < sequences.size()) ? sequences[Index] : nullptr;
}

void FAssetManager::RegisterAnimSequence(const std::string& Name, std::shared_ptr<UAnimSequence> Sequence)
{
	m_AnimSequenceCache[Name] = std::move(Sequence);
}

std::shared_ptr<UAnimSequence> FAssetManager::FindAnimSequence(const std::string& Name) const
{
	auto it = m_AnimSequenceCache.find(Name);
	return (it != m_AnimSequenceCache.end()) ? it->second : nullptr;
}

void FAssetManager::GetAnimSequenceNames(std::vector<std::string>& OutNames) const
{
	OutNames.clear();
	for (const auto& kv : m_AnimSequenceCache)
	{
		if (kv.second) OutNames.push_back(kv.first);
	}
	std::sort(OutNames.begin(), OutNames.end());
}

std::shared_ptr<TEXTURE> FAssetManager::LoadTexture(const std::string& Path, bool sRGB)
{
	auto it = m_TextureCache.find(Path);
	if (it != m_TextureCache.end())
	{
		return it->second;
	}

	// 未キャッシュ: RHI 経由でロードして登録する
	// (unique_ptr -> shared_ptr へ所有権を移す)
	std::shared_ptr<TEXTURE> texture =
		RenderManager::GetInstance()->LoadTexture(Path.c_str(), sRGB);

	m_TextureCache.emplace(Path, texture);
	return texture;
}


