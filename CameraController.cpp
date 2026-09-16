#include "Main.h"
#include "CameraController.h"
#include "DirectxMathOperators.h"

#include <algorithm>
#include <cmath>

// ============================================================
//  FEditorCameraController
// ============================================================

namespace
{
	// 未放出量のうち今フレーム放出する割合。Rate = 0 なら全量 (即時)。
	// 1 - exp(-Rate * dt) はフレームレートに依らず同じ時定数で収束する。
	float ReleaseFraction(float Rate, float DeltaTime)
	{
		if (Rate <= 0.0f)
			return 1.0f;
		return 1.0f - expf(-Rate * DeltaTime);
	}

	// [-PI, PI) へ正規化 (yaw の浮動小数の蓄積を防ぐ)
	float NormalizeRadians(float Angle)
	{
		Angle = fmodf(Angle + XM_PI, XM_2PI);
		if (Angle < 0.0f)
			Angle += XM_2PI;
		return Angle - XM_PI;
	}

	constexpr float KINDA_SMALL_NUMBER = 1.0e-4f;
}


// ------------------------------------------------------------
//  UpdateSimulation
//  回転 -> 位置の順に更新する (移動インパルスは更新後の向きで解決)。
// ------------------------------------------------------------
void FEditorCameraController::UpdateSimulation(const FCameraControllerUserImpulseData& UserImpulseData, float DeltaTime, float MovementSpeedScale,
	XMFLOAT3& InOutCameraPosition, XMFLOAT3& InOutCameraRotation)
{
	if (DeltaTime <= 0.0f)
		return;

	UpdateRotation(UserImpulseData, DeltaTime, InOutCameraRotation);
	UpdatePosition(UserImpulseData, MovementSpeedScale, DeltaTime, InOutCameraPosition, InOutCameraRotation);
}

void FEditorCameraController::ResetVelocity()
{
	m_MovementVelocity = { 0.0f, 0.0f, 0.0f };
	m_PendingTranslation = { 0.0f, 0.0f, 0.0f };
	m_PendingPitch = 0.0f;
	m_PendingYaw = 0.0f;
}


// ------------------------------------------------------------
//  回転: マウスルックの差分を未放出量へ積み、指数追従で放出する。
//  ピッチは「現在値 + 未放出量」(= 目標値) の段階で制限に収めるので、
//  制限に当たった分の入力は捨てられ、放出後に制限を超えることはない。
// ------------------------------------------------------------
void FEditorCameraController::UpdateRotation(const FCameraControllerUserImpulseData& UserImpulse, float DeltaTime, XMFLOAT3& InOutCameraRotation)
{
	const float minPitch = XMConvertToRadians(m_Config.MinimumAllowedPitchRotation);
	const float maxPitch = XMConvertToRadians(m_Config.MaximumAllowedPitchRotation);

	const float targetPitch = std::clamp(InOutCameraRotation.x + m_PendingPitch + UserImpulse.PitchDelta, minPitch, maxPitch);
	m_PendingPitch = targetPitch - InOutCameraRotation.x;
	m_PendingYaw += UserImpulse.YawDelta;

	const float fraction = ReleaseFraction(m_Config.RotationSmoothingRate, DeltaTime);

	float pitchStep = m_PendingPitch * fraction;
	float yawStep = m_PendingYaw * fraction;

	// 残りが十分小さければ使い切る (漸近で永久に微小回転し続けない)
	constexpr float snapThreshold = 1.0e-5f;	// [rad] (約 0.0006 度)
	if (fabsf(m_PendingPitch - pitchStep) < snapThreshold) pitchStep = m_PendingPitch;
	if (fabsf(m_PendingYaw - yawStep) < snapThreshold)     yawStep = m_PendingYaw;

	InOutCameraRotation.x = std::clamp(InOutCameraRotation.x + pitchStep, minPitch, maxPitch);
	InOutCameraRotation.y = NormalizeRadians(InOutCameraRotation.y + yawStep);

	m_PendingPitch -= pitchStep;
	m_PendingYaw -= yawStep;
}


