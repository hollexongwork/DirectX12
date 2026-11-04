#pragma once
#include <memory>
#include <string>
#include <vector>
#include "SkeletalMesh.h"
#include "AnimSequence.h"

// ============================================================
//  FSkeletalMeshImporter
//  UnFbx::FFbxImporter のスケルタルメッシュ / アニメーション部分
//  (ImportSkeletalMesh / ImportAnimations) に相当。assimp で
//  FBX / glTF 等を読み、
//    - aiBone と参照ノードの祖先からリファレンススケルトン
//      (aiNode::mTransformation = リファレンスポーズ、
//       aiBone::mOffsetMatrix = 逆バインド行列)
//    - aiMesh の頂点 / 影響 / セクション (= aiMesh ごと、MaterialIndex = aiMesh::mMaterialIndex)
//    - aiAnimation のチャンネルを SampleFrameRate で等間隔にリサンプルした UAnimSequence
//  を作る。座標系は静的メッシュ (FBXModel::Load) と同じ
//  aiProcess_ConvertToLeftHanded。単位変換はしない (アクターのスケールで合わせる)。
//
//  頂点はメッシュノードのワールド変換 (シーンルート基準) を掛けてコンポーネント空間へ
//  そろえ、逆バインド行列は mOffsetMatrix * inverse(メッシュノード変換) にする
//  (メッシュが複数ノードに分かれていても 1 組の RefBasesInvMatrix で済む)。
// ============================================================

struct aiScene;
struct aiNode;

class FSkeletalMeshImporter
{
public:
	struct FImportOptions
	{
		bool  bFlipUV = true;			// FBXModel::Load と同じ既定
		float SampleFrameRate = 30.0f;	// アニメーションのリサンプルレート [fps]
	};

	// スケルタルメッシュの取り込み (ボーンを持つメッシュが無ければ false)
	static bool ImportSkeletalMesh(const char* FilePath, FSkeletalMeshImportData& OutData,
		const FImportOptions& Options = FImportOptions());

	// ファイル内の全アニメーションを取り込む。Skeleton のボーン名でチャンネルを対応付ける
	// (無いボーンのチャンネルは捨てる)。アニメーションが無ければ false
	static bool ImportAnimSequences(const char* FilePath, const std::shared_ptr<USkeleton>& Skeleton,
		std::vector<std::shared_ptr<UAnimSequence>>& OutSequences,
		const FImportOptions& Options = FImportOptions());

	// ---- 書き出し (検証用) ----
	// インポートデータとアニメーションを assimp の Exporter で書き出す (FormatId: "gltf2" / "fbx" など)。
	// 本エンジンの左手系データを assimp の右手系 (aiProcess_ConvertToLeftHanded の逆: Z 反転 /
	// 巻き順反転 / V 反転 / 四元数の xy 反転) に戻してから書くので、ImportSkeletalMesh /
	// ImportAnimSequences で読み戻すと同じメッシュ / ポーズになる (テストドライバの skeletal シナリオ)。
	// リポジトリにリグ付きアセットが無いため、インポート経路の検証に使う
	static bool ExportSkeletalMesh(const char* FilePath, const char* FormatId, const FSkeletalMeshImportData& Data,
		const std::vector<std::shared_ptr<UAnimSequence>>& Sequences, const FImportOptions& Options = FImportOptions());

private:
	static unsigned int GetImportFlags(bool bFlipUV);

	// aiBone が参照するノードとその祖先からリファレンススケルトンを作る。
	// OutNodeToBone はノード -> ボーン添字
	static bool BuildReferenceSkeleton(const aiScene* Scene, FReferenceSkeleton& OutRefSkeleton,
		std::vector<std::pair<const aiNode*, int>>& OutNodeToBone);
};
