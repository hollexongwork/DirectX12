#pragma once
#include <memory>
#include "SkinnedMeshComponent.h"
#include "AnimInstance.h"

// ============================================================
//  USkeletalMeshComponent
//  USkeletalMeshComponent (Components/SkeletalMeshComponent.h) に相当。
//  アニメーション (UAnimInstance) を駆動してポーズを作り、
//  基底 USkinnedMeshComponent 経由でレンダー側へ送る。
//
//  1 フレーム (TickComponent):
//    TickPose(DeltaTime)
//      TickAnimation(DeltaTime)       AnimScriptInstance->UpdateAnimation (bPauseAnims なら省く)
//    RefreshBoneTransforms()
//      EvaluateAnimation              AnimScriptInstance->EvaluateAnimation -> BoneSpaceTransforms
//      FillComponentSpaceTransforms   親から順に合成 -> ComponentSpaceTransforms
//      FinalizeBoneTransform          UpdateBounds + MarkRenderTransformDirty + MarkRenderDynamicDataDirty
//
//  AnimationMode (EAnimationMode):
//    AnimationBlueprint  : SetAnimInstanceClass<T>() で載せた UAnimInstance 派生
//                          (C++ で NativeUpdateAnimation / GetRootNode を実装する)
//    AnimationSingleNode : UAnimSingleNodeInstance。PlayAnimation / SetAnimation /
//                          Play / Stop / SetPlayRate / SetPosition で操作する
//    AnimationCustomMode : 外部がポーズを与える (本実装ではリファレンスポーズ)
// ============================================================

enum class EAnimationMode
{
	AnimationBlueprint,
	AnimationSingleNode,
	AnimationCustomMode,
};

class USkeletalMeshComponent : public USkinnedMeshComponent
{
private:
	EAnimationMode                 m_AnimationMode = EAnimationMode::AnimationSingleNode;
	std::unique_ptr<UAnimInstance> m_AnimScriptInstance;

	// ボーン空間 (親相対) のポーズ (USkeletalMeshComponent::BoneSpaceTransforms)
	std::vector<FTransform>        m_BoneSpaceTransforms;

	// 単一ノードモードの再生設定 (FSingleAnimationPlayData)。
	// InitAnim でインスタンスを作り直すときに復元する
	struct FSingleAnimationPlayData
	{
		std::shared_ptr<UAnimSequenceBase> AnimToPlay;
		bool  bSavedLooping = true;
		bool  bSavedPlaying = true;
		float SavedPosition = 0.0f;
		float SavedPlayRate = 1.0f;
	};
	FSingleAnimationPlayData m_AnimationData;

	// AnimScriptInstance をメッシュで初期化し、単一ノードモードなら再生設定を復元する
	void InitializeAnimScriptInstance(bool bForceReinit);

public:
	// アニメーションの更新を止める (USkeletalMeshComponent::bPauseAnims)
	bool  bPauseAnims = false;
	// 全アニメーションの速度倍率 (GlobalAnimRateScale)
	float GlobalAnimRateScale = 1.0f;
	// ポーズ評価を止める (bNoSkeletonUpdate)
	bool  bNoSkeletonUpdate = false;

	USkeletalMeshComponent();

	// ---- ライフサイクル ----
	void OnRegister() override;
	void TickComponent(float DeltaTime) override;

	// メッシュ差し替え (USkeletalMeshComponent::SetSkeletalMesh -> InitAnim(true))
	void SetSkeletalMesh(std::shared_ptr<USkeletalMesh> Mesh) override;
	using USkinnedMeshComponent::SetSkeletalMesh;

	// ---- アニメーションインスタンス ----
	// AnimScriptInstance を (再) 生成する (USkeletalMeshComponent::InitAnim)
	void InitAnim(bool bForceReinit);

	void SetAnimationMode(EAnimationMode InAnimationMode);
	EAnimationMode GetAnimationMode() const { return m_AnimationMode; }

	// 任意の UAnimInstance 派生を載せる (AnimationBlueprint モード。SetAnimInstanceClass 相当)
	template <class T>
	T* SetAnimInstanceClass()
	{
		static_assert(std::is_base_of<UAnimInstance, T>::value, "T must derive from UAnimInstance");
		auto instance = std::make_unique<T>();
		T* ptr = instance.get();
		m_AnimScriptInstance = std::move(instance);
		m_AnimationMode = EAnimationMode::AnimationBlueprint;
		InitializeAnimScriptInstance(true);
		return ptr;
	}

	UAnimInstance* GetAnimInstance() const { return m_AnimScriptInstance.get(); }
	// 単一ノードモードのインスタンス (それ以外のモードでは nullptr)
	UAnimSingleNodeInstance* GetSingleNodeInstance() const;

	// ---- 単一ノード再生 (USkeletalMeshComponent::PlayAnimation 系) ----
	void  PlayAnimation(std::shared_ptr<UAnimSequenceBase> NewAnimToPlay, bool bLooping);
	void  SetAnimation(std::shared_ptr<UAnimSequenceBase> NewAnimToPlay);
	void  Play(bool bLooping);
	void  Stop();
	bool  IsPlaying() const;
	void  SetPlayRate(float Rate);
	float GetPlayRate() const;
	void  SetLooping(bool bLooping);
	bool  IsLooping() const;
	void  SetPosition(float InPos, bool bFireNotifies = true);
	float GetPosition() const;
	std::shared_ptr<UAnimSequenceBase> GetCurrentAnimation() const;

	// ---- ポーズ更新 ----
	void TickPose(float DeltaTime);
	void TickAnimation(float DeltaTime);
	void RefreshBoneTransforms();
	void FillComponentSpaceTransforms();
	void FinalizeBoneTransform();

	const std::vector<FTransform>& GetBoneSpaceTransforms() const { return m_BoneSpaceTransforms; }
};
