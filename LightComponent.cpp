#include "Main.h"
#include "LightComponent.h"
#include "World.h"
#include "Scene.h"

// ============================================================
//  ULightComponent : 登録 / レンダーステート
// ============================================================

void ULightComponent::OnRegister()
{
	// 登録直後はプロキシが最新なので、未エンキューのフラグを下ろしてから作る
	m_RenderStateDirty = false;
	m_RenderTransformDirty = false;

	CreateRenderState();
}

void ULightComponent::OnUnregister()
{
	DestroyRenderState();

	// 「フラグ = エンキュー済み」の対応を保つため、ダーティリストからも外す
	// (解放済みポインタを FScene に残さない)
	if (GetWorld())
	{
		GetWorld()->GetScene()->RemoveLightFromDirtyLists(this);
	}
	m_RenderStateDirty = false;
	m_RenderTransformDirty = false;
}

// ULightComponent::CreateRenderState_Concurrent 相当。
// ワールドに影響し、可視で、明るさが正のライトだけをシーンへ入れる
void ULightComponent::CreateRenderState()
{
	UWorld* world = GetWorld();
	if (world == nullptr)
	{
		return;
	}

	if (m_bAffectsWorld)
	{
		const bool bHidden = !m_bVisible || m_Intensity <= 0.0f;
		if (!bHidden)
		{
			world->GetScene()->AddLight(this);
			m_bAddedToSceneVisible = (m_SceneProxy != nullptr);
		}
	}
}

// ULightComponent::DestroyRenderState_Concurrent 相当
void ULightComponent::DestroyRenderState()
{
	if (UWorld* world = GetWorld())
	{
		world->GetScene()->RemoveLight(this);
	}
	m_bAddedToSceneVisible = false;
}

void ULightComponent::RecreateRenderState()
{
	DestroyRenderState();
	CreateRenderState();
}

// ULightComponent::SendRenderTransform_Concurrent 相当
void ULightComponent::SendRenderTransform()
{
	if (UWorld* world = GetWorld())
	{
		world->GetScene()->UpdateLightTransform(this);
	}
}

// ============================================================
//  ダーティ通知 (プッシュ型更新)
//  フラグの立ち上がり (false -> true) のときだけ FScene の
//  ダーティリストへ自分を積む (二重登録防止)。未登録時はフラグのみ
//  立て、OnRegister が登録時に下ろす (登録時に最新のプロキシを作るため)。
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

// ULightComponent::UpdateColorAndBrightness 相当
void ULightComponent::UpdateColorAndBrightness()
{
	UWorld* world = GetWorld();
	if (world == nullptr || !IsRegistered())
	{
		return;
	}

	const bool bNeedsToBeAddedToScene = (!m_bAddedToSceneVisible && m_Intensity > 0.0f);
	const bool bNeedsToBeRemovedFromScene = (m_bAddedToSceneVisible && m_Intensity <= 0.0f);
	if (bNeedsToBeAddedToScene || bNeedsToBeRemovedFromScene)
	{
		// 明るさが 0 を跨いだ (または非表示だった)。シーンへの出し入れが要るので作り直す
		MarkRenderStateDirty();
	}
	else if (m_bAddedToSceneVisible && m_Intensity > 0.0f)
	{
		// 既にシーンに居る。軽量経路で色だけ更新する
		world->GetScene()->UpdateLightColorAndBrightness(this);
	}
}

// ============================================================
//  可視性
// ============================================================

void ULightComponent::SetVisibility(bool bNewVisibility)
{
	if (m_bVisible != bNewVisibility)
	{
		m_bVisible = bNewVisibility;
		MarkRenderStateDirty();
	}
}

// ============================================================
//  軽量経路で反映されるプロパティ
// ============================================================

void ULightComponent::SetIntensity(float NewIntensity)
{
	if (m_Intensity != NewIntensity)
	{
		m_Intensity = NewIntensity;
		UpdateColorAndBrightness();
	}
}

void ULightComponent::SetIndirectLightingIntensity(float NewIntensity)
{
	if (m_IndirectLightingIntensity != NewIntensity)
	{
		m_IndirectLightingIntensity = NewIntensity;
		UpdateColorAndBrightness();
	}
}

void ULightComponent::SetVolumetricScatteringIntensity(float NewIntensity)
{
	if (m_VolumetricScatteringIntensity != NewIntensity)
	{
		m_VolumetricScatteringIntensity = NewIntensity;
		UpdateColorAndBrightness();
	}
}

void ULightComponent::SetLightColor(const XMFLOAT4& NewLightColor)
{
	if (m_LightColor.x != NewLightColor.x || m_LightColor.y != NewLightColor.y
		|| m_LightColor.z != NewLightColor.z || m_LightColor.w != NewLightColor.w)
	{
		m_LightColor = NewLightColor;
		UpdateColorAndBrightness();
	}
}

void ULightComponent::SetTemperature(float NewTemperature)
{
	if (m_Temperature != NewTemperature)
	{
		m_Temperature = NewTemperature;
		UpdateColorAndBrightness();
	}
}

void ULightComponent::SetUseTemperature(bool bNewValue)
{
	if (m_bUseTemperature != bNewValue)
	{
		m_bUseTemperature = bNewValue;
		UpdateColorAndBrightness();
	}
}

