#include "Main.h"
#include "ExponentialHeightFog.h"

AExponentialHeightFog::AExponentialHeightFog()
{
	bCanEverTick = false;	// Tick 不要

	m_FogComponent = CreateDefaultSubobject<UExponentialHeightFogComponent>();
	m_RootComponent = m_FogComponent;
}

void AExponentialHeightFog::SetEnabled(bool bEnabled)
{
	m_bEnabled = bEnabled;

	if (m_FogComponent)
	{
		// OnRep_bEnabled -> Component->SetVisibility(bEnabled)
		m_FogComponent->SetVisibility(bEnabled);
	}
}
