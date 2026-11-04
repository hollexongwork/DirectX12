#include "Main.h"
#include "SkeletalMeshComponent.h"

USkeletalMeshComponent::USkeletalMeshComponent()
{
	// ポーズ更新のため毎フレーム Tick する (PrimaryComponentTick.bCanEverTick = true)
	bCanEverTick = true;
}

// ============================================================
//  ライフサイクル
// ============================================================

void USkeletalMeshComponent::OnRegister()
{
	USkinnedMeshComponent::OnRegister();

	// UE: USkeletalMeshComponent::OnRegister -> InitAnim(false)
	InitAnim(false);
}

void USkeletalMeshComponent::TickComponent(float DeltaTime)
{
	USkinnedMeshComponent::TickComponent(DeltaTime);

	if (!m_SkeletalMesh || !m_SkeletalMesh->IsLoaded())
	{
		return;
	}

	// UE: TickComponent -> TickPose -> RefreshBoneTransforms
	TickPose(DeltaTime);

	if (!bNoSkeletonUpdate)
	{
		RefreshBoneTransforms();
	}
}

void USkeletalMeshComponent::SetSkeletalMesh(std::shared_ptr<USkeletalMesh> Mesh)
{
	USkinnedMeshComponent::SetSkeletalMesh(std::move(Mesh));

	m_BoneSpaceTransforms.clear();
	if (m_SkeletalMesh && m_SkeletalMesh->IsLoaded())
	{
		m_BoneSpaceTransforms = m_SkeletalMesh->GetRefSkeleton().GetRefBonePose();
	}

	// UE: SetSkeletalMesh -> InitAnim(true) (アニメーションインスタンスを作り直す)
	InitAnim(true);
}

// ============================================================
//  アニメーションインスタンス
// ============================================================

void USkeletalMeshComponent::InitAnim(bool bForceReinit)
{
	if (!m_SkeletalMesh || !m_SkeletalMesh->IsLoaded())
	{
		// メッシュが無ければインスタンスは作らない (単一ノードの再生設定は保つ)
		if (bForceReinit) m_AnimScriptInstance.reset();
		return;
	}

	// 単一ノードモードは UAnimSingleNodeInstance を (無ければ) 作る。
	// AnimationBlueprint モードは SetAnimInstanceClass が載せたものをそのまま使う
	if (m_AnimationMode == EAnimationMode::AnimationSingleNode)
	{
		if (m_AnimScriptInstance == nullptr || dynamic_cast<UAnimSingleNodeInstance*>(m_AnimScriptInstance.get()) == nullptr)
		{
			m_AnimScriptInstance = std::make_unique<UAnimSingleNodeInstance>();
			bForceReinit = true;
		}
	}
	else if (m_AnimationMode == EAnimationMode::AnimationCustomMode)
	{
		m_AnimScriptInstance.reset();
	}

	if (m_AnimScriptInstance && (bForceReinit || !m_AnimScriptInstance->IsInitialized()))
	{
		InitializeAnimScriptInstance(bForceReinit);
	}

	// 初期ポーズを確定してレンダー側へ送る
	RefreshBoneTransforms();
}

void USkeletalMeshComponent::InitializeAnimScriptInstance(bool bForceReinit)
{
	(void)bForceReinit;
	if (!m_AnimScriptInstance) return;

	m_AnimScriptInstance->InitializeAnimation(this);

	// 単一ノードモード: 保存していた再生設定を復元する (FSingleAnimationPlayData::Initialize)
	if (UAnimSingleNodeInstance* single = GetSingleNodeInstance())
	{
		if (m_AnimationData.AnimToPlay)
		{
			single->SetAnimationAsset(m_AnimationData.AnimToPlay, m_AnimationData.bSavedLooping, m_AnimationData.SavedPlayRate);
			single->SetPosition(m_AnimationData.SavedPosition, false);
		}
		single->SetPlaying(m_AnimationData.bSavedPlaying);
		single->SetLooping(m_AnimationData.bSavedLooping);
		single->SetPlayRate(m_AnimationData.SavedPlayRate);
	}
}

void USkeletalMeshComponent::SetAnimationMode(EAnimationMode InAnimationMode)
{
	if (m_AnimationMode == InAnimationMode) return;

	// モードが変わるときは現在のインスタンスを捨てて作り直す
	// (AnimationBlueprint は SetAnimInstanceClass が後で載せる)
	m_AnimationMode = InAnimationMode;
	m_AnimScriptInstance.reset();
	InitAnim(true);
}

UAnimSingleNodeInstance* USkeletalMeshComponent::GetSingleNodeInstance() const
{
	return (m_AnimationMode == EAnimationMode::AnimationSingleNode)
		? dynamic_cast<UAnimSingleNodeInstance*>(m_AnimScriptInstance.get())
		: nullptr;
}

// ============================================================
//  単一ノード再生 API (UE と同じ委譲)
// ============================================================

void USkeletalMeshComponent::PlayAnimation(std::shared_ptr<UAnimSequenceBase> NewAnimToPlay, bool bLooping)
{
	// UE: SetAnimationMode(AnimationSingleNode); SetAnimation(NewAnimToPlay); Play(bLooping);
	SetAnimationMode(EAnimationMode::AnimationSingleNode);
	SetAnimation(std::move(NewAnimToPlay));
	Play(bLooping);
}

