#include "Main.h"
#include "AnimInstance.h"
#include "SkeletalMeshComponent.h"
#include "SkeletalMesh.h"

// ============================================================
//  UAnimInstance
// ============================================================

void UAnimInstance::InitializeAnimation(USkeletalMeshComponent* Component)
{
	m_SkelMeshComponent = Component;
	m_bInitialized = false;
	m_CurrentSkeleton.reset();
	m_RequiredBones.Reset();

	const std::shared_ptr<USkeletalMesh>& mesh = Component ? Component->GetSkeletalMesh() : nullptr;
	if (!mesh || !mesh->IsLoaded())
	{
		return;
	}

	// UE: UAnimInstance::InitializeAnimation -> CurrentSkeleton / RequiredBones (FBoneContainer)
	m_CurrentSkeleton = mesh->GetSkeleton();
	m_RequiredBones.InitializeTo(mesh->GetRefSkeleton());

	NativeInitializeAnimation();

	if (FAnimNode_Base* root = GetRootNode())
	{
		root->Initialize_AnyThread(FAnimationInitializeContext(this, &m_RequiredBones));
	}

	m_bInitialized = true;
}

void UAnimInstance::UpdateAnimation(float DeltaSeconds)
{
	if (!m_bInitialized) return;

	m_CurrentDeltaSeconds = DeltaSeconds;

	NativeUpdateAnimation(DeltaSeconds);

	// GetRootNode は NativeUpdateAnimation でアセットが変わると別ノードになり得るので毎回引く
	if (FAnimNode_Base* root = GetRootNode())
	{
		root->Update_AnyThread(FAnimationUpdateContext(this, &m_RequiredBones, DeltaSeconds));
	}
}

bool UAnimInstance::EvaluateAnimation(FPoseContext& Output)
{
	if (!m_bInitialized) return false;

	FAnimNode_Base* root = GetRootNode();
	if (root == nullptr) return false;

	root->Evaluate_AnyThread(Output);
	return true;
}

// ============================================================
//  UAnimSingleNodeInstance
// ============================================================

bool UAnimSingleNodeInstance::SetAnimationAsset(std::shared_ptr<UAnimSequenceBase> NewAsset, bool bIsLooping, float InPlayRate)
{
	if (NewAsset && m_CurrentSkeleton && !NewAsset->IsCompatibleWith(m_CurrentSkeleton.get()))
	{
		char msg[256];
		sprintf_s(msg, "[UAnimSingleNodeInstance] animation '%s' is not compatible with skeleton '%s'\n",
			NewAsset->GetName().c_str(), m_CurrentSkeleton->GetName().c_str());
		OutputDebugStringA(msg);
		return false;
	}

	m_CurrentAsset = std::move(NewAsset);
	m_bLooping = bIsLooping;
	m_PlayRate = InPlayRate;

	m_SequencePlayer.Sequence = m_CurrentAsset;
	m_SequencePlayer.bLoopAnimation = m_bLooping;
	m_SequencePlayer.PlayRate = m_bPlaying ? m_PlayRate : 0.0f;
	m_SequencePlayer.StartPosition = 0.0f;
	m_SequencePlayer.Initialize_AnyThread(FAnimationInitializeContext(this, &m_RequiredBones));
	return true;
}

void UAnimSingleNodeInstance::SetPosition(float InPosition, bool bFireNotifies)
{
	(void)bFireNotifies;	// 通知は未対応
	m_SequencePlayer.SetAccumulatedTime(InPosition);
	m_SequencePlayer.bFinished = false;
}

void UAnimSingleNodeInstance::NativeInitializeAnimation()
{
	// InitializeAnimation (メッシュ差し替え) の後も現在のアセットを保ったまま再開する。
	// スケルトンが変わって非互換になったアセットは外す
	if (m_CurrentAsset && m_CurrentSkeleton && !m_CurrentAsset->IsCompatibleWith(m_CurrentSkeleton.get()))
	{
		m_CurrentAsset.reset();
	}
	m_SequencePlayer.Sequence = m_CurrentAsset;
	m_SequencePlayer.bLoopAnimation = m_bLooping;
	m_SequencePlayer.PlayRate = m_bPlaying ? m_PlayRate : 0.0f;
	m_SequencePlayer.StartPosition = 0.0f;
}

void UAnimSingleNodeInstance::NativeUpdateAnimation(float DeltaSeconds)
{
	(void)DeltaSeconds;
	// 停止中は速度 0 で位置を保つ (UE はティックレコードを作らない。評価は現在位置のまま)
	m_SequencePlayer.bLoopAnimation = m_bLooping;
	m_SequencePlayer.PlayRate = m_bPlaying ? m_PlayRate : 0.0f;
}
