#include "Main.h"
#include "SkeletalMeshImporter.h"

#include "assimp/Importer.hpp"
#include "assimp/Exporter.hpp"
#include "assimp/scene.h"
#include "assimp/postprocess.h"

#include <unordered_map>
#include <unordered_set>
#include <functional>
#include <cmath>

namespace
{
	// aiMatrix4x4 (列ベクトル規約 v' = M v) -> XMMATRIX (行ベクトル規約 v' = v M) = 転置
	XMMATRIX ToXMMatrix(const aiMatrix4x4& M)
	{
		return XMMatrixSet(
			M.a1, M.b1, M.c1, M.d1,
			M.a2, M.b2, M.c2, M.d2,
			M.a3, M.b3, M.c3, M.d3,
			M.a4, M.b4, M.c4, M.d4);
	}

	FTransform ToTransform(const aiMatrix4x4& M)
	{
		aiVector3D scaling, position;
		aiQuaternion rotation;
		M.Decompose(scaling, rotation, position);

		FTransform t;
		t.Rotation = { rotation.x, rotation.y, rotation.z, rotation.w };
		t.Translation = { position.x, position.y, position.z };
		t.Scale3D = { scaling.x, scaling.y, scaling.z };
		t.NormalizeRotation();
		return t;
	}

	const aiNode* FindNode(const aiNode* Node, const char* Name)
	{
		if (Node == nullptr) return nullptr;
		if (strcmp(Node->mName.C_Str(), Name) == 0) return Node;
		for (unsigned int i = 0; i < Node->mNumChildren; ++i)
		{
			if (const aiNode* found = FindNode(Node->mChildren[i], Name)) return found;
		}
		return nullptr;
	}

	// シーンルート基準のノード変換 (列ベクトル規約)
	aiMatrix4x4 GetGlobalTransform(const aiNode* Node)
	{
		aiMatrix4x4 m;
		for (const aiNode* n = Node; n != nullptr; n = n->mParent)
		{
			m = n->mTransformation * m;
		}
		return m;
	}

	void LogImport(const char* Format, const char* A, const char* B = "")
	{
		char msg[512];
		sprintf_s(msg, Format, A, B);
		OutputDebugStringA(msg);
	}

	// ---- アニメーションキーの補間 (時刻はティック) ----
	template <class TKey>
	void FindKeys(const TKey* Keys, unsigned int NumKeys, double Time, unsigned int& OutK1, unsigned int& OutK2, float& OutAlpha)
	{
		if (NumKeys == 0) { OutK1 = OutK2 = 0; OutAlpha = 0.0f; return; }
		if (Time <= Keys[0].mTime || NumKeys == 1) { OutK1 = OutK2 = 0; OutAlpha = 0.0f; return; }
		if (Time >= Keys[NumKeys - 1].mTime) { OutK1 = OutK2 = NumKeys - 1; OutAlpha = 0.0f; return; }

		unsigned int k = 0;
		while (k + 1 < NumKeys && Keys[k + 1].mTime <= Time) ++k;
		OutK1 = k;
		OutK2 = (std::min)(k + 1, NumKeys - 1);
		const double span = Keys[OutK2].mTime - Keys[OutK1].mTime;
		OutAlpha = (span > 0.0) ? static_cast<float>((Time - Keys[OutK1].mTime) / span) : 0.0f;
	}

	XMFLOAT3 SampleVectorKeys(const aiVectorKey* Keys, unsigned int NumKeys, double Time, const XMFLOAT3& Default)
	{
		if (NumKeys == 0 || Keys == nullptr) return Default;
		unsigned int k1, k2; float alpha;
		FindKeys(Keys, NumKeys, Time, k1, k2, alpha);
		const aiVector3D& a = Keys[k1].mValue;
		const aiVector3D& b = Keys[k2].mValue;
		return { a.x + (b.x - a.x) * alpha, a.y + (b.y - a.y) * alpha, a.z + (b.z - a.z) * alpha };
	}

