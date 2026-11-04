#pragma once
#include <memory>
#include "AnimNodes.h"

// ============================================================
//  UAnimInstance
//  UAnimInstance (Animation/AnimInstance.h) + FAnimInstanceProxy の
//  本エンジン版。USkeletalMeshComponent が所有し、毎フレーム
//    UpdateAnimation(DeltaSeconds)  -> NativeUpdateAnimation + ルートノードの Update_AnyThread
//    EvaluateAnimation(FPoseContext) -> ルートノードの Evaluate_AnyThread
//  を呼ぶ。派生クラス (AnimationBlueprint モード相当) は
//  NativeInitializeAnimation / NativeUpdateAnimation / GetRootNode を
//  オーバーライドして C++ でノードグラフを組む。
//
//  UAnimSingleNodeInstance は AnimationSingleNode モード
//  (PlayAnimation / SetAnimation / Play / Stop ...) の実装。
// ============================================================

class USkeletalMeshComponent;

class UAnimInstance
{
protected:
	USkeletalMeshComponent* m_SkelMeshComponent = nullptr;
	std::shared_ptr<USkeleton> m_CurrentSkeleton;
	FBoneContainer          m_RequiredBones;
	float                   m_CurrentDeltaSeconds = 0.0f;
	bool                    m_bInitialized = false;

public:
	virtual ~UAnimInstance() = default;

	// ---- ライフサイクル (USkeletalMeshComponent::InitAnim が呼ぶ) ----
	// RequiredBones をメッシュのリファレンススケルトンで初期化し、
	// NativeInitializeAnimation とルートノードの Initialize_AnyThread を呼ぶ
	void InitializeAnimation(USkeletalMeshComponent* Component);

	// 1 フレーム分の更新 (UAnimInstance::UpdateAnimation)。
	// NativeUpdateAnimation の後にルートノードの Update_AnyThread
	void UpdateAnimation(float DeltaSeconds);

	// ポーズ評価 (UAnimInstance::EvaluateAnimation / ParallelEvaluateAnimation)。
	// ルートノードが無ければ false (呼び出し側はリファレンスポーズ)
	bool EvaluateAnimation(FPoseContext& Output);

	// ---- 派生側フック (NativeInitializeAnimation / NativeUpdateAnimation) ----
	virtual void NativeInitializeAnimation() {}
	virtual void NativeUpdateAnimation(float DeltaSeconds) { (void)DeltaSeconds; }

	// グラフのルートノード (FAnimInstanceProxy::GetRootNode)。nullptr = リファレンスポーズ
	virtual FAnimNode_Base* GetRootNode() { return nullptr; }

	// ---- アクセサ ----
	USkeletalMeshComponent* GetSkelMeshComponent() const { return m_SkelMeshComponent; }
	const FBoneContainer& GetRequiredBones() const { return m_RequiredBones; }
	const std::shared_ptr<USkeleton>& GetCurrentSkeleton() const { return m_CurrentSkeleton; }
	float GetDeltaSeconds() const { return m_CurrentDeltaSeconds; }
	bool  IsInitialized() const { return m_bInitialized; }
};

// ------------------------------------------------------------
//  UAnimSingleNodeInstance
//  単一のアニメーションアセットを FAnimNode_SequencePlayer で再生する
//  (USkeletalMeshComponent::AnimationMode == AnimationSingleNode)
// ------------------------------------------------------------
class UAnimSingleNodeInstance : public UAnimInstance
{
private:
	FAnimNode_SequencePlayer           m_SequencePlayer;
	std::shared_ptr<UAnimSequenceBase> m_CurrentAsset;
	bool  m_bPlaying = true;
	float m_PlayRate = 1.0f;
	bool  m_bLooping = true;

public:
	// アセットを差し替える (UAnimSingleNodeInstance::SetAnimationAsset)。
	// 位置は 0 に戻る。スケルトン非互換なら差し替えず false
	bool SetAnimationAsset(std::shared_ptr<UAnimSequenceBase> NewAsset, bool bIsLooping = true, float InPlayRate = 1.0f);
	const std::shared_ptr<UAnimSequenceBase>& GetAnimationAsset() const { return m_CurrentAsset; }

	void  SetPlaying(bool bIsPlaying) { m_bPlaying = bIsPlaying; }
	bool  IsPlaying() const { return m_bPlaying; }
	void  SetLooping(bool bIsLooping) { m_bLooping = bIsLooping; }
	bool  IsLooping() const { return m_bLooping; }
	void  SetPlayRate(float InPlayRate) { m_PlayRate = InPlayRate; }
	float GetPlayRate() const { return m_PlayRate; }

	// 再生位置 [s]
	void  SetPosition(float InPosition, bool bFireNotifies = true);
	float GetCurrentTime() const { return m_SequencePlayer.GetAccumulatedTime(); }
	float GetLength() const { return m_CurrentAsset ? m_CurrentAsset->GetPlayLength() : 0.0f; }

	// 非ループ再生が終端に達したか
	bool  IsFinished() const { return m_SequencePlayer.bFinished; }

	void NativeInitializeAnimation() override;
	void NativeUpdateAnimation(float DeltaSeconds) override;
	FAnimNode_Base* GetRootNode() override { return m_CurrentAsset ? &m_SequencePlayer : nullptr; }
};
