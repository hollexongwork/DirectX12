#include "Main.h"
#include "DownLight.h"
#include "SpotLightComponent.h"

ADownLight::ADownLight()
{
	SetActorLocation({ 2.0f, 8.0f, 1.0f });

	USpotLightComponent* light = GetSpotLightComponent();
	light->SetIntensity(2000.0f);		// lm
	light->SetOuterConeAngle(30.0f);
	light->SetInnerConeAngle(15.0f);
}
