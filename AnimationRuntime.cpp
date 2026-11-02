#include "Main.h"
#include "AnimationRuntime.h"

#include <cmath>

namespace FAnimationRuntime
{
	void BlendTwoPosesTogether(const FCompactPose& SourcePose1, const FCompactPose& SourcePose2,
		const FBlendedCurve& SourceCurve1, const FBlendedCurve& SourceCurve2,
		float WeightOfPose1, FCompactPose& OutPose, FBlendedCurve& OutCurve)
	{
		// UE: BlendPose<ETransformBlendMode::Overwrite>(Pose1, OutPose, Weight1) +
		//     BlendPose<ETransformBlendMode::Accumulate>(Pose2, OutPose, 1 - Weight1) + NormalizeRotations。
		// 本実装は FTransform::Blend (最短経路 nlerp) で同じ結果になる
		const float alpha = 1.0f - WeightOfPose1;	// 0 = Pose1, 1 = Pose2
		const int num = (std::min)(SourcePose1.GetNumBones(), SourcePose2.GetNumBones());

		OutPose.BoneContainer = SourcePose1.BoneContainer;
		OutPose.Bones.resize(SourcePose1.GetNumBones());
		for (int i = 0; i < num; ++i)
		{
			OutPose.Bones[i] = FTransform::Blend(SourcePose1[i], SourcePose2[i], alpha);
		}
		for (int i = num; i < SourcePose1.GetNumBones(); ++i)
		{
			OutPose.Bones[i] = SourcePose1[i];
		}

		OutCurve.Blend(SourceCurve1, SourceCurve2, alpha);
	}

	void FillUpComponentSpaceTransforms(const FReferenceSkeleton& RefSkeleton,
		const std::vector<FTransform>& BoneSpaceTransforms, std::vector<FTransform>& OutComponentSpaceTransforms)
	{
		const int num = (std::min)(RefSkeleton.GetNum(), static_cast<int>(BoneSpaceTransforms.size()));
		OutComponentSpaceTransforms.resize(num);

		for (int i = 0; i < num; ++i)
		{
			const int parent = RefSkeleton.GetParentIndex(i);
			if (parent == INDEX_NONE)
			{
				OutComponentSpaceTransforms[i] = BoneSpaceTransforms[i];
			}
			else
			{
				assert(parent < i && "parent bones must precede their children");
				OutComponentSpaceTransforms[i] = BoneSpaceTransforms[i] * OutComponentSpaceTransforms[parent];
			}
			OutComponentSpaceTransforms[i].NormalizeRotation();
		}
	}

	void GetKeyIndicesFromTime(int& OutKeyIndex1, int& OutKeyIndex2, float& OutAlpha,
		float Time, int NumKeys, float SequenceLength)
	{
		// UE: FAnimationRuntime::GetKeyIndicesFromTime
		if (NumKeys < 2)
		{
			OutKeyIndex1 = 0;
			OutKeyIndex2 = 0;
			OutAlpha = 0.0f;
			return;
		}

		if (NumKeys < 3 || Time <= 0.0f || SequenceLength <= 0.0f)
		{
			OutKeyIndex1 = 0;
			OutKeyIndex2 = 1;
			OutAlpha = (SequenceLength > 0.0f) ? (std::max)(0.0f, (std::min)(1.0f, Time / SequenceLength)) : 0.0f;
			return;
		}

		if (Time >= SequenceLength)
		{
			OutKeyIndex1 = NumKeys - 1;
			OutKeyIndex2 = NumKeys - 1;
			OutAlpha = 0.0f;
			return;
		}

		const int   lastIndex = NumKeys - 1;
		const float keyPos = Time / SequenceLength * static_cast<float>(lastIndex);
		const int   keyIndex1 = (std::max)(0, (std::min)(lastIndex, static_cast<int>(keyPos)));
		const float alpha = keyPos - static_cast<float>(keyIndex1);

		OutKeyIndex1 = keyIndex1;
		OutKeyIndex2 = (std::min)(keyIndex1 + 1, lastIndex);
		OutAlpha = (OutKeyIndex2 == keyIndex1) ? 0.0f : alpha;
	}

	ETypeAdvanceAnim AdvanceTime(bool bAllowLooping, float MoveDelta, float& InOutTime, float EndTime)
	{
		// UE: FAnimationRuntime::AdvanceTime
		InOutTime += MoveDelta;

		if (InOutTime < 0.0f || InOutTime > EndTime)
		{
			if (bAllowLooping)
			{
				if (EndTime != 0.0f)
				{
					InOutTime = std::fmod(InOutTime, EndTime);
					if (InOutTime < 0.0f)
					{
						InOutTime += EndTime;
					}
				}
				else
				{
					InOutTime = 0.0f;
				}
				return ETypeAdvanceAnim::ETAA_Looped;
			}
			else
			{
				InOutTime = (std::max)(0.0f, (std::min)(InOutTime, EndTime));
				return ETypeAdvanceAnim::ETAA_Finished;
			}
		}

		return ETypeAdvanceAnim::ETAA_Default;
	}
}
