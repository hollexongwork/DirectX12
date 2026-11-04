#pragma once
#include <vector>
#include <string>
#include <unordered_map>
#include "ReferenceSkeleton.h"

// ============================================================
//  AnimationRuntime
//  FBoneContainer / FCompactPose / FBlendedCurve (BonePose.h /
//  BoneContainer.h) と FAnimationRuntime (AnimationRuntime.h) の
//  本エンジン版。アニメーションノードはこれらの型でポーズを
//  受け渡し、USkeletalMeshComponent が最後にコンポーネント空間へ
//  変換する。
//
//  本実装は LOD / 仮想ボーンを持たないため、コンパクトポーズの
//  添字 = スケルトンのボーン添字 (FCompactPoseBoneIndex ==
//  FMeshPoseBoneIndex == FSkeletonPoseBoneIndex)。
// ============================================================

// ------------------------------------------------------------
//  FBoneContainer
//  評価に必要なボーン集合 (全ボーン) とリファレンススケルトンへの参照。
//  UAnimInstance::RequiredBones が保持し、FCompactPose が参照する
// ------------------------------------------------------------
class FBoneContainer
{
private:
	const FReferenceSkeleton* m_RefSkeleton = nullptr;

public:
	void InitializeTo(const FReferenceSkeleton& RefSkeleton) { m_RefSkeleton = &RefSkeleton; }
	void Reset() { m_RefSkeleton = nullptr; }

	bool IsValid() const { return m_RefSkeleton != nullptr && m_RefSkeleton->GetNum() > 0; }
	int  GetNumBones() const { return m_RefSkeleton ? m_RefSkeleton->GetNum() : 0; }

	const FReferenceSkeleton& GetReferenceSkeleton() const { return *m_RefSkeleton; }
	int GetParentBoneIndex(int BoneIndex) const { return m_RefSkeleton ? m_RefSkeleton->GetParentIndex(BoneIndex) : INDEX_NONE; }
	const FTransform& GetRefPoseTransform(int BoneIndex) const { return m_RefSkeleton->GetRefBonePose()[BoneIndex]; }
	const std::vector<FTransform>& GetRefPoseArray() const { return m_RefSkeleton->GetRefBonePose(); }
};

// ------------------------------------------------------------
//  FCompactPose
//  ボーン空間 (親相対) の FTransform 列。FBoneContainer に従う
// ------------------------------------------------------------
struct FCompactPose
{
	std::vector<FTransform> Bones;
	const FBoneContainer*   BoneContainer = nullptr;

	void SetBoneContainer(const FBoneContainer* InBoneContainer)
	{
		BoneContainer = InBoneContainer;
		Bones.resize(InBoneContainer ? InBoneContainer->GetNumBones() : 0);
	}

	bool IsValid() const { return BoneContainer != nullptr && BoneContainer->IsValid() && Bones.size() == (size_t)BoneContainer->GetNumBones(); }
	int  GetNumBones() const { return static_cast<int>(Bones.size()); }

	FTransform&       operator[](int BoneIndex) { return Bones[BoneIndex]; }
	const FTransform& operator[](int BoneIndex) const { return Bones[BoneIndex]; }

	const FBoneContainer& GetBoneContainer() const { return *BoneContainer; }

	// リファレンスポーズで初期化 (FCompactPose::ResetToRefPose)
	void ResetToRefPose()
	{
		if (BoneContainer == nullptr) return;
		const std::vector<FTransform>& refPose = BoneContainer->GetRefPoseArray();
		Bones.assign(refPose.begin(), refPose.end());
	}

	// 加算ポーズの恒等値 (平行移動 0 / 回転 単位 / スケール 0) で初期化
	void ResetToAdditiveIdentity()
	{
		for (FTransform& bone : Bones)
		{
			bone.SetIdentity();
			bone.Scale3D = { 0.0f, 0.0f, 0.0f };
		}
	}

	void CopyBonesFrom(const FCompactPose& Other)
	{
		BoneContainer = Other.BoneContainer;
		Bones = Other.Bones;
	}

	void NormalizeRotations()
	{
		for (FTransform& bone : Bones)
		{
			bone.NormalizeRotation();
		}
	}
};