	XMFLOAT4 SampleQuatKeys(const aiQuatKey* Keys, unsigned int NumKeys, double Time, const XMFLOAT4& Default)
	{
		if (NumKeys == 0 || Keys == nullptr) return Default;
		unsigned int k1, k2; float alpha;
		FindKeys(Keys, NumKeys, Time, k1, k2, alpha);
		const aiQuaternion& a = Keys[k1].mValue;
		const aiQuaternion& b = Keys[k2].mValue;
		const XMVECTOR qa = XMVectorSet(a.x, a.y, a.z, a.w);
		const XMVECTOR qb = XMVectorSet(b.x, b.y, b.z, b.w);
		XMFLOAT4 out;
		XMStoreFloat4(&out, XMQuaternionNormalize((alpha <= 0.0f) ? qa : XMQuaternionSlerp(qa, qb, alpha)));
		return out;
	}
}

unsigned int FSkeletalMeshImporter::GetImportFlags(bool bFlipUV)
{
	unsigned int flags =
		aiProcess_Triangulate |
		aiProcess_CalcTangentSpace |
		aiProcess_GenSmoothNormals |
		aiProcess_JoinIdenticalVertices |
		aiProcess_SortByPType |
		aiProcess_LimitBoneWeights |		// 4 影響 (AI_LBW_MAX_WEIGHTS の既定)
		aiProcess_PopulateArmatureData |
		aiProcess_ConvertToLeftHanded;		// 静的メッシュ (FBXModel::Load) と同じ座標系
	if (bFlipUV) flags |= aiProcess_FlipUVs;
	return flags;
}

// ============================================================
//  BuildReferenceSkeleton
// ============================================================
bool FSkeletalMeshImporter::BuildReferenceSkeleton(const aiScene* Scene, FReferenceSkeleton& OutRefSkeleton,
	std::vector<std::pair<const aiNode*, int>>& OutNodeToBone)
{
	OutRefSkeleton.Empty();
	OutNodeToBone.clear();

	// ---- aiBone が参照するノード ----
	std::unordered_set<const aiNode*> needed;
	for (unsigned int m = 0; m < Scene->mNumMeshes; ++m)
	{
		const aiMesh* mesh = Scene->mMeshes[m];
		for (unsigned int b = 0; b < mesh->mNumBones; ++b)
		{
			const aiBone* bone = mesh->mBones[b];
			const aiNode* node = bone->mNode ? bone->mNode : FindNode(Scene->mRootNode, bone->mName.C_Str());
			if (node == nullptr)
			{
				LogImport("[SkeletalMeshImporter] bone node not found: %s%s\n", bone->mName.C_Str());
				continue;
			}
			// 祖先 (シーンルートまで) もボーンにする (UE はノード階層全体を取り込む)
			for (const aiNode* n = node; n != nullptr; n = n->mParent)
			{
				needed.insert(n);
			}
		}
	}

	if (needed.empty())
	{
		return false;
	}

	// ---- ルートからの深さ優先で親 -> 子の順に追加する ----
	std::function<void(const aiNode*, int)> addNode = [&](const aiNode* node, int parentIndex)
		{
			int myIndex = parentIndex;
			if (needed.count(node) != 0)
			{
				myIndex = OutRefSkeleton.Add(FMeshBoneInfo(node->mName.C_Str(), parentIndex), ToTransform(node->mTransformation));
				OutNodeToBone.emplace_back(node, myIndex);
			}
			for (unsigned int i = 0; i < node->mNumChildren; ++i)
			{
				addNode(node->mChildren[i], myIndex);
			}
		};
	addNode(Scene->mRootNode, INDEX_NONE);

	return OutRefSkeleton.GetNum() > 0;
}

