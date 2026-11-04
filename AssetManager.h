#pragma once
#include <memory>
#include <string>
#include <unordered_map>

#include <vector>

class FBXModel;
struct TEXTURE;
struct FProceduralMeshData;
class USkeletalMesh;
class USkeleton;
class UAnimSequence;
struct FSkeletalMeshImportData;

// ============================================================
//  FAssetManager
//  アセット共有 (UStaticMesh / UTexture2D が複数の
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

	// プロシージャルメッシュは弱参照で共有する。使っているコンポーネント /
	// プロキシが居なくなった時点で解放され、GPU バッファと SDF スロットが
	// 返却される (パラメータ編集のたびに別名のメッシュが増えるため)
	std::unordered_map<std::string, std::weak_ptr<FBXModel>>   m_ProceduralMeshCache;

	// スケルタルメッシュ (USkeletalMesh) / アニメーション (UAnimSequence) のキャッシュ。
	// スケルタルメッシュはパス (またはプロシージャル名) をキーに、アニメーションは
	// "<パス>" (ファイル単位の列) と "<名前>" (登録名) をキーに持つ
	std::unordered_map<std::string, std::shared_ptr<USkeletalMesh>>              m_SkeletalMeshCache;
	std::unordered_map<std::string, std::vector<std::shared_ptr<UAnimSequence>>> m_AnimSequenceFileCache;
	std::unordered_map<std::string, std::shared_ptr<UAnimSequence>>              m_AnimSequenceCache;

public:
	FAssetManager();
	~FAssetManager();

	static FAssetManager* GetInstance() { return m_Instance; }

	// スタティックメッシュのロード / キャッシュ取得 (UStaticMesh 相当)。
	// 初回は FBXModel::Load でロードし、以降は共有インスタンスを返す。
	// ロード失敗もキャッシュする (IsLoaded() = false で判定できる)。
	std::shared_ptr<FBXModel> LoadStaticMesh(const std::string& Path, bool FlipUV = true);

	// プロシージャルメッシュの構築 / キャッシュ取得。
	// Name をキーに弱参照で共有する (FBX のパスとは別の表)。同じ Name の
	// メッシュが生きていれば Data を見ずにそれを返すので、形状を変えるときは
	// 別の Name (パラメータのハッシュなど) を使うこと。
	// 誰も使わなくなったメッシュは解放される (構築失敗も同じ扱い)。
	std::shared_ptr<FBXModel> CreateProceduralMesh(const std::string& Name, const FProceduralMeshData& Data);

	// 生きているプロシージャルメッシュを取得する (未登録 / 解放済みなら nullptr)
	std::shared_ptr<FBXModel> FindProceduralMesh(const std::string& Name) const;

	// テクスチャのロード / キャッシュ取得 (UTexture2D 相当)。
	// 初回は RenderManager::LoadTexture でロードし、以降は共有
	// インスタンスを返す。
	std::shared_ptr<TEXTURE> LoadTexture(const std::string& Path, bool sRGB = false);

	// ---- スケルタルメッシュ / アニメーション (USkeletalMesh / UAnimSequence 相当) ----
	// FBX / glTF からのロード (FSkeletalMeshImporter)。ロード失敗もキャッシュする (IsLoaded() = false)
	std::shared_ptr<USkeletalMesh> LoadSkeletalMesh(const std::string& Path, bool FlipUV = true);

	// インポートデータから構築して Name で共有する (プロシージャル生成用。同名なら既存を返す)
	std::shared_ptr<USkeletalMesh> CreateSkeletalMesh(const std::string& Name, const FSkeletalMeshImportData& Data,
		std::shared_ptr<USkeleton> Skeleton = nullptr);
	std::shared_ptr<USkeletalMesh> FindSkeletalMesh(const std::string& Name) const;

	// ファイル内の全アニメーションをロードする (Skeleton のボーン名で対応付け)。
	// ファイル単位でキャッシュし、各シーケンスは "<Path>#<名前>" でも引ける
	const std::vector<std::shared_ptr<UAnimSequence>>& LoadAnimSequences(const std::string& Path,
		const std::shared_ptr<USkeleton>& Skeleton, bool FlipUV = true);
	// ファイル内の Index 番目 (無ければ nullptr)
	std::shared_ptr<UAnimSequence> LoadAnimSequence(const std::string& Path, unsigned int Index,
		const std::shared_ptr<USkeleton>& Skeleton, bool FlipUV = true);

	// 生成したシーケンスを名前で登録 / 取得する (プロシージャル生成用)
	void RegisterAnimSequence(const std::string& Name, std::shared_ptr<UAnimSequence> Sequence);
	std::shared_ptr<UAnimSequence> FindAnimSequence(const std::string& Name) const;
	// 登録済みの全シーケンス名 (ソート済み。Details のアセット選択用)
	void GetAnimSequenceNames(std::vector<std::string>& OutNames) const;
};
