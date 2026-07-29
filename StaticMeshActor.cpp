#include "Main.h"
#include "RenderManager.h"
#include "StaticMeshActor.h"

AStaticMeshActor::AStaticMeshActor()
{
	m_StaticMeshComponent = CreateDefaultSubobject<UStaticMeshComponent>();

	// 静的配置アクターのため Tick 不要 (UE5 の AStaticMeshActor と同じ)
	bCanEverTick = false;
}
