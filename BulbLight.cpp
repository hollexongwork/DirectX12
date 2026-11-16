#include "Main.h"
#include "BulbLight.h"
#include "PointLightComponent.h"

ABulbLight::ABulbLight()
{
	SetActorLocation({ -2.5f, 5.0f, -2.0f });

	UPointLightComponent* light = GetPointLightComponent();
	light->SetIntensity(800.0f);		// lm
	light->SetSourceRadius(0.05f);
	light->SetUseTemperature(true);
	light->SetTemperature(3000.0f);		// 電球色
}