void USkeletalMeshComponent::SetAnimation(std::shared_ptr<UAnimSequenceBase> NewAnimToPlay)
{
	m_AnimationData.AnimToPlay = std::move(NewAnimToPlay);
	m_AnimationData.SavedPosition = 0.0f;
	m_AnimationData.bSavedPlaying = false;

	if (UAnimSingleNodeInstance* single = GetSingleNodeInstance())
	{
		// UE: SingleNodeInstance->SetAnimationAsset(NewAnimToPlay, false); SetPlaying(false)
		if (!single->SetAnimationAsset(m_AnimationData.AnimToPlay, m_AnimationData.bSavedLooping, m_AnimationData.SavedPlayRate))
		{
			m_AnimationData.AnimToPlay.reset();
		}
		single->SetPlaying(false);
	}
}

void USkeletalMeshComponent::Play(bool bLooping)
{
	m_AnimationData.bSavedPlaying = true;
	m_AnimationData.bSavedLooping = bLooping;

	if (UAnimSingleNodeInstance* single = GetSingleNodeInstance())
	{
		single->SetPlaying(true);
		single->SetLooping(bLooping);
	}
}

void USkeletalMeshComponent::Stop()
{
	m_AnimationData.bSavedPlaying = false;

	if (UAnimSingleNodeInstance* single = GetSingleNodeInstance())
	{
		single->SetPlaying(false);
	}
}

bool USkeletalMeshComponent::IsPlaying() const
{
	if (const UAnimSingleNodeInstance* single = GetSingleNodeInstance())
	{
		return single->IsPlaying();
	}
	return false;
}

void USkeletalMeshComponent::SetPlayRate(float Rate)
{
	m_AnimationData.SavedPlayRate = Rate;

	if (UAnimSingleNodeInstance* single = GetSingleNodeInstance())
	{
		single->SetPlayRate(Rate);
	}
}

float USkeletalMeshComponent::GetPlayRate() const
{
	if (const UAnimSingleNodeInstance* single = GetSingleNodeInstance())
	{
		return single->GetPlayRate();
	}
	return m_AnimationData.SavedPlayRate;
}

void USkeletalMeshComponent::SetLooping(bool bLooping)
{
	m_AnimationData.bSavedLooping = bLooping;

	if (UAnimSingleNodeInstance* single = GetSingleNodeInstance())
	{
		single->SetLooping(bLooping);
	}
}

bool USkeletalMeshComponent::IsLooping() const
{
	if (const UAnimSingleNodeInstance* single = GetSingleNodeInstance())
	{
		return single->IsLooping();
	}
	return m_AnimationData.bSavedLooping;
}

void USkeletalMeshComponent::SetPosition(float InPos, bool bFireNotifies)
{
	m_AnimationData.SavedPosition = InPos;

	if (UAnimSingleNodeInstance* single = GetSingleNodeInstance())
	{
		single->SetPosition(InPos, bFireNotifies);
	}
}

float USkeletalMeshComponent::GetPosition() const
{
	if (const UAnimSingleNodeInstance* single = GetSingleNodeInstance())
	{
		return single->GetCurrentTime();
	}
	return m_AnimationData.SavedPosition;
}

std::shared_ptr<UAnimSequenceBase> USkeletalMeshComponent::GetCurrentAnimation() const
{
	if (const UAnimSingleNodeInstance* single = GetSingleNodeInstance())
	{
		return single->GetAnimationAsset();
	}
	return m_AnimationData.AnimToPlay;
}

// ============================================================
//  ポーズ更新
// ============================================================

void USkeletalMeshComponent::TickPose(float DeltaTime)
{
	// UE: USkeletalMeshComponent::TickPose -> bPauseAnims なら TickAnimation しない
	if (bPauseAnims) return;

	TickAnimation(DeltaTime * GlobalAnimRateScale);
}

void USkeletalMeshComponent::TickAnimation(float DeltaTime)
{
	if (m_AnimScriptInstance)
	{
		m_AnimScriptInstance->UpdateAnimation(DeltaTime);
	}
}

void USkeletalMeshComponent::RefreshBoneTransforms()
{
	if (!m_SkeletalMesh || !m_SkeletalMesh->IsLoaded())
	{
		return;
	}

	const FReferenceSkeleton& refSkeleton = m_SkeletalMesh->GetRefSkeleton();

	// ---- EvaluateAnimation (UAnimInstance::EvaluateAnimation / ParallelEvaluateAnimation) ----
	bool bEvaluated = false;
	if (m_AnimScriptInstance && m_AnimScriptInstance->IsInitialized())
	{
		FPoseContext output(m_AnimScriptInstance.get(), &m_AnimScriptInstance->GetRequiredBones());
		if (m_AnimScriptInstance->EvaluateAnimation(output) && output.Pose.GetNumBones() == refSkeleton.GetNum())
		{
			m_BoneSpaceTransforms = output.Pose.Bones;
			bEvaluated = true;
		}
	}

	if (!bEvaluated)
	{
		// インスタンス無し / ルートノード無し: リファレンスポーズ
		m_BoneSpaceTransforms = refSkeleton.GetRefBonePose();
	}

	FillComponentSpaceTransforms();
	FinalizeBoneTransform();
}

void USkeletalMeshComponent::FillComponentSpaceTransforms()
{
	if (!m_SkeletalMesh) return;

	FAnimationRuntime::FillUpComponentSpaceTransforms(m_SkeletalMesh->GetRefSkeleton(),
		m_BoneSpaceTransforms, m_ComponentSpaceTransforms);
}

void USkeletalMeshComponent::FinalizeBoneTransform()
{
	// UE: FinalizeBoneTransform -> UpdateBounds / MarkRenderTransformDirty (境界) /
	//     MarkRenderDynamicDataDirty (ボーン行列)
	MarkRenderTransformDirty();
	MarkRenderDynamicDataDirty();
}