// ============================================================
//  ImportSkeletalMesh
// ============================================================
bool FSkeletalMeshImporter::ImportSkeletalMesh(const char* FilePath, FSkeletalMeshImportData& OutData, const FImportOptions& Options)
{
	OutData = FSkeletalMeshImportData{};

	Assimp::Importer importer;
	const aiScene* scene = importer.ReadFile(FilePath, GetImportFlags(Options.bFlipUV));
	if (!scene || (scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE) || !scene->mRootNode)
	{
		LogImport("[SkeletalMeshImporter] load failed: %s\n  file: %s\n", importer.GetErrorString(), FilePath);
		return false;
	}

	std::vector<std::pair<const aiNode*, int>> nodeToBone;
	if (!BuildReferenceSkeleton(scene, OutData.RefSkeleton, nodeToBone))
	{
		LogImport("[SkeletalMeshImporter] no bones in file: %s%s\n", FilePath);
		return false;
	}

	const int numBones = OutData.RefSkeleton.GetNum();

	// 逆バインド行列は aiBone から埋める。aiBone が無いボーン (祖先ノード) はリファレンスポーズの逆
	std::vector<FTransform> componentSpaceRefPose;
	OutData.RefSkeleton.ComputeComponentSpaceRefPose(componentSpaceRefPose);
	OutData.RefBasesInvMatrix.resize(numBones);
	std::vector<bool> hasOffset(numBones, false);
	for (int i = 0; i < numBones; ++i)
	{
		XMStoreFloat4x4(&OutData.RefBasesInvMatrix[i], XMMatrixInverse(nullptr, componentSpaceRefPose[i].ToMatrixWithScale()));
	}

	// ---- メッシュノードを辿って頂点を集める ----
	std::function<void(const aiNode*)> processNode = [&](const aiNode* node)
		{
			const aiMatrix4x4 nodeGlobal = GetGlobalTransform(node);
			const XMMATRIX meshToComponent = ToXMMatrix(nodeGlobal);
			const XMMATRIX componentToMesh = XMMatrixInverse(nullptr, meshToComponent);

			for (unsigned int mi = 0; mi < node->mNumMeshes; ++mi)
			{
				const aiMesh* mesh = scene->mMeshes[node->mMeshes[mi]];
				if (mesh->mNumVertices == 0 || mesh->mNumFaces == 0) continue;

				const unsigned int baseVertex = static_cast<unsigned int>(OutData.Vertices.size());

				// 頂点 (メッシュ空間 -> コンポーネント空間)
				for (unsigned int v = 0; v < mesh->mNumVertices; ++v)
				{
					FSkeletalMeshImportVertex dst;

					const XMVECTOR p = XMVector3TransformCoord(XMVectorSet(mesh->mVertices[v].x, mesh->mVertices[v].y, mesh->mVertices[v].z, 1.0f), meshToComponent);
					XMStoreFloat3(&dst.Position, p);

					if (mesh->HasNormals())
					{
						const XMVECTOR n = XMVector3Normalize(XMVector3TransformNormal(XMVectorSet(mesh->mNormals[v].x, mesh->mNormals[v].y, mesh->mNormals[v].z, 0.0f), meshToComponent));
						XMStoreFloat3(&dst.Normal, n);
					}
					if (mesh->HasTangentsAndBitangents())
					{
						const XMVECTOR t = XMVector3Normalize(XMVector3TransformNormal(XMVectorSet(mesh->mTangents[v].x, mesh->mTangents[v].y, mesh->mTangents[v].z, 0.0f), meshToComponent));
						XMStoreFloat3(&dst.Tangent, t);
					}
					if (mesh->HasTextureCoords(0))
					{
						dst.TexCoord = { mesh->mTextureCoords[0][v].x, mesh->mTextureCoords[0][v].y };
					}
					if (mesh->HasVertexColors(0))
					{
						dst.Color = { mesh->mColors[0][v].r, mesh->mColors[0][v].g, mesh->mColors[0][v].b, mesh->mColors[0][v].a };
					}

					OutData.Vertices.push_back(dst);
				}

				// 影響 (aiBone::mWeights) + 逆バインド行列
				for (unsigned int b = 0; b < mesh->mNumBones; ++b)
				{
					const aiBone* bone = mesh->mBones[b];
					const int boneIndex = OutData.RefSkeleton.FindBoneIndex(bone->mName.C_Str());
					if (boneIndex == INDEX_NONE) continue;

					if (!hasOffset[boneIndex])
					{
						// Offset (メッシュ空間 -> ボーン空間) * inverse(メッシュノード) = コンポーネント空間 -> ボーン空間
						XMStoreFloat4x4(&OutData.RefBasesInvMatrix[boneIndex], componentToMesh * ToXMMatrix(bone->mOffsetMatrix));
						hasOffset[boneIndex] = true;
					}

					for (unsigned int w = 0; w < bone->mNumWeights; ++w)
					{
						const aiVertexWeight& vw = bone->mWeights[w];
						if (vw.mVertexId < mesh->mNumVertices)
						{
							OutData.Vertices[baseVertex + vw.mVertexId].AddInfluence(boneIndex, vw.mWeight);
						}
					}
				}

				// セクション (= aiMesh。MaterialIndex = aiMesh::mMaterialIndex)
				FSkeletalMeshImportSection section;
				section.MaterialIndex = mesh->mMaterialIndex;
				section.FirstIndex = static_cast<unsigned int>(OutData.Indices.size());
				section.NumTriangles = 0;
				for (unsigned int f = 0; f < mesh->mNumFaces; ++f)
				{
					const aiFace& face = mesh->mFaces[f];
					if (face.mNumIndices != 3) continue;
					OutData.Indices.push_back(baseVertex + face.mIndices[0]);
					OutData.Indices.push_back(baseVertex + face.mIndices[1]);
					OutData.Indices.push_back(baseVertex + face.mIndices[2]);
					section.NumTriangles++;
				}
				if (section.NumTriangles > 0)
				{
					OutData.Sections.push_back(section);
				}
			}

			for (unsigned int i = 0; i < node->mNumChildren; ++i)
			{
				processNode(node->mChildren[i]);
			}
		};
	processNode(scene->mRootNode);

	if (OutData.IsEmpty())
	{
		LogImport("[SkeletalMeshImporter] no skinned geometry: %s%s\n", FilePath);
		return false;
	}

	char msg[256];
	sprintf_s(msg, "[SkeletalMeshImporter] imported %s: %d bones, %u verts, %u sections\n",
		FilePath, numBones, static_cast<unsigned int>(OutData.Vertices.size()), static_cast<unsigned int>(OutData.Sections.size()));
	OutputDebugStringA(msg);
	return true;
}

