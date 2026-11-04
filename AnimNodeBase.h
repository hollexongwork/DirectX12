#pragma once
#include "AnimationRuntime.h"

// ============================================================
//  AnimNodeBase
//  FAnimNode_Base / FPoseLink / FAnimationBaseContext 系
//  (Animation/AnimNodeBase.h) に相当。アニメーショングラフの
//  ノードは Initialize_AnyThread / Update_AnyThread /
//  Evaluate_AnyThread の 3 段で動き、UAnimInstance のルートノード
//  から FPoseLink を辿って子へ伝搬する。
//
//  コンテキスト:
//    FAnimationBaseContext      : UAnimInstance + FBoneContainer
//    FAnimationInitializeContext: 初期化
//    FAnimationUpdateContext    : DeltaTime + 累積ウェイト
//    FPoseContext               : 評価の出力 (Pose + Curve)
// ============================================================

class UAnimInstance;
struct FAnimNode_Base;

struct FAnimationBaseContext
{
	UAnimInstance*        AnimInstance = nullptr;
	const FBoneContainer* BoneContainer = nullptr;

	FAnimationBaseContext() = default;
	FAnimationBaseContext(UAnimInstance* InAnimInstance, const FBoneContainer* InBoneContainer)
		: AnimInstance(InAnimInstance)
		, BoneContainer(InBoneContainer)
	{
	}

	const FBoneContainer& GetBoneContainer() const { return *BoneContainer; }
};

struct FAnimationInitializeContext : public FAnimationBaseContext
{
	using FAnimationBaseContext::FAnimationBaseContext;
};

struct FAnimationUpdateContext : public FAnimationBaseContext
{
	float DeltaTime = 0.0f;
	float CurrentWeight = 1.0f;		// グラフ内の累積ブレンドウェイト

	FAnimationUpdateContext() = default;
	FAnimationUpdateContext(UAnimInstance* InAnimInstance, const FBoneContainer* InBoneContainer, float InDeltaTime)
		: FAnimationBaseContext(InAnimInstance, InBoneContainer)
		, DeltaTime(InDeltaTime)
	{
	}

	// ウェイトを掛けた子用コンテキスト (FAnimationUpdateContext::FractionalWeight)
	FAnimationUpdateContext FractionalWeight(float Multiplier) const
	{
		FAnimationUpdateContext result(*this);
		result.CurrentWeight = CurrentWeight * Multiplier;
		return result;
	}

	float GetDeltaTime() const { return DeltaTime; }
	float GetFinalBlendWeight() const { return CurrentWeight; }
};

struct FPoseContext : public FAnimationBaseContext
{
	FCompactPose  Pose;
	FBlendedCurve Curve;

	FPoseContext() = default;
	FPoseContext(UAnimInstance* InAnimInstance, const FBoneContainer* InBoneContainer)
		: FAnimationBaseContext(InAnimInstance, InBoneContainer)
	{
		Pose.SetBoneContainer(InBoneContainer);
	}

	// 既存のコンテキストから同じ設定の空の出力を作る (子ノードの評価用)
	explicit FPoseContext(const FPoseContext& Other, bool /*bForChild*/)
		: FAnimationBaseContext(Other.AnimInstance, Other.BoneContainer)
	{
		Pose.SetBoneContainer(Other.BoneContainer);
	}

	void ResetToRefPose() { Pose.ResetToRefPose(); Curve.Reset(); }
	void ResetToAdditiveIdentity() { Pose.ResetToAdditiveIdentity(); Curve.Reset(); }
	bool IsValid() const { return Pose.IsValid(); }
};

// ------------------------------------------------------------
//  FPoseLink
//  子ノードへのリンク (FPoseLinkBase / FPoseLink)。
//  未接続なら評価はリファレンスポーズ
// ------------------------------------------------------------
struct FPoseLink
{
	FAnimNode_Base* LinkedNode = nullptr;

	void SetLinkNode(FAnimNode_Base* Node) { LinkedNode = Node; }
	FAnimNode_Base* GetLinkNode() const { return LinkedNode; }

	void Initialize(const FAnimationInitializeContext& Context);
	void Update(const FAnimationUpdateContext& Context);
	void Evaluate(FPoseContext& Output);
};

// ------------------------------------------------------------
//  FAnimNode_Base
// ------------------------------------------------------------
struct FAnimNode_Base
{
	virtual ~FAnimNode_Base() = default;

	virtual void Initialize_AnyThread(const FAnimationInitializeContext& Context) { (void)Context; }
	virtual void Update_AnyThread(const FAnimationUpdateContext& Context) { (void)Context; }
	virtual void Evaluate_AnyThread(FPoseContext& Output) { Output.ResetToRefPose(); }
};

inline void FPoseLink::Initialize(const FAnimationInitializeContext& Context)
{
	if (LinkedNode) LinkedNode->Initialize_AnyThread(Context);
}

inline void FPoseLink::Update(const FAnimationUpdateContext& Context)
{
	if (LinkedNode) LinkedNode->Update_AnyThread(Context);
}

inline void FPoseLink::Evaluate(FPoseContext& Output)
{
	if (LinkedNode)
	{
		LinkedNode->Evaluate_AnyThread(Output);
	}
	else
	{
		Output.ResetToRefPose();
	}
}
