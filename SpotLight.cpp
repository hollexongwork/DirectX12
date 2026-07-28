#include "Main.h"
#include "SpotLight.h"

ASpotLight::ASpotLight()
{
	m_SpotLightComponent = CreateDefaultSubobject<USpotLightComponent>();
	m_LightComponent = m_SpotLightComponent;

	// 既定 (Pitch -90 度) と同じ真下向き (+Z を -Y へ)
	SetActorRotation({ XMConvertToRadians(90.0f), 0.0f, 0.0f });
}
