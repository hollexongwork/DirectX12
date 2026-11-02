#include "Main.h"
#include "SkeletalMeshActor.h"

ASkeletalMeshActor::ASkeletalMeshActor()
{
	// 最初の SceneComponent が自動的に Root になる。
	// メッシュは持たない (派生クラス (AManny 等) か GameManager が SetSkeletalMesh する)
	m_SkeletalMeshComponent = CreateDefaultSubobject<USkeletalMeshComponent>();

	// アクター本体は Tick しない (ポーズ更新は USkeletalMeshComponent::TickComponent)
	bCanEverTick = false;
}
