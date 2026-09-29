#include "Main.h"
#include "StaticMeshActor.h"

AStaticMeshActor::AStaticMeshActor()
{
	m_StaticMeshComponent = CreateDefaultSubobject<UStaticMeshComponent>();

	// 静的配置アクターのため Tick 不要 (UE の AStaticMeshActor と同じ)
	bCanEverTick = false;
}