// ------------------------------------------------------------
//  位置: 物理ベース移動 (インパルス -> 加速度 -> 減衰付き速度)
//  + 変位放出 (パン / ドリー)。
// ------------------------------------------------------------
void FEditorCameraController::UpdatePosition(const FCameraControllerUserImpulseData& UserImpulse, float MovementSpeedScale, float DeltaTime,
	XMFLOAT3& InOutCameraPosition, const XMFLOAT3& CameraRotation)
{
	// ---- ワールド空間の加速度 ----
	XMVECTOR worldSpaceAcceleration;
	{
		// 前後 / 左右はカメラのローカル空間で、上下はワールド空間で適用する
		const XMVECTOR localSpaceImpulse = XMVectorSet(UserImpulse.MoveRightLeftImpulse, 0.0f, UserImpulse.MoveForwardBackwardImpulse, 0.0f);
		const XMMATRIX cameraOrientation = XMMatrixRotationRollPitchYaw(CameraRotation.x, CameraRotation.y, CameraRotation.z);

		XMVECTOR worldSpaceImpulse = XMVector3TransformNormal(localSpaceImpulse, cameraOrientation);
		worldSpaceImpulse = XMVectorAdd(worldSpaceImpulse, XMVectorSet(0.0f, UserImpulse.MoveUpDownImpulse, 0.0f, 0.0f));

		// 斜め入力が軸方向より速くならないよう大きさを 1.0 で頭打ちにする
		const float impulseLength = XMVectorGetX(XMVector3Length(worldSpaceImpulse));
		if (impulseLength > 1.0f)
		{
			worldSpaceImpulse = XMVectorScale(worldSpaceImpulse, 1.0f / impulseLength);
		}

		worldSpaceAcceleration = XMVectorScale(worldSpaceImpulse, m_Config.MovementAccelerationRate * MovementSpeedScale);
	}

	// ---- 物理ベース移動: 加速 + 一次減衰 ----
	// dv/dt = a - k v の厳密解:
	//   v(t+dt) = v_t + (v - v_t) exp(-k dt),  v_t = a / k (終端速度)
	//   dx      = v_t dt + (v - v_t) (1 - exp(-k dt)) / k
	// 終端速度が a/k (1 - k dt) とフレームレートに依存するため厳密解
	// (60fps と 300fps で速度が 15% 変わるのを避ける)。
	const XMVECTOR previousVelocity = XMLoadFloat3(&m_MovementVelocity);
	XMVECTOR velocity;
	XMVECTOR displacement;
	{
		const float damping = m_Config.MovementVelocityDampingAmount;
		if (damping > 0.0f)
		{
			const XMVECTOR terminalVelocity = XMVectorScale(worldSpaceAcceleration, 1.0f / damping);
			const XMVECTOR excess = XMVectorSubtract(previousVelocity, terminalVelocity);
			const float decay = expf(-damping * DeltaTime);

			velocity = XMVectorAdd(terminalVelocity, XMVectorScale(excess, decay));
			displacement = XMVectorAdd(XMVectorScale(terminalVelocity, DeltaTime), XMVectorScale(excess, (1.0f - decay) / damping));
		}
		else
		{
			velocity = XMVectorAdd(previousVelocity, XMVectorScale(worldSpaceAcceleration, DeltaTime));
			displacement = XMVectorAdd(XMVectorScale(previousVelocity, DeltaTime), XMVectorScale(worldSpaceAcceleration, 0.5f * DeltaTime * DeltaTime));
		}
	}

	// ---- 最大速度で制限 (制限された場合はその速度で等速移動) ----
	const float maxSpeed = m_Config.MaximumMovementSpeed * MovementSpeedScale;
	const float speed = XMVectorGetX(XMVector3Length(velocity));
	if (speed > maxSpeed && speed > 0.0f)
	{
		velocity = XMVectorScale(velocity, maxSpeed / speed);
		displacement = XMVectorScale(velocity, DeltaTime);
	}

	// 十分遅くなったら止める (減衰の漸近で微小移動し続けない)
	if (speed < KINDA_SMALL_NUMBER)
	{
		velocity = XMVectorZero();
	}

	XMStoreFloat3(&m_MovementVelocity, velocity);

	// ---- 位置更新 ----
	XMVECTOR position = XMLoadFloat3(&InOutCameraPosition);
	position = XMVectorAdd(position, displacement);

	// ---- 変位放出 (パン / ホイールドリー) ----
	{
		XMVECTOR pending = XMLoadFloat3(&m_PendingTranslation);
		pending = XMVectorAdd(pending, XMLoadFloat3(&UserImpulse.TranslationDelta));

		XMVECTOR step = XMVectorScale(pending, ReleaseFraction(m_Config.TranslationSmoothingRate, DeltaTime));

		const float remaining = XMVectorGetX(XMVector3Length(XMVectorSubtract(pending, step)));
		if (remaining < KINDA_SMALL_NUMBER)
		{
			step = pending;
		}

		position = XMVectorAdd(position, step);
		XMStoreFloat3(&m_PendingTranslation, XMVectorSubtract(pending, step));
	}

	XMStoreFloat3(&InOutCameraPosition, position);
}