// ============================================================
//  ImportAnimSequences
// ============================================================
bool FSkeletalMeshImporter::ImportAnimSequences(const char* FilePath, const std::shared_ptr<USkeleton>& Skeleton,
	std::vector<std::shared_ptr<UAnimSequence>>& OutSequences, const FImportOptions& Options)
{
	OutSequences.clear();
	if (!Skeleton) return false;

	Assimp::Importer importer;
	const aiScene* scene = importer.ReadFile(FilePath, GetImportFlags(Options.bFlipUV));
	if (!scene || !scene->mRootNode)
	{
		LogImport("[SkeletalMeshImporter] load failed: %s\n  file: %s\n", importer.GetErrorString(), FilePath);
		return false;
	}
	if (scene->mNumAnimations == 0)
	{
		return false;
	}

	const FReferenceSkeleton& refSkeleton = Skeleton->GetReferenceSkeleton();
	const float sampleRate = (Options.SampleFrameRate > 0.0f) ? Options.SampleFrameRate : 30.0f;

	for (unsigned int a = 0; a < scene->mNumAnimations; ++a)
	{
		const aiAnimation* anim = scene->mAnimations[a];
		const double ticksPerSecond = (anim->mTicksPerSecond > 0.0) ? anim->mTicksPerSecond : 25.0;
		const double durationSec = (anim->mDuration > 0.0) ? anim->mDuration / ticksPerSecond : 0.0;

		auto sequence = std::make_shared<UAnimSequence>();
		std::string name = anim->mName.C_Str();
		if (name.empty()) name = "Anim" + std::to_string(a);
		sequence->SetName(name);
		sequence->SetSkeleton(Skeleton);

		// [0, duration] を等間隔にリサンプルする
		const int numKeys = (durationSec > 0.0) ? (std::max)(2, static_cast<int>(std::lround(durationSec * sampleRate)) + 1) : 1;
		FFrameRate frameRate;
		frameRate.Numerator = static_cast<int>(std::lround(sampleRate));
		frameRate.Denominator = 1;
		sequence->SetSamplingFrameRate(frameRate);
		sequence->SetNumberOfKeys(numKeys);
		sequence->SetSequenceLength(static_cast<float>(durationSec));

		int numTracks = 0;
		for (unsigned int c = 0; c < anim->mNumChannels; ++c)
		{
			const aiNodeAnim* channel = anim->mChannels[c];
			const int boneIndex = refSkeleton.FindBoneIndex(channel->mNodeName.C_Str());
			if (boneIndex == INDEX_NONE) continue;

			const FTransform& refPose = refSkeleton.GetRefBonePose()[boneIndex];

			FRawAnimSequenceTrack track;
			track.PosKeys.resize(numKeys);
			track.RotKeys.resize(numKeys);
			track.ScaleKeys.resize(numKeys);
			for (int k = 0; k < numKeys; ++k)
			{
				const double t = (numKeys > 1) ? (static_cast<double>(k) / static_cast<double>(numKeys - 1)) * durationSec : 0.0;
				const double ticks = t * ticksPerSecond;
				track.PosKeys[k] = SampleVectorKeys(channel->mPositionKeys, channel->mNumPositionKeys, ticks, refPose.Translation);
				track.RotKeys[k] = SampleQuatKeys(channel->mRotationKeys, channel->mNumRotationKeys, ticks, refPose.Rotation);
				track.ScaleKeys[k] = SampleVectorKeys(channel->mScalingKeys, channel->mNumScalingKeys, ticks, refPose.Scale3D);
			}

			if (sequence->AddTrack(boneIndex, track)) ++numTracks;
		}

		char msg[256];
		sprintf_s(msg, "[SkeletalMeshImporter] animation '%s': %.3f s, %d keys, %d tracks\n",
			name.c_str(), durationSec, numKeys, numTracks);
		OutputDebugStringA(msg);

		OutSequences.push_back(sequence);
	}

	return !OutSequences.empty();
}

