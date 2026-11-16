#include "Main.h"
#include "VolumetricHeightFog.h"

AVolumetricHeightFog::AVolumetricHeightFog()
{
	SetActorLocation({ 0.0f, 0.0f, 0.0f });	// フォグの基準高さ = Y 0
	GetComponent()->SetVolumetricFog(true);
}
