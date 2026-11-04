#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <cassert>
#include "Transform.h"

// ============================================================
//  FReferenceSkeleton
//  FReferenceSkeleton (ReferenceSkeleton.h) に相当する
//  ボーンツリー + リファレンスポーズ。USkeleton が所有し、
//  USkeletalMesh / UAnimSequence / FBoneContainer が参照する。
//
//  規約 (UE と同じ):
//    - RawRefBoneInfo[i].ParentIndex < i (親は必ず子より前。
//      FillComponentSpaceTransforms が先頭から 1 パスで計算できる)
//    - ルートボーンは 1 つ (ParentIndex = -1 は添字 0 だけ)
//    - RawRefBonePose[i] は親ボーン空間の FTransform
//    - 本実装は LOD / 仮想ボーンを持たないので Raw = Final
//      (コンパクトポーズの添字 = スケルトンのボーン添字)
// ============================================================

constexpr int INDEX_NONE = -1;

struct FMeshBoneInfo
{
	std::string Name;
	int         ParentIndex = INDEX_NONE;

	FMeshBoneInfo() = default;
	FMeshBoneInfo(const std::string& InName, int InParentIndex)
		: Name(InName)
		, ParentIndex(InParentIndex)
	{
	}
};

class FReferenceSkeleton
{
private:
	std::vector<FMeshBoneInfo>           m_RefBoneInfo;
	std::vector<FTransform>              m_RefBonePose;		// 親ボーン空間
	std::unordered_map<std::string, int> m_NameToIndexMap;

public:
	void Empty()
	{
		m_RefBoneInfo.clear();
		m_RefBonePose.clear();
		m_NameToIndexMap.clear();
	}

	// ボーンを末尾に追加する (FReferenceSkeletonModifier::Add)。
	// 親は既に追加済みであること (ParentIndex < 新しい添字)。
	// 同名のボーンがあれば追加せず既存の添字を返す
	int Add(const FMeshBoneInfo& BoneInfo, const FTransform& BonePose)
	{
		auto it = m_NameToIndexMap.find(BoneInfo.Name);
		if (it != m_NameToIndexMap.end())
		{
			return it->second;
		}

		const int index = static_cast<int>(m_RefBoneInfo.size());
		assert(BoneInfo.ParentIndex < index && "parent bones must be added before their children");
		assert((index == 0) == (BoneInfo.ParentIndex == INDEX_NONE) && "exactly one root bone (index 0)");

		m_RefBoneInfo.push_back(BoneInfo);
		m_RefBonePose.push_back(BonePose);
		m_NameToIndexMap.emplace(BoneInfo.Name, index);
		return index;
	}

	int GetNum() const { return static_cast<int>(m_RefBoneInfo.size()); }
	bool IsValidIndex(int BoneIndex) const { return BoneIndex >= 0 && BoneIndex < GetNum(); }

	int FindBoneIndex(const std::string& BoneName) const
	{
		auto it = m_NameToIndexMap.find(BoneName);
		return (it != m_NameToIndexMap.end()) ? it->second : INDEX_NONE;
	}

	int GetParentIndex(int BoneIndex) const
	{
		return IsValidIndex(BoneIndex) ? m_RefBoneInfo[BoneIndex].ParentIndex : INDEX_NONE;
	}

	const std::string& GetBoneName(int BoneIndex) const
	{
		static const std::string none;
		return IsValidIndex(BoneIndex) ? m_RefBoneInfo[BoneIndex].Name : none;
	}

	const std::vector<FMeshBoneInfo>& GetRefBoneInfo() const { return m_RefBoneInfo; }
	const std::vector<FTransform>&    GetRefBonePose() const { return m_RefBonePose; }

	// ChildBoneIndex が ParentBoneIndex の子孫か (FReferenceSkeleton::BoneIsChildOf)
	bool BoneIsChildOf(int ChildBoneIndex, int ParentBoneIndex) const
	{
		if (!IsValidIndex(ChildBoneIndex) || !IsValidIndex(ParentBoneIndex)) return false;
		for (int bone = GetParentIndex(ChildBoneIndex); bone != INDEX_NONE; bone = GetParentIndex(bone))
		{
			if (bone == ParentBoneIndex) return true;
		}
		return false;
	}

	// コンポーネント空間のリファレンスポーズ (親から順に合成)
	void ComputeComponentSpaceRefPose(std::vector<FTransform>& OutComponentSpace) const
	{
		const int num = GetNum();
		OutComponentSpace.resize(num);
		for (int i = 0; i < num; ++i)
		{
			const int parent = m_RefBoneInfo[i].ParentIndex;
			OutComponentSpace[i] = (parent == INDEX_NONE)
				? m_RefBonePose[i]
				: m_RefBonePose[i] * OutComponentSpace[parent];
		}
	}

	// 2 つのスケルトンが同じボーン列か (名前と親の一致)
	bool IsSameSkeleton(const FReferenceSkeleton& Other) const
	{
		if (GetNum() != Other.GetNum()) return false;
		for (int i = 0; i < GetNum(); ++i)
		{
			if (m_RefBoneInfo[i].Name != Other.m_RefBoneInfo[i].Name ||
				m_RefBoneInfo[i].ParentIndex != Other.m_RefBoneInfo[i].ParentIndex)
			{
				return false;
			}
		}
		return true;
	}
};