// ============================================================
//  ExportSkeletalMesh (検証用)
//  aiProcess_ConvertToLeftHanded の逆変換を掛けた aiScene を組み立てて書き出す。
//  ノード: RootNode -> (ボーン階層) / ChainMesh (メッシュノード, 単位変換)
// ============================================================
namespace
{
	// 行ベクトル規約の XMMATRIX -> aiMatrix4x4 (列ベクトル規約) = 転置
	aiMatrix4x4 ToAiMatrix(FXMMATRIX M)
	{
		XMFLOAT4X4 f;
		XMStoreFloat4x4(&f, M);
		return aiMatrix4x4(
			f._11, f._21, f._31, f._41,
			f._12, f._22, f._32, f._42,
			f._13, f._23, f._33, f._43,
			f._14, f._24, f._34, f._44);
	}

	// Z 反転の鏡映 S * M * S (MakeLeftHandedProcess と同じ: 3 行目と 3 列目の符号反転)
	XMMATRIX MirrorZ(FXMMATRIX M)
	{
		const XMMATRIX S = XMMatrixScaling(1.0f, 1.0f, -1.0f);
		return S * M * S;
	}

	// 書き出し用に組み立てた aiScene をこちらの CRT で解放する。
	// aiNode / aiScene / aiMaterial のデストラクタは assimp DLL 側 (リリース CRT) にあるので、
	// こちらの new で確保した配列を DLL の delete[] に渡さないよう、先に外して自前で解放する
	// (Debug ビルドでは CRT ヒープが異なりクラッシュする)。aiMesh / aiBone / aiAnimation /
	// aiNodeAnim のデストラクタはヘッダのインライン (こちらの CRT) なのでそのまま delete できる
	void FreeExportNode(aiNode* Node)
	{
		if (Node == nullptr) return;
		for (unsigned int i = 0; i < Node->mNumChildren; ++i)
		{
			FreeExportNode(Node->mChildren[i]);
		}
		delete[] Node->mChildren;
		Node->mChildren = nullptr;
		Node->mNumChildren = 0;
		delete[] Node->mMeshes;
		Node->mMeshes = nullptr;
		Node->mNumMeshes = 0;
		delete Node;	// DLL のデストラクタは何も解放しない (mMetaData は null)
	}

	void FreeExportScene(aiScene* Scene)
	{
		if (Scene == nullptr) return;

		FreeExportNode(Scene->mRootNode);
		Scene->mRootNode = nullptr;

		for (unsigned int i = 0; i < Scene->mNumMeshes; ++i) delete Scene->mMeshes[i];	// インラインのデストラクタ
		delete[] Scene->mMeshes;
		Scene->mMeshes = nullptr;
		Scene->mNumMeshes = 0;

		for (unsigned int i = 0; i < Scene->mNumMaterials; ++i) delete Scene->mMaterials[i];	// プロパティは DLL 側が確保 / 解放
		delete[] Scene->mMaterials;
		Scene->mMaterials = nullptr;
		Scene->mNumMaterials = 0;

		for (unsigned int i = 0; i < Scene->mNumAnimations; ++i) delete Scene->mAnimations[i];	// インラインのデストラクタ
		delete[] Scene->mAnimations;
		Scene->mAnimations = nullptr;
		Scene->mNumAnimations = 0;

		delete Scene;	// 残る mPrivate は DLL のコンストラクタが確保したもの (DLL 側で解放)
	}
}

