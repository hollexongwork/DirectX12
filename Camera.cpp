#include "Main.h"
#include "Input.h"
#include "Mouse.h"
#include "InputManager.h"
#include "GameManager.h"
#include "Time.h"
#include "Camera.h"
#include "DirectxMathOperators.h"

#include <algorithm>

// ============================================================
//  ACameraActor
//  FEditorViewportClient の入力処理 (UpdateCameraMovement /
//  OnDollyPerspectiveCamera / OnChangeCameraSpeed) を
//  FEditorCameraController への入力に変換する。
// ============================================================

ACameraActor::ACameraActor()
{
	m_CameraComponent = CreateDefaultSubobject<UCameraComponent>();

	SetActorRotation({ XMConvertToRadians(20.0f), 0.0f, 0.0f });
	SetActorLocation({ 0.0f, 7.0f, -15.0f });
}

float ACameraActor::GetCameraSpeedScale(int SpeedSetting)
{
	static const float Speed[MaxCameraSpeeds] = { 0.033333f, 0.1f, 0.33f, 1.0f, 3.0f, 8.0f, 16.0f, 32.0f };

	const int speedToUse = std::clamp(SpeedSetting, 1, MaxCameraSpeeds);
	return Speed[speedToUse - 1];
}

float ACameraActor::GetFlightSpeed() const
{
	const FCameraControllerConfig& config = m_CameraController.GetConfig();
	const float terminalVelocity = (config.MovementVelocityDampingAmount > 0.0f)
		? config.MovementAccelerationRate / config.MovementVelocityDampingAmount
		: config.MaximumMovementSpeed;

	return terminalVelocity * GetCameraSpeedScale(m_ViewportSettings.CameraSpeed) * m_ViewportSettings.CameraSpeedScalar;
}

float ACameraActor::GetScrollDollyDistance() const
{
	return ScrollDollyStep * std::clamp(m_ViewportSettings.MouseScrollCameraSpeed, 1, MaxMouseScrollCameraSpeed);
}


// ------------------------------------------------------------
//  Tick: 入力 -> インパルス -> シミュレーション -> トランスフォーム
// ------------------------------------------------------------
void ACameraActor::Tick(float DeltaTime)
{
	// エディタカメラはゲーム時間のスケール / ポーズに影響されない (UE のエディタビューポートと同じ)
	UNREFERENCED_PARAMETER(DeltaTime);
	const float dt = Time::GetUnscaledDeltaTime();

	const InputManager* input = GameManager::GetInstance()->GetInputManager();

	Mouse_State mouse;
	Mouse_GetState(&mouse);

	FCameraControllerUserImpulseData impulse;

	// InputManager が ImGui 外で開始した右 / 中ドラッグだけを相対座標モードにする
	const bool bViewportDrag = (mouse.positionMode == MOUSE_POSITION_MODE_RELATIVE) && input->IsViewportDragActive();
	const bool bFlightCameraInputMode = bViewportDrag && mouse.rightButton;	// UE: IsFlightCameraInputModeActive

	// ---- マウスルック (右ドラッグ) ----
	if (bFlightCameraInputMode)
	{
		const float sensitivity = XMConvertToRadians(m_ViewportSettings.MouseSensitivity);
		const float pitchSign = m_ViewportSettings.bInvertMouseLookYAxis ? -1.0f : 1.0f;

		impulse.YawDelta = (float)mouse.x * sensitivity;
		impulse.PitchDelta = (float)mouse.y * sensitivity * pitchSign;
	}

	// ---- パン (中ドラッグ): 画面の右 / 上方向へ平行移動 (マウスと逆向き = シーンを掴んで動かす) ----
	if (bViewportDrag && mouse.middleButton)
	{
		const float sensitivity = m_ViewportSettings.PanSensitivity;

		impulse.TranslationDelta += -GetActorRightVector() * ((float)mouse.x * sensitivity);
		impulse.TranslationDelta += GetActorUpVector() * ((float)mouse.y * sensitivity);
	}

	// ---- ホイール ----
	// ImGui ウィンドウ上 (WantCaptureMouse) では ImGui のスクロールに譲る。
	// ビューポートがドラッグを所有している間はカーソルが非表示なので常にビューポートへ。
	if (mouse.scrollWheelDelta != 0 && (bViewportDrag || !input->IsMouseCapturedByUI()))
	{
		if (bFlightCameraInputMode)
		{
			// フライト入力中のホイールはカメラ速度段階の変更 (OnChangeCameraSpeed)
			OnChangeCameraSpeed(mouse.scrollWheelDelta);
		}
		else
		{
			// 視線方向へ 1 ノッチあたり一定距離
			const float notches = (float)mouse.scrollWheelDelta / (float)WHEEL_DELTA;
			impulse.TranslationDelta += GetActorForwardVector() * (notches * GetScrollDollyDistance());
		}
	}

	// ---- キーボード (フライト) ----
	// ImGui のテキスト入力 / アクティブ項目 (WantCaptureKeyboard) 中は無視する。
	if (!input->IsKeyboardCapturedByUI())
	{
		if (Input::GetKeyPress('W')) impulse.MoveForwardBackwardImpulse += 1.0f;
		if (Input::GetKeyPress('S')) impulse.MoveForwardBackwardImpulse -= 1.0f;
		if (Input::GetKeyPress('D')) impulse.MoveRightLeftImpulse += 1.0f;
		if (Input::GetKeyPress('A')) impulse.MoveRightLeftImpulse -= 1.0f;
		if (Input::GetKeyPress('E')) impulse.MoveUpDownImpulse += 1.0f;
		if (Input::GetKeyPress('Q')) impulse.MoveUpDownImpulse -= 1.0f;
	}

	// ---- シミュレーション ----
	ApplyViewportSettingsToController();

	const float movementSpeedScale = GetCameraSpeedScale(m_ViewportSettings.CameraSpeed) * m_ViewportSettings.CameraSpeedScalar;

	XMFLOAT3 location = GetActorLocation();
	XMFLOAT3 rotation = GetActorRotation();

	m_CameraController.UpdateSimulation(impulse, dt, movementSpeedScale, location, rotation);

	SetActorLocation(location);
	SetActorRotation(rotation);
}


// ------------------------------------------------------------
//  ホイール 1 ノッチで速度段階を 1 段変更する (高分解能ホイールの端数は持ち越す)。
// ------------------------------------------------------------
void ACameraActor::OnChangeCameraSpeed(int WheelDelta)
{
	m_SpeedChangeWheelAccumulator += WheelDelta;

	const int steps = m_SpeedChangeWheelAccumulator / WHEEL_DELTA;
	m_SpeedChangeWheelAccumulator -= steps * WHEEL_DELTA;

	m_ViewportSettings.CameraSpeed = std::clamp(m_ViewportSettings.CameraSpeed + steps, 1, MaxCameraSpeeds);
}

// ImGui / INI で編集される操作設定のうちコントローラ側の項目を毎フレーム同期する
void ACameraActor::ApplyViewportSettingsToController()
{
	FCameraControllerConfig& config = m_CameraController.GetConfig();

	config.RotationSmoothingRate = m_ViewportSettings.bSmoothMouseLook ? m_ViewportSettings.MouseLookSmoothingRate : 0.0f;
	config.TranslationSmoothingRate = m_ViewportSettings.bSmoothPanAndDolly ? m_ViewportSettings.PanDollySmoothingRate : 0.0f;
}
