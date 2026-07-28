#include "Main.h"
#include "PostProcessVolume.h"
#include "World.h"

void APostProcessVolume::BeginPlay()
{
	if (GetWorld())
	{
		GetWorld()->GetScene()->SetPostProcessVolume(this);
	}
}

void APostProcessVolume::EndPlay()
{
	if (GetWorld() && GetWorld()->GetScene()->GetPostProcessVolume() == this)
	{
		GetWorld()->GetScene()->SetPostProcessVolume(nullptr);
	}
}

void APostProcessVolume::Tick(float DeltaTime)
{
	// Per-frame: advance grain seed, recompute exposure from EV.
	m_Settings.FilmGrainTime += DeltaTime;
	m_Settings.Exposure = powf(2.0f, m_EV);
}
