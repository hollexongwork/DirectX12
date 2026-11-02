#pragma once
#include <string>
#include "ReferenceSkeleton.h"

// ============================================================
//  USkeleton
//  USkeleton (Animation/Skeleton.h) に相当。ボーンツリーと
//  リファレンスポーズ (FReferenceSkeleton) を所有し、
//  USkeletalMesh と UAnimSequence がこれを共有する
//  (同じ USkeleton を指すメッシュとアニメーションだけが組になる =
//  USkeleton::IsCompatibleMesh / IsCompatibleForEditor)。
//  本実装はスロット / カーブ名 / ソケット / 仮想ボーンを持たない。
// ============================================================

class USkeleton
{
private:
	std::string        m_Name;
	FReferenceSkeleton m_ReferenceSkeleton;

public:
	USkeleton() = default;
	explicit USkeleton(const std::string& Name, const FReferenceSkeleton& RefSkeleton)
		: m_Name(Name)
		, m_ReferenceSkeleton(RefSkeleton)
	{
	}

	const std::string& GetName() const { return m_Name; }
	void SetName(const std::string& Name) { m_Name = Name; }

	const FReferenceSkeleton& GetReferenceSkeleton() const { return m_ReferenceSkeleton; }
	void SetReferenceSkeleton(const FReferenceSkeleton& RefSkeleton) { m_ReferenceSkeleton = RefSkeleton; }

	int GetNumBones() const { return m_ReferenceSkeleton.GetNum(); }

	// USkeleton::GetReferenceSkeleton().FindBoneIndex の短縮
	int GetBoneIndex(const std::string& BoneName) const { return m_ReferenceSkeleton.FindBoneIndex(BoneName); }

	// 同じボーン列を持つスケルトンか (USkeleton::IsCompatibleMesh のボーン検査部分)
	bool IsCompatible(const USkeleton* Other) const;
	bool IsCompatible(const FReferenceSkeleton& Other) const;
};
