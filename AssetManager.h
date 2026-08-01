#pragma once
#include <memory>
#include <string>
#include <unordered_map>

class FBXModel;
struct TEXTURE;

// ============================================================
//  FAssetManager
//  UE5 のアセット共有 (UStaticMesh / UTexture2D が複数の
//  コンポーネントから参照カウントで共有される仕組み) に相当する
//  最小のアセットキャッシュ。
//  パスをキーに FBXModel / TEXTURE を shared_ptr で共有し、
//  同じアセットを使うコンポーネントが GPU バッファ / テクスチャを
//  重複生成しないようにする。プロキシ (FStaticMeshSceneProxy 等) も
//  同じ shared_ptr を保持するため、コンポーネントより長生きしても
//  レンダーデータは安全に生存する。
//
//  ※ キャッシュはプログラム生存期間中アセットを保持する
//    (LRU などのエビクションは将来の課題)。
//  ※ TEXTURE / FBXModel が抱える GPU リソースの破棄は
//    RenderManager の遅延削除キューを経由するため、shared_ptr と
//    組み合わせることで実行時のアセット差し替えも安全。
//  ※ GameManager が RenderManager の直後に所有する
//    (破棄はメンバ宣言の逆順 = RenderManager より先に解放される)。
// ============================================================

class FAssetManager
{
private:
	static FAssetManager* m_Instance;

	// パスをキーにしたキャッシュ。
	// ※ ロードフラグ (sRGB / FlipUV) はキーに含めていないため、
	//   同一パスをフラグ違いで再要求しても最初のロード結果が返る
	//   (現状の使用箇所では衝突しない。必要になったらキーへ含める)
	std::unordered_map<std::string, std::shared_ptr<FBXModel>> m_StaticMeshCache;
	std::unordered_map<std::string, std::shared_ptr<TEXTURE>>  m_TextureCache;

public:
	FAssetManager();
	~FAssetManager();

	static FAssetManager* GetInstance() { return m_Instance; }

	// スタティックメッシュのロード / キャッシュ取得 (UStaticMesh 相当)。
	// 初回は FBXModel::Load でロードし、以降は共有インスタンスを返す。
	// ロード失敗もキャッシュする (IsLoaded() = false で判定できる)。
	std::shared_ptr<FBXModel> LoadStaticMesh(const std::string& Path, bool FlipUV = true);

	// テクスチャのロード / キャッシュ取得 (UTexture2D 相当)。
	// 初回は RenderManager::LoadTexture でロードし、以降は共有
	// インスタンスを返す。
	std::shared_ptr<TEXTURE> LoadTexture(const std::string& Path, bool sRGB = false);
};
