#include "Main.h"
#include "HeterogeneousVolume.h"

AHeterogeneousVolume::AHeterogeneousVolume()
{
	bCanEverTick = false;	// Tick 不要

	m_HeterogeneousVolumeComponent = CreateDefaultSubobject<UHeterogeneousVolumeComponent>();
}
