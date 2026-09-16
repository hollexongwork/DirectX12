#pragma once
#include "Actor.h"
#include "CameraComponent.h"
#include "CameraController.h"

// ============================================================
//  ACameraActor
//  UCameraComponent を Root に持ち、カメラ操作を Tick で行う。
//  ビュー定数のアップロードは FScene 側の責務。
//
//  操作:
//    右ドラッグ           : マウスルック (ピッチ / ヨー)
//    中ドラッグ           : パン (画面の右 / 上方向へ平行移動)
//    ホイール             : ドリー (ズーム)
//    右ドラッグ + ホイール : フライト速度段階 (Camera Speed) の変更
//    W / S, A / D, E / Q  : 前後 / 左右 / 上下 (右ドラッグ中でなくても有効)
//
//  移動は FEditorCameraController の物理ベース (加速 / 減衰) で、
//  ルック / パン / ドリーは指数追従で滑らかにする。
//  ImGui がマウス / キーボードを使用中の入力は無視する (InputManager)。
// ============================================================

class ACameraActor : public AActor
{
public:
	struct FLevelEditorViewportSettings
	{
		// ---- フライト (WASD / EQ) ----
		int   CameraSpeed = 4;					// 速度段階 1..MaxCameraSpeeds。右ドラッグ中のホイールでも変更
		float CameraSpeedScalar = 1.0f;			// 速度段階に掛けるスカラー

		// ---- ホイールドリー ----
		int   MouseScrollCameraSpeed = 3;		// 1..10。1 ノッチの移動量 = 段階 x ScrollDollyStep

		// ---- マウスルック (右ドラッグ) ----
		float MouseSensitivity = 0.1f;			// [度 / カウント]
		bool  bInvertMouseLookYAxis = false;	

		// ---- パン (中ドラッグ) ----
		float PanSensitivity = 0.005f;			// [m / カウント]

		// ---- スムージング ----
		bool  bSmoothMouseLook = true;
		float MouseLookSmoothingRate = 25.0f;	// [1/s] 大きいほど追従が速い (時定数 40ms)
		bool  bSmoothPanAndDolly = true;
		float PanDollySmoothingRate = 12.0f;	// [1/s] (時定数 83ms)
	};

	static constexpr int   MaxCameraSpeeds = 8;			
	static constexpr int   MaxMouseScrollCameraSpeed = 10;
	static constexpr float ScrollDollyStep = 0.5f;		// [m / ノッチ] MouseScrollCameraSpeed = 1 のとき

	// 速度段階 -> 速度スケール
	static float GetCameraSpeedScale(int SpeedSetting);

private:
	UCameraComponent* m_CameraComponent = nullptr;

	FEditorCameraController        m_CameraController;
	FLevelEditorViewportSettings   m_ViewportSettings;

	int m_SpeedChangeWheelAccumulator = 0;	// 速度段階変更用のホイール端数 [WHEEL_DELTA 単位]

	void OnChangeCameraSpeed(int WheelDelta);
	void ApplyViewportSettingsToController();

public:
	ACameraActor();

	void Tick(float DeltaTime) override;

	UCameraComponent* GetCameraComponent() const { return m_CameraComponent; }

	FLevelEditorViewportSettings& GetViewportSettings() { return m_ViewportSettings; }
	const FLevelEditorViewportSettings& GetViewportSettings() const { return m_ViewportSettings; }

	// 現在のフライト終端速度 [m/s] (ImGui 表示用)
	float GetFlightSpeed() const;

	// ホイール 1 ノッチのドリー移動量 [m] (ImGui 表示用)
	float GetScrollDollyDistance() const;
};
