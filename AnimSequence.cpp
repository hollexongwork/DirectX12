#include "Main.h"
#include "AnimSequence.h"

// ============================================================
//  UAnimSequenceBase
// ============================================================

void UAnimSequenceBase::GetAnimationPose(FAnimationPoseData& OutPoseData, const FAnimExtractContext& ExtractionContext) const
{
	(void)ExtractionContext;
	OutPoseData.Pose.ResetToRefPose();
	OutPoseData.Curve.Reset();
}

// ============================================================
//  UAnimSequence
// ============================================================

bool UAnimSequence::AddTrack(int BoneIndex, const FRawAnimSequenceTrack& Track)
{
	if (BoneIndex < 0) return false;
	if (m_Skeleton && !m_Skeleton->GetReferenceSkeleton().IsValidIndex(BoneIndex)) return false;

	const int existing = GetTrackIndexForBone(BoneIndex);
	if (existing != INDEX_NONE)
	{
		m_RawAnimationData[existing] = Track;
		return true;
	}

	m_RawAnimationData.push_back(Track);
	FTrackToSkeletonMap map;
	map.BoneTreeIndex = BoneIndex;
	m_TrackToSkeletonMapTable.push_back(map);
	return true;
}

int UAnimSequence::GetTrackIndexForBone(int BoneIndex) const
{
	for (size_t i = 0; i < m_TrackToSkeletonMapTable.size(); ++i)
	{
		if (m_TrackToSkeletonMapTable[i].BoneTreeIndex == BoneIndex)
		{
			return static_cast<int>(i);
		}
	}
	return INDEX_NONE;
}

void UAnimSequence::GetAnimationPose(FAnimationPoseData& OutPoseData, const FAnimExtractContext& ExtractionContext) const
{
	GetBonePose(OutPoseData.Pose, OutPoseData.Curve, ExtractionContext);
}

void UAnimSequence::GetBonePose(FCompactPose& OutPose, FBlendedCurve& OutCurve, const FAnimExtractContext& ExtractionContext) const
{
	// UE: UAnimSequence::GetBonePose -> リファレンスポーズで初期化し、
	// トラックを持つボーンだけ BuildPoseFromRawData で上書きする
	OutPose.ResetToRefPose();
	OutCurve.Reset();

	if (!OutPose.IsValid()) return;

	const int numBones = OutPose.GetNumBones();
	for (size_t track = 0; track < m_RawAnimationData.size(); ++track)
	{
		const int boneIndex = m_TrackToSkeletonMapTable[track].BoneTreeIndex;
		if (boneIndex < 0 || boneIndex >= numBones) continue;

		ExtractBoneTransform(m_RawAnimationData[track], OutPose[boneIndex], ExtractionContext.CurrentTime);
	}
}

void UAnimSequence::ExtractBoneTransform(const FRawAnimSequenceTrack& Track, FTransform& OutAtom, float Time) const
{
	// 各キー列ごとに時刻 -> (Key1, Key2, Alpha) を引く (列ごとにキー数が異なってもよい)
	auto Sample3 = [&](const std::vector<XMFLOAT3>& Keys, const XMFLOAT3& Default) -> XMVECTOR
		{
			if (Keys.empty()) return XMLoadFloat3(&Default);
			if (Keys.size() == 1) return XMLoadFloat3(&Keys[0]);

			int k1 = 0, k2 = 0;
			float alpha = 0.0f;
			FAnimationRuntime::GetKeyIndicesFromTime(k1, k2, alpha, Time, static_cast<int>(Keys.size()), m_SequenceLength);
			return XMVectorLerp(XMLoadFloat3(&Keys[k1]), XMLoadFloat3(&Keys[k2]), alpha);
		};

	auto SampleRot = [&](const std::vector<XMFLOAT4>& Keys) -> XMVECTOR
		{
			if (Keys.empty()) return XMQuaternionIdentity();
			if (Keys.size() == 1) return XMQuaternionNormalize(XMLoadFloat4(&Keys[0]));

			int k1 = 0, k2 = 0;
			float alpha = 0.0f;
			FAnimationRuntime::GetKeyIndicesFromTime(k1, k2, alpha, Time, static_cast<int>(Keys.size()), m_SequenceLength);
			const XMVECTOR q1 = XMLoadFloat4(&Keys[k1]);
			const XMVECTOR q2 = XMLoadFloat4(&Keys[k2]);
			if (alpha <= 0.0f) return XMQuaternionNormalize(q1);
			return XMQuaternionNormalize(XMQuaternionSlerp(q1, q2, alpha));
		};

	static const XMFLOAT3 kZero = { 0.0f, 0.0f, 0.0f };
	static const XMFLOAT3 kOne = { 1.0f, 1.0f, 1.0f };

	OutAtom.SetTranslation(Sample3(Track.PosKeys, kZero));
	OutAtom.SetRotation(SampleRot(Track.RotKeys));
	OutAtom.SetScale3D(Sample3(Track.ScaleKeys, kOne));
}
