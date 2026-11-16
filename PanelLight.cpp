#include "Main.h"
#include "PanelLight.h"
#include "RectLightComponent.h"

APanelLight::APanelLight()
{
	SetActorLocation({ 0.0f, 5.0f, 4.0f });
	SetActorRotation({ XMConvertToRadians(15.0f), XMConvertToRadians(180.0f), 0.0f });

	URectLightComponent* light = GetRectLightComponent();
	light->SetIntensity(1500.0f);		// lm
	light->SetSourceWidth(1.5f);
	light->SetSourceHeight(1.0f);
	light->SetUseTemperature(true);
	light->SetTemperature(9000.0f);		// 寒色
}
