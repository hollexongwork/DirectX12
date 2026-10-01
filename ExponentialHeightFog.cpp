#include "Main.h"
#include "ExponentialHeightFog.h"

AExponentialHeightFog::AExponentialHeightFog()
{
	bCanEverTick = false;	// Tick 不要

	m_FogComponent = CreateDefaultSubobject<UExponentialHeightFogComponent>();
}

void AExponentialHeightFog::SetEnabled(bool bEnabled)
{
	if (m_FogComponent)
	{
		// OnRep_bEnabled -> Component->SetVisibility(bEnabled)
		m_FogComponent->SetVisibility(bEnabled);
	}
}