// ============================================================
//  プロキシを作り直すプロパティ
// ============================================================

void ULightComponent::SetAffectTranslucentLighting(bool bNewValue)
{
	if (m_bAffectTranslucentLighting != bNewValue)
	{
		m_bAffectTranslucentLighting = bNewValue;
		MarkRenderStateDirty();
	}
}

void ULightComponent::SetShadowBias(float NewValue)
{
	if (m_ShadowBias != NewValue)
	{
		m_ShadowBias = NewValue;
		MarkRenderStateDirty();
	}
}

void ULightComponent::SetShadowSlopeBias(float NewValue)
{
	if (m_ShadowSlopeBias != NewValue)
	{
		m_ShadowSlopeBias = NewValue;
		MarkRenderStateDirty();
	}
}

void ULightComponent::SetSpecularScale(float NewValue)
{
	if (m_SpecularScale != NewValue)
	{
		m_SpecularScale = NewValue;
		MarkRenderStateDirty();
	}
}

void ULightComponent::SetDiffuseScale(float NewValue)
{
	if (m_DiffuseScale != NewValue)
	{
		m_DiffuseScale = NewValue;
		MarkRenderStateDirty();
	}
}

void ULightComponent::SetMaxDrawDistance(float NewValue)
{
	if (m_MaxDrawDistance != NewValue)
	{
		m_MaxDrawDistance = NewValue;
		MarkRenderStateDirty();
	}
}

void ULightComponent::SetMaxDistanceFadeRange(float NewValue)
{
	if (m_MaxDistanceFadeRange != NewValue)
	{
		m_MaxDistanceFadeRange = NewValue;
		MarkRenderStateDirty();
	}
}

void ULightComponent::SetContactShadowLength(float NewValue)
{
	if (m_ContactShadowLength != NewValue)
	{
		m_ContactShadowLength = NewValue;
		MarkRenderStateDirty();
	}
}

void ULightComponent::SetContactShadowLengthInWS(bool bNewValue)
{
	if (m_ContactShadowLengthInWS != bNewValue)
	{
		m_ContactShadowLengthInWS = bNewValue;
		MarkRenderStateDirty();
	}
}

void ULightComponent::SetContactShadowCastingIntensity(float NewValue)
{
	if (m_ContactShadowCastingIntensity != NewValue)
	{
		m_ContactShadowCastingIntensity = NewValue;
		MarkRenderStateDirty();
	}
}

void ULightComponent::SetContactShadowNonCastingIntensity(float NewValue)
{
	if (m_ContactShadowNonCastingIntensity != NewValue)
	{
		m_ContactShadowNonCastingIntensity = NewValue;
		MarkRenderStateDirty();
	}
}

void ULightComponent::SetUseRayTracedDistanceFieldShadows(bool bNewValue)
{
	if (m_bUseRayTracedDistanceFieldShadows != bNewValue)
	{
		m_bUseRayTracedDistanceFieldShadows = bNewValue;
		MarkRenderStateDirty();
	}
}

// ============================================================
//  明るさ / 色
// ============================================================

// ULightComponent::ComputeLightBrightness。IES が無いので Intensity そのまま
float ULightComponent::ComputeLightBrightness() const
{
	return m_Intensity;
}

XMFLOAT4X4 ULightComponent::GetLightToWorldNoScale() const
{
	const XMMATRIX world = GetComponentToWorld();

	// 各軸を正規化してスケールを落とす (FTransform::ToMatrixNoScale 相当)
	XMFLOAT3 right, up, forward, location;
	XMStoreFloat3(&right, XMVector3Normalize(world.r[0]));
	XMStoreFloat3(&up, XMVector3Normalize(world.r[1]));
	XMStoreFloat3(&forward, XMVector3Normalize(world.r[2]));
	XMStoreFloat3(&location, world.r[3]);

	XMFLOAT4X4 lightToWorld;
	lightToWorld._11 = right.x;    lightToWorld._12 = right.y;    lightToWorld._13 = right.z;    lightToWorld._14 = 0.0f;
	lightToWorld._21 = up.x;       lightToWorld._22 = up.y;       lightToWorld._23 = up.z;       lightToWorld._24 = 0.0f;
	lightToWorld._31 = forward.x;  lightToWorld._32 = forward.y;  lightToWorld._33 = forward.z;  lightToWorld._34 = 0.0f;
	lightToWorld._41 = location.x; lightToWorld._42 = location.y; lightToWorld._43 = location.z; lightToWorld._44 = 1.0f;
	return lightToWorld;
}

XMFLOAT3 ULightComponent::GetColoredLightBrightness() const
{
	const float brightness = ComputeLightBrightness();

	XMFLOAT3 color = { m_LightColor.x, m_LightColor.y, m_LightColor.z };

	if (m_bUseTemperature)
	{
		const XMFLOAT3 temperature = MakeFromColorTemperature(m_Temperature);
		color.x *= temperature.x;
		color.y *= temperature.y;
		color.z *= temperature.z;
	}

	color.x *= brightness;
	color.y *= brightness;
	color.z *= brightness;
	return color;
}

XMFLOAT3 ULightComponent::MakeFromColorTemperature(float TemperatureKelvin)
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
