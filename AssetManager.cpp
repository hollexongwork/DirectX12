#include "Main.h"
#include "RenderManager.h"
#include "AssetManager.h"
#include "FBXModel.h"

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


