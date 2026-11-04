#include "Main.h"
#include "AnimNodes.h"

// ============================================================
//  FAnimNode_SequencePlayer
// ============================================================

void FAnimNode_SequencePlayer::Initialize_AnyThread(const FAnimationInitializeContext& Context)
{
	(void)Context;
	SetAccumulatedTime(StartPosition);
	bFinished = false;
}

void FAnimNode_SequencePlayer::SetAccumulatedTime(float Time)
{
	const float length = GetCurrentAssetLength();
	InternalTimeAccumulator = (length > 0.0f) ? (std::max)(0.0f, (std::min)(Time, length)) : 0.0f;
}

void FAnimNode_SequencePlayer::Update_AnyThread(const FAnimationUpdateContext& Context)
{
	if (!Sequence) return;

	const float length = Sequence->GetPlayLength();
	if (length <= 0.0f)
	{
		InternalTimeAccumulator = 0.0f;
		return;
	}

	// UE: FAnimNode_SequencePlayer::UpdateAssetPlayer -> CreateTickRecordForNode ->
	//     FAnimationRuntime::AdvanceTime(bLoop, PlayRate * RateScale * DeltaTime, ...)
	const float moveDelta = GetEffectivePlayRate() * Context.GetDeltaTime();
	const ETypeAdvanceAnim result = FAnimationRuntime::AdvanceTime(bLoopAnimation, moveDelta, InternalTimeAccumulator, length);
	bFinished = (result == ETypeAdvanceAnim::ETAA_Finished);
}

void FAnimNode_SequencePlayer::Evaluate_AnyThread(FPoseContext& Output)
{
	if (Sequence && Output.IsValid())
	{
		FAnimationPoseData poseData(Output.Pose, Output.Curve);
		Sequence->GetAnimationPose(poseData, FAnimExtractContext(InternalTimeAccumulator, bLoopAnimation));
	}
	else
	{
		Output.ResetToRefPose();
	}
}

// ============================================================
//  FAnimNode_SequenceEvaluator
// ============================================================

void FAnimNode_SequenceEvaluator::Evaluate_AnyThread(FPoseContext& Output)
{
	if (Sequence && Output.IsValid())
	{
		float time = ExplicitTime;
		const float length = Sequence->GetPlayLength();
		if (bShouldLoop && length > 0.0f)
		{
			time = std::fmod(time, length);
			if (time < 0.0f) time += length;
		}
		else
		{
			time = (std::max)(0.0f, (std::min)(time, length));
		}

		FAnimationPoseData poseData(Output.Pose, Output.Curve);
		Sequence->GetAnimationPose(poseData, FAnimExtractContext(time, bShouldLoop));
	}
	else
	{
		Output.ResetToRefPose();
	}
}

// ============================================================
//  FAnimNode_TwoWayBlend
// ============================================================

void FAnimNode_TwoWayBlend::Initialize_AnyThread(const FAnimationInitializeContext& Context)
{
	A.Initialize(Context);
	B.Initialize(Context);
}

void FAnimNode_TwoWayBlend::Update_AnyThread(const FAnimationUpdateContext& Context)
{
	const float alpha = (std::max)(0.0f, (std::min)(1.0f, Alpha));

	// UE は片側のウェイトが 0 なら、その側の更新を省く (bAlwaysUpdateChildren = false)
	if (alpha < 1.0f)
	{
		A.Update(Context.FractionalWeight(1.0f - alpha));
	}
	if (alpha > 0.0f)
	{
		B.Update(Context.FractionalWeight(alpha));
	}
}

void FAnimNode_TwoWayBlend::Evaluate_AnyThread(FPoseContext& Output)
{
	const float alpha = (std::max)(0.0f, (std::min)(1.0f, Alpha));

	if (alpha <= 0.0f)
	{
		A.Evaluate(Output);
		return;
	}
	if (alpha >= 1.0f)
	{
		B.Evaluate(Output);
		return;
	}

	FPoseContext poseA(Output, true);
	FPoseContext poseB(Output, true);
	A.Evaluate(poseA);
	B.Evaluate(poseB);

	FAnimationRuntime::BlendTwoPosesTogether(poseA.Pose, poseB.Pose, poseA.Curve, poseB.Curve,
		1.0f - alpha, Output.Pose, Output.Curve);
}
