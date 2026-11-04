#pragma once
#include <memory>
#include "AnimNodeBase.h"
#include "AnimSequence.h"

// ============================================================
//  標準アニメーションノード
//    FAnimNode_SequencePlayer    : シーケンスを時間で再生 (ループ / 速度)
//    FAnimNode_SequenceEvaluator : 明示した時刻のポーズを評価
//    FAnimNode_TwoWayBlend       : 2 入力を Alpha でブレンド
//    FAnimNode_Root              : グラフの出力 (Result)
//  (AnimNodes/AnimNode_SequencePlayer.h / AnimNode_SequenceEvaluator.h /
//   AnimNode_TwoWayBlend.h / AnimNode_Root.h)
// ============================================================

// ------------------------------------------------------------
//  FAnimNode_SequencePlayer
// ------------------------------------------------------------
struct FAnimNode_SequencePlayer : public FAnimNode_Base
{
	std::shared_ptr<UAnimSequenceBase> Sequence;
	float PlayRate = 1.0f;
	float StartPosition = 0.0f;		// 初期化時の再生位置 [s]
	bool  bLoopAnimation = true;

	// 再生位置 [s] (FAnimNode_AssetPlayerBase::InternalTimeAccumulator)
	float InternalTimeAccumulator = 0.0f;
	// 直近の Update で終端に達したか (非ループ)
	bool  bFinished = false;

	void Initialize_AnyThread(const FAnimationInitializeContext& Context) override;
	void Update_AnyThread(const FAnimationUpdateContext& Context) override;
	void Evaluate_AnyThread(FPoseContext& Output) override;

	float GetCurrentAssetLength() const { return Sequence ? Sequence->GetPlayLength() : 0.0f; }
	float GetAccumulatedTime() const { return InternalTimeAccumulator; }
	void  SetAccumulatedTime(float Time);
	float GetEffectivePlayRate() const { return PlayRate * (Sequence ? Sequence->RateScale : 1.0f); }
};

// ------------------------------------------------------------
//  FAnimNode_SequenceEvaluator
// ------------------------------------------------------------
struct FAnimNode_SequenceEvaluator : public FAnimNode_Base
{
	std::shared_ptr<UAnimSequenceBase> Sequence;
	float ExplicitTime = 0.0f;
	bool  bShouldLoop = true;

	void Evaluate_AnyThread(FPoseContext& Output) override;
};

// ------------------------------------------------------------
//  FAnimNode_TwoWayBlend
//  Alpha = 0 で A、1 で B
// ------------------------------------------------------------
struct FAnimNode_TwoWayBlend : public FAnimNode_Base
{
	FPoseLink A;
	FPoseLink B;
	float     Alpha = 0.0f;

	void Initialize_AnyThread(const FAnimationInitializeContext& Context) override;
	void Update_AnyThread(const FAnimationUpdateContext& Context) override;
	void Evaluate_AnyThread(FPoseContext& Output) override;
};

// ------------------------------------------------------------
//  FAnimNode_Root
// ------------------------------------------------------------
struct FAnimNode_Root : public FAnimNode_Base
{
	FPoseLink Result;

	void Initialize_AnyThread(const FAnimationInitializeContext& Context) override { Result.Initialize(Context); }
	void Update_AnyThread(const FAnimationUpdateContext& Context) override { Result.Update(Context); }
	void Evaluate_AnyThread(FPoseContext& Output) override { Result.Evaluate(Output); }
};