bool FSkeletalMeshImporter::ExportSkeletalMesh(const char* FilePath, const char* FormatId, const FSkeletalMeshImportData& Data,
	const std::vector<std::shared_ptr<UAnimSequence>>& Sequences, const FImportOptions& Options)
{
	if (Data.IsEmpty())
	{
		return false;
	}

	const int numBones = Data.RefSkeleton.GetNum();

	// 逆バインド行列 (無ければリファレンスポーズの逆)
	std::vector<FTransform> componentSpaceRefPose;
	Data.RefSkeleton.ComputeComponentSpaceRefPose(componentSpaceRefPose);
	std::vector<XMMATRIX> invRef(numBones);
	for (int i = 0; i < numBones; ++i)
	{
		invRef[i] = (Data.RefBasesInvMatrix.size() == (size_t)numBones)
			? XMLoadFloat4x4(&Data.RefBasesInvMatrix[i])
			: XMMatrixInverse(nullptr, componentSpaceRefPose[i].ToMatrixWithScale());
	}

	// aiNode のコンストラクタは DLL 側にあるため std::string を渡さない (Debug CRT の std::string は
	// レイアウトが違い DLL 境界で壊れる)。既定コンストラクタ + aiString 代入で名前を付ける
	auto NewNode = [](const std::string& Name)
		{
			aiNode* node = new aiNode();
			node->mName = aiString(Name);
			return node;
		};

	aiScene* scene = new aiScene();
	scene->mRootNode = NewNode("RootNode");

	// ---- ボーンノード階層 ----
	std::vector<aiNode*> boneNodes(numBones, nullptr);
	for (int i = 0; i < numBones; ++i)
	{
		aiNode* node = NewNode(Data.RefSkeleton.GetBoneName(i));
		node->mTransformation = ToAiMatrix(MirrorZ(Data.RefSkeleton.GetRefBonePose()[i].ToMatrixWithScale()));
		boneNodes[i] = node;
	}
	// 子の数を数えてから配列を作る (親は必ず子より前)
	std::vector<std::vector<aiNode*>> children(numBones);
	std::vector<aiNode*> rootChildren;
	for (int i = 0; i < numBones; ++i)
	{
		const int parent = Data.RefSkeleton.GetParentIndex(i);
		if (parent == INDEX_NONE) rootChildren.push_back(boneNodes[i]);
		else                      children[parent].push_back(boneNodes[i]);
	}
	for (int i = 0; i < numBones; ++i)
	{
		if (children[i].empty()) continue;
		boneNodes[i]->mNumChildren = static_cast<unsigned int>(children[i].size());
		boneNodes[i]->mChildren = new aiNode * [children[i].size()];
		for (size_t c = 0; c < children[i].size(); ++c)
		{
			boneNodes[i]->mChildren[c] = children[i][c];
			children[i][c]->mParent = boneNodes[i];
		}
	}

	// ---- メッシュノード ----
	aiNode* meshNode = NewNode("ChainMesh");
	meshNode->mNumMeshes = 1;
	meshNode->mMeshes = new unsigned int[1]{ 0 };
	rootChildren.push_back(meshNode);

	scene->mRootNode->mNumChildren = static_cast<unsigned int>(rootChildren.size());
	scene->mRootNode->mChildren = new aiNode * [rootChildren.size()];
	for (size_t c = 0; c < rootChildren.size(); ++c)
	{
		scene->mRootNode->mChildren[c] = rootChildren[c];
		rootChildren[c]->mParent = scene->mRootNode;
	}

	// ---- マテリアル ----
	scene->mNumMaterials = 1;
	scene->mMaterials = new aiMaterial * [1];
	scene->mMaterials[0] = new aiMaterial();
	{
		aiString name("ChainMaterial");
		scene->mMaterials[0]->AddProperty(&name, AI_MATKEY_NAME);
	}

	// ---- メッシュ (Z 反転 / V 反転 / 巻き順反転) ----
	aiMesh* mesh = new aiMesh();
	mesh->mName = aiString("ChainMesh");
	mesh->mPrimitiveTypes = aiPrimitiveType_TRIANGLE;
	mesh->mMaterialIndex = 0;
	mesh->mNumVertices = static_cast<unsigned int>(Data.Vertices.size());
	mesh->mVertices = new aiVector3D[mesh->mNumVertices];
	mesh->mNormals = new aiVector3D[mesh->mNumVertices];
	mesh->mTangents = new aiVector3D[mesh->mNumVertices];
	mesh->mBitangents = new aiVector3D[mesh->mNumVertices];
	mesh->mTextureCoords[0] = new aiVector3D[mesh->mNumVertices];
	mesh->mNumUVComponents[0] = 2;
	for (unsigned int v = 0; v < mesh->mNumVertices; ++v)
	{
		const FSkeletalMeshImportVertex& src = Data.Vertices[v];
		mesh->mVertices[v] = aiVector3D(src.Position.x, src.Position.y, -src.Position.z);
		mesh->mNormals[v] = aiVector3D(src.Normal.x, src.Normal.y, -src.Normal.z);
		mesh->mTangents[v] = aiVector3D(src.Tangent.x, src.Tangent.y, -src.Tangent.z);
		const XMVECTOR n = XMVectorSet(src.Normal.x, src.Normal.y, -src.Normal.z, 0.0f);
		const XMVECTOR t = XMVectorSet(src.Tangent.x, src.Tangent.y, -src.Tangent.z, 0.0f);
		XMFLOAT3 b;
		XMStoreFloat3(&b, XMVector3Cross(n, t));
		mesh->mBitangents[v] = aiVector3D(b.x, b.y, b.z);
		mesh->mTextureCoords[0][v] = aiVector3D(src.TexCoord.x, Options.bFlipUV ? (1.0f - src.TexCoord.y) : src.TexCoord.y, 0.0f);
	}

	const unsigned int numTriangles = static_cast<unsigned int>(Data.Indices.size() / 3);
	mesh->mNumFaces = numTriangles;
	mesh->mFaces = new aiFace[numTriangles];
	for (unsigned int f = 0; f < numTriangles; ++f)
	{
		aiFace& face = mesh->mFaces[f];
		face.mNumIndices = 3;
		face.mIndices = new unsigned int[3];
		face.mIndices[0] = Data.Indices[f * 3 + 0];
		face.mIndices[1] = Data.Indices[f * 3 + 2];	// 巻き順反転
		face.mIndices[2] = Data.Indices[f * 3 + 1];
	}

	// ボーン (影響のあるものだけ)
	std::vector<std::vector<aiVertexWeight>> weights(numBones);
	for (unsigned int v = 0; v < mesh->mNumVertices; ++v)
	{
		FSkeletalMeshImportVertex n = Data.Vertices[v];
		n.NormalizeInfluences();
		for (unsigned int i = 0; i < MAX_TOTAL_INFLUENCES; ++i)
		{
			if (n.BoneIndices[i] == INDEX_NONE || n.BoneWeights[i] <= 0.0f || n.BoneIndices[i] >= numBones) continue;
			weights[n.BoneIndices[i]].push_back(aiVertexWeight(v, n.BoneWeights[i]));
		}
	}
	std::vector<aiBone*> bones;
	for (int i = 0; i < numBones; ++i)
	{
		if (weights[i].empty()) continue;
		aiBone* bone = new aiBone();
		bone->mName = aiString(Data.RefSkeleton.GetBoneName(i));
		bone->mNumWeights = static_cast<unsigned int>(weights[i].size());
		bone->mWeights = new aiVertexWeight[weights[i].size()];
		for (size_t w = 0; w < weights[i].size(); ++w) bone->mWeights[w] = weights[i][w];
		bone->mOffsetMatrix = ToAiMatrix(MirrorZ(invRef[i]));
		bones.push_back(bone);
	}
	mesh->mNumBones = static_cast<unsigned int>(bones.size());
	mesh->mBones = new aiBone * [bones.size()];
	for (size_t b = 0; b < bones.size(); ++b) mesh->mBones[b] = bones[b];

	scene->mNumMeshes = 1;
	scene->mMeshes = new aiMesh * [1];
	scene->mMeshes[0] = mesh;

	// ---- アニメーション ----
	if (!Sequences.empty())
	{
		scene->mNumAnimations = static_cast<unsigned int>(Sequences.size());
		scene->mAnimations = new aiAnimation * [Sequences.size()];
		for (size_t a = 0; a < Sequences.size(); ++a)
		{
			const UAnimSequence& seq = *Sequences[a];
			const double tps = (std::max)(1.0, static_cast<double>(seq.GetSamplingFrameRate().AsDecimal()));
			const double length = seq.GetPlayLength();

			aiAnimation* anim = new aiAnimation();
			anim->mName = aiString(seq.GetName());
			anim->mTicksPerSecond = tps;
			anim->mDuration = length * tps;

			const std::vector<FRawAnimSequenceTrack>& tracks = seq.GetRawAnimationData();
			const std::vector<FTrackToSkeletonMap>& map = seq.GetTrackToSkeletonMapTable();
			anim->mNumChannels = static_cast<unsigned int>(tracks.size());
			anim->mChannels = new aiNodeAnim * [tracks.size()];

			for (size_t t = 0; t < tracks.size(); ++t)
			{
				const FRawAnimSequenceTrack& track = tracks[t];
				const int boneIndex = map[t].BoneTreeIndex;

				aiNodeAnim* channel = new aiNodeAnim();
				channel->mNodeName = aiString(Data.RefSkeleton.GetBoneName(boneIndex));

				auto keyTime = [&](size_t k, size_t n) { return (n > 1) ? (static_cast<double>(k) / static_cast<double>(n - 1)) * length * tps : 0.0; };

				const std::vector<XMFLOAT3>& pos = track.PosKeys.empty() ? std::vector<XMFLOAT3>{ Data.RefSkeleton.GetRefBonePose()[boneIndex].Translation } : track.PosKeys;
				channel->mNumPositionKeys = static_cast<unsigned int>(pos.size());
				channel->mPositionKeys = new aiVectorKey[pos.size()];
				for (size_t k = 0; k < pos.size(); ++k)
				{
					channel->mPositionKeys[k].mTime = keyTime(k, pos.size());
					channel->mPositionKeys[k].mValue = aiVector3D(pos[k].x, pos[k].y, -pos[k].z);
				}

				const std::vector<XMFLOAT4>& rot = track.RotKeys.empty() ? std::vector<XMFLOAT4>{ Data.RefSkeleton.GetRefBonePose()[boneIndex].Rotation } : track.RotKeys;
				channel->mNumRotationKeys = static_cast<unsigned int>(rot.size());
				channel->mRotationKeys = new aiQuatKey[rot.size()];
				for (size_t k = 0; k < rot.size(); ++k)
				{
					channel->mRotationKeys[k].mTime = keyTime(k, rot.size());
					// 鏡映: (x, y, z, w) -> (-x, -y, z, w)。aiQuaternion のコンストラクタは (w, x, y, z)
					channel->mRotationKeys[k].mValue = aiQuaternion(rot[k].w, -rot[k].x, -rot[k].y, rot[k].z);
				}

				const std::vector<XMFLOAT3>& scale = track.ScaleKeys.empty() ? std::vector<XMFLOAT3>{ Data.RefSkeleton.GetRefBonePose()[boneIndex].Scale3D } : track.ScaleKeys;
				channel->mNumScalingKeys = static_cast<unsigned int>(scale.size());
				channel->mScalingKeys = new aiVectorKey[scale.size()];
				for (size_t k = 0; k < scale.size(); ++k)
				{
					channel->mScalingKeys[k].mTime = keyTime(k, scale.size());
					channel->mScalingKeys[k].mValue = aiVector3D(scale[k].x, scale[k].y, scale[k].z);
				}

				anim->mChannels[t] = channel;
			}

			scene->mAnimations[a] = anim;
		}
	}

	// ---- 書き出し ----
	Assimp::Exporter exporter;
	const aiReturn result = exporter.Export(scene, FormatId, FilePath, 0);
	if (result != aiReturn_SUCCESS)
	{
		LogImport("[SkeletalMeshImporter] export failed: %s\n  file: %s\n", exporter.GetErrorString(), FilePath);
	}
	else
	{
		LogImport("[SkeletalMeshImporter] exported %s (%s)\n", FilePath, FormatId);
	}

	FreeExportScene(scene);
	return result == aiReturn_SUCCESS;
}
