#include "Main.h"
#include "LightComponent.h"
#include "World.h"
#include "Scene.h"

// ============================================================
//  ULightComponentBase
// ============================================================

XMFLOAT3 ULightComponentBase::DirectionToRotator(const XMFLOAT3& Direction)
{
	XMFLOAT3 d;
	XMStoreFloat3(&d, XMVector3Normalize(XMLoadFloat3(&Direction)));

	// 前方 +Z 規約: forward = (cosP * sinY, -sinP, cosP * cosY)
	// (XMMatrixRotationRollPitchYaw に行ベクトルを掛けた結果)
	float pitch = asinf(-d.y);
	float yaw = atan2f(d.x, d.z);
	return XMFLOAT3(pitch, yaw, 0.0f);
}

// ============================================================
//  ULightComponent
// ============================================================

void ULightComponent::OnRegister()
{
	if (GetWorld())
	{
		GetWorld()->GetScene()->AddLight(this);
	}
}

void ULightComponent::OnUnregister()
{
	if (GetWorld())
	{
		GetWorld()->GetScene()->RemoveLight(this);
	}
}

// ============================================================
//  ダーティ通知 (プッシュ型更新)
//  フラグの立ち上がり (false -> true) のときだけ FScene の
//  ダーティリストへ自分を積む (二重登録防止)。未登録時はフラグのみ
//  立て、FScene::AddLight が登録時に処理する。
// ============================================================

void ULightComponent::MarkRenderStateDirty()
{
	const bool bWasDirty = m_RenderStateDirty;
	ULightComponentBase::MarkRenderStateDirty();

	if (!bWasDirty && IsRegistered() && GetWorld())
	{
		GetWorld()->GetScene()->AddLightRenderStateDirty(this);
	}
}

void ULightComponent::MarkRenderTransformDirty()
{
	if (!m_RenderTransformDirty)
	{
		m_RenderTransformDirty = true;

		if (IsRegistered() && GetWorld())
		{
			GetWorld()->GetScene()->AddLightTransformDirty(this);
		}
	}

	// アタッチ子への再帰伝搬
	USceneComponent::MarkRenderTransformDirty();
}

void ULightComponent::SendRenderTransform()
{
	if (m_SceneProxy)
	{
		m_SceneProxy->SetTransform(GetComponentLocation(), GetForwardVector(), GetRightVector());
	}
}

XMFLOAT3 ULightComponent::GetColoredLightBrightness() const
{
	const float brightness = ComputeLightBrightness();

	XMFLOAT3 color = { m_LightColor.x, m_LightColor.y, m_LightColor.z };

	if (m_bUseTemperature)
	{
		const XMFLOAT3 temperature = ColorTemperatureToRGB(m_Temperature);
		color.x *= temperature.x;
		color.y *= temperature.y;
		color.z *= temperature.z;
	}

	color.x *= brightness;
	color.y *= brightness;
	color.z *= brightness;
	return color;
}

XMFLOAT3 ULightComponent::ColorTemperatureToRGB(float TemperatureKelvin)
{
	// FLinearColor::MakeFromColorTemperature の移植。
	// Planckian locus (黒体軌跡) の CIE 1960 UCS 近似 -> xy 色度 -> XYZ -> リニア sRGB。
	const float t = fmaxf(fminf(TemperatureKelvin, 15000.0f), 1000.0f);

	const float u = (0.860117757f + 1.54118254e-4f * t + 1.28641212e-7f * t * t)
		/ (1.0f + 8.42420235e-4f * t + 7.08145163e-7f * t * t);
	const float v = (0.317398726f + 4.22806245e-5f * t + 4.20481691e-8f * t * t)
		/ (1.0f - 2.89741816e-5f * t + 1.61456053e-7f * t * t);

	const float x = 3.0f * u / (2.0f * u - 8.0f * v + 4.0f);
	const float y = 2.0f * v / (2.0f * u - 8.0f * v + 4.0f);
	const float z = 1.0f - x - y;

	const float Y = 1.0f;
	const float X = Y / y * x;
	const float Z = Y / y * z;

	// XYZ -> リニア sRGB (D65)
	XMFLOAT3 color;
	color.x = 3.2404542f * X - 1.5371385f * Y - 0.4985314f * Z;
	color.y = -0.9692660f * X + 1.8760108f * Y + 0.0415560f * Z;
	color.z = 0.0556434f * X - 0.2040259f * Y + 1.0572252f * Z;

	color.x = fmaxf(color.x, 0.0f);
	color.y = fmaxf(color.y, 0.0f);
	color.z = fmaxf(color.z, 0.0f);
	return color;
}

// ============================================================
//  UDirectionalLightComponent
// ============================================================

FLightSceneProxy* UDirectionalLightComponent::CreateLightSceneProxy() const
{
	// 共通スナップショット (種別 / 色 x lux 強度 / トランスフォーム / CastShadows)
	// + CSM パラメータを流し込む
	FLightSceneProxy* proxy = new FLightSceneProxy(this);
	proxy->SetShadowParameters(m_ShadowBias, m_ShadowSlopeBias);
	proxy->SetDirectionalShadowParameters(
		m_DynamicShadowDistance, m_DynamicShadowCascades,
		m_CascadeDistributionExponent, m_ShadowDistanceFadeoutFraction);
	proxy->SetDistanceFieldParameters(
		m_DistanceFieldShadowDistance, m_DistanceFieldTraceDistance, m_LightSourceAngle);
	return proxy;
}

// ============================================================
//  UPointLightComponent
// ============================================================