// ------------------------------------------------------------
//  FBlendedCurve
//  アニメーションカーブ (名前 -> 値) の最小実装。本実装は
//  UAnimSequence にカーブデータを持たないため、ブレンドの
//  インタフェース (FPoseContext::Curve) を UE と同じ形で残すためのもの
// ------------------------------------------------------------
struct FBlendedCurve
{
	std::unordered_map<std::string, float> Curves;

	void Reset() { Curves.clear(); }
	void Set(const std::string& Name, float Value) { Curves[Name] = Value; }
	float Get(const std::string& Name) const
	{
		auto it = Curves.find(Name);
		return (it != Curves.end()) ? it->second : 0.0f;
	}

	// this = A * (1 - Alpha) + B * Alpha (無いカーブは 0 扱い)
	void Blend(const FBlendedCurve& A, const FBlendedCurve& B, float Alpha)
	{
		Curves.clear();
		for (const auto& kv : A.Curves) Curves[kv.first] = kv.second * (1.0f - Alpha);
		for (const auto& kv : B.Curves) Curves[kv.first] += kv.second * Alpha;
	}

	void CopyFrom(const FBlendedCurve& Other) { Curves = Other.Curves; }
};

// ------------------------------------------------------------
//  FAnimExtractContext
//  UAnimSequence::GetAnimationPose へ渡す抽出パラメータ
// ------------------------------------------------------------
struct FAnimExtractContext
{
	float CurrentTime = 0.0f;
	bool  bLooping = false;
	bool  bExtractRootMotion = false;	// 予約 (ルートモーションは未対応)

	FAnimExtractContext() = default;
	FAnimExtractContext(float InCurrentTime, bool bInLooping)
		: CurrentTime(InCurrentTime)
		, bLooping(bInLooping)
	{
	}
};

// ------------------------------------------------------------
//  FAnimationPoseData
//  ポーズ + カーブの出力束 (FAnimationPoseData)
// ------------------------------------------------------------
struct FAnimationPoseData
{
	FCompactPose&  Pose;
	FBlendedCurve& Curve;

	FAnimationPoseData(FCompactPose& InPose, FBlendedCurve& InCurve)
		: Pose(InPose)
		, Curve(InCurve)
	{
	}
};

// ------------------------------------------------------------
//  ETypeAdvanceAnim (FAnimationRuntime::AdvanceTime の戻り値)
// ------------------------------------------------------------
enum class ETypeAdvanceAnim
{
	ETAA_Default,	// 通常前進
	ETAA_Finished,	// 非ループで終端に達した
	ETAA_Looped,	// ループして先頭へ戻った
};

// ------------------------------------------------------------
//  FAnimationRuntime
// ------------------------------------------------------------
namespace FAnimationRuntime
{
	// 2 つのポーズをブレンドする (BlendTwoPosesTogether)。
	// WeightOfPose1 = 1 で SourcePose1、0 で SourcePose2
	void BlendTwoPosesTogether(const FCompactPose& SourcePose1, const FCompactPose& SourcePose2,
		const FBlendedCurve& SourceCurve1, const FBlendedCurve& SourceCurve2,
		float WeightOfPose1, FCompactPose& OutPose, FBlendedCurve& OutCurve);

	// ボーン空間 -> コンポーネント空間 (FillUpComponentSpaceTransforms)
	void FillUpComponentSpaceTransforms(const FReferenceSkeleton& RefSkeleton,
		const std::vector<FTransform>& BoneSpaceTransforms, std::vector<FTransform>& OutComponentSpaceTransforms);

	// 時間 -> キー添字と補間率 (GetKeyIndicesFromTime)。
	// NumKeys 個のキーが [0, SequenceLength] に等間隔 (最後のキー = SequenceLength)
	void GetKeyIndicesFromTime(int& OutKeyIndex1, int& OutKeyIndex2, float& OutAlpha,
		float Time, int NumKeys, float SequenceLength);

	// 再生時間を進める (AdvanceTime)。ループ時は [0, EndTime) に巻き戻す
	ETypeAdvanceAnim AdvanceTime(bool bAllowLooping, float MoveDelta, float& InOutTime, float EndTime);
}