float UPointLightComponent::ComputeLightBrightness() const
{
	// UPointLightComponent::ComputeLightBrightness のメートル世界版。
	float LightBrightness = m_Intensity;

	if (m_bUseInverseSquaredFalloff)
	{
		switch (m_IntensityUnits)
		{
		case ELightUnits::Candelas:
			// cd はそのまま (1000 cd = 1m で 1000 lux)
			break;
		case ELightUnits::Lumens:
			// 全球 4π sr で除して cd 化
			LightBrightness *= 1.0f / (4.0f * XM_PI);
			break;
		case ELightUnits::EV:
			LightBrightness = powf(2.0f, m_Intensity);
			break;
		default:	// Unitless
			// legacy 係数 16 / 10000 (cm^2 -> m^2 換算込み) = 1/625
			LightBrightness *= 1.0f / 625.0f;
			break;
		}
	}
	return LightBrightness;
}

FLightSceneProxy* UPointLightComponent::CreateLightSceneProxy() const
{
	FLightSceneProxy* proxy = new FLightSceneProxy(this);
	proxy->SetRadialParameters(
		1.0f / fmaxf(m_AttenuationRadius, 0.0001f),
		m_LightFalloffExponent,
		m_bUseInverseSquaredFalloff);
	proxy->SetSourceShape(m_SourceRadius, m_SoftSourceRadius, m_SourceLength);
	proxy->SetShadowParameters(m_ShadowBias, m_ShadowSlopeBias);
	return proxy;
}

// ============================================================
//  USpotLightComponent
// ============================================================

float USpotLightComponent::GetHalfConeAngle() const
{
	// 1..80 度にクランプ
	const float clamped = fmaxf(fminf(m_OuterConeAngle, 80.0f), 1.0f);
	return XMConvertToRadians(clamped);
}

float USpotLightComponent::GetCosHalfConeAngle() const
{
	return cosf(GetHalfConeAngle());
}

float USpotLightComponent::ComputeLightBrightness() const
{
	float LightBrightness = m_Intensity;

	if (m_bUseInverseSquaredFalloff)
	{
		switch (m_IntensityUnits)
		{
		case ELightUnits::Candelas:
			break;
		case ELightUnits::Lumens:
			// コーン立体角 2π(1 - cosθ) で除して cd 化
			LightBrightness *= 1.0f / (2.0f * XM_PI * (1.0f - GetCosHalfConeAngle()));
			break;
		case ELightUnits::EV:
			LightBrightness = powf(2.0f, m_Intensity);
			break;
		default:	// Unitless
			LightBrightness *= 1.0f / 625.0f;
			break;
		}
	}
	return LightBrightness;
}

FLightSceneProxy* USpotLightComponent::CreateLightSceneProxy() const
{
	FLightSceneProxy* proxy = new FLightSceneProxy(this);
	proxy->SetRadialParameters(
		1.0f / fmaxf(m_AttenuationRadius, 0.0001f),
		m_LightFalloffExponent,
		m_bUseInverseSquaredFalloff);
	proxy->SetSourceShape(m_SourceRadius, m_SoftSourceRadius, m_SourceLength);

	// SpotAngles: x = cos(Outer), y = 1 / (cos(Inner) - cos(Outer))
	// (FSpotLightSceneProxy と同じ詰め方。Inner は Outer 以下にクランプ)
	const float clampedOuterDeg = fmaxf(fminf(m_OuterConeAngle, 80.0f), 1.0f);
	const float clampedInnerDeg = fmaxf(fminf(m_InnerConeAngle, clampedOuterDeg), 0.0f);
	const float cosOuter = cosf(XMConvertToRadians(clampedOuterDeg));
	const float cosInner = cosf(XMConvertToRadians(clampedInnerDeg));
	proxy->SetSpotAngles(cosOuter, 1.0f / fmaxf(cosInner - cosOuter, 0.0001f));
	proxy->SetShadowParameters(m_ShadowBias, m_ShadowSlopeBias);
	return proxy;
}

// ============================================================
//  URectLightComponent
// ============================================================

float URectLightComponent::ComputeLightBrightness() const
{
	// レクトライトは常に逆二乗 
	float LightBrightness = m_Intensity;

	switch (m_IntensityUnits)
	{
	case ELightUnits::Candelas:
		break;
	case ELightUnits::Lumens:
		// 半球コサイン分布の実効立体角 π で除して cd 化
		LightBrightness *= 1.0f / XM_PI;
		break;
	case ELightUnits::EV:
		LightBrightness = powf(2.0f, m_Intensity);
		break;
	default:	// Unitless
		LightBrightness *= 1.0f / 625.0f;
		break;
	}
	return LightBrightness;
}

FLightSceneProxy* URectLightComponent::CreateLightSceneProxy() const
{
	FLightSceneProxy* proxy = new FLightSceneProxy(this);

	// レクトライトは逆二乗固定 (FalloffExponent は未使用)
	proxy->SetRadialParameters(1.0f / fmaxf(m_AttenuationRadius, 0.0001f), 8.0f, true);

	// FRectLightSceneProxy と同じ詰め方:
	//   SourceRadius = 半幅 / SourceLength = 半高
	proxy->SetSourceShape(m_SourceWidth * 0.5f, 0.0f, m_SourceHeight * 0.5f);

	// バーンドア (88 度以上 ≒ 全開はシェーダ側で早期スキップされる)
	const float clampedBarnDeg = fmaxf(fminf(m_BarnDoorAngle, 88.0f), 0.0f);
	proxy->SetRectBarnDoor(cosf(XMConvertToRadians(clampedBarnDeg)), fmaxf(m_BarnDoorLength, 0.0f));
	proxy->SetShadowParameters(m_ShadowBias, m_ShadowSlopeBias);
	return proxy;
}
