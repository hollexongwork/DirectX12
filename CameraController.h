#pragma once
#include <DirectXMath.h>
#include <cfloat>

using namespace DirectX;

// ============================================================
//  FEditorCameraController
//  エディタビューポートのフライトカメラ (WASD / EQ) を物理ベースで
//  シミュレーションする:
//    入力インパルス -> 加速度 -> 速度 (減衰) -> 位置
//  単位は m / 秒。
//
//  拡張:
//    - TranslationDelta : パン / ホイールドリーの変位 [m] を
//                         指数追従で数フレームかけて放出する
//    - Pitch / YawDelta : マウスルックの回転差分 [rad] を同様に放出する
//  いずれも SmoothingRate = 0 で UE と同じ即時適用になる。
//  「未放出量」を保持する方式なので、入力が連続しても総移動量 /
//  総回転量は入力の積算と厳密に一致する (フレームレート非依存)。
// ============================================================

struct FCameraControllerConfig
{
	// ---- 移動 (既定: 20000 cm/s^2 / 減衰 10 / 上限なし) ----
	// 終端速度 = MovementAccelerationRate / MovementVelocityDampingAmount
	//          = 20 m/s x MovementSpeedScale
	float MovementAccelerationRate = 200.0f;		// [m/s^2] インパルス 1.0 あたりの加速度
	float MovementVelocityDampingAmount = 10.0f;	// [1/s] 速度減衰
	float MaximumMovementSpeed = FLT_MAX;			// [m/s] (x MovementSpeedScale)

	// ---- ピッチ制限 (既定 -90..90 度。ジンバルロック回避で 89 度) ----
	float MinimumAllowedPitchRotation = -89.0f;		// [度]
	float MaximumAllowedPitchRotation = 89.0f;		// [度]

	// ---- 拡張: 変位 / 回転差分の指数追従レート [1/s] (0 = 即時) ----
	// 時定数 = 1 / Rate (12 -> 約 83ms で 63% 放出、25 -> 約 40ms)
	float TranslationSmoothingRate = 12.0f;
	float RotationSmoothingRate = 25.0f;
};

struct FCameraControllerUserImpulseData
{
	// ---- 移動インパルス (-1..1)。前後 / 左右はローカル空間、上下はワールド空間 ----
	float MoveForwardBackwardImpulse = 0.0f;
	float MoveRightLeftImpulse = 0.0f;
	float MoveUpDownImpulse = 0.0f;

	// ---- 拡張: 今フレーム要求された積分済みの変位 / 回転差分 ----
	XMFLOAT3 TranslationDelta = { 0.0f, 0.0f, 0.0f };	// [m] ワールド空間
	float    PitchDelta = 0.0f;							// [rad]
	float    YawDelta = 0.0f;							// [rad]
};

class FEditorCameraController
{
private:
	FCameraControllerConfig m_Config;

	XMFLOAT3 m_MovementVelocity = { 0.0f, 0.0f, 0.0f };		// [m/s] ワールド空間
	XMFLOAT3 m_PendingTranslation = { 0.0f, 0.0f, 0.0f };	// [m] 未放出の変位
	float    m_PendingPitch = 0.0f;							// [rad] 未放出の回転差分
	float    m_PendingYaw = 0.0f;

	void UpdateRotation(const FCameraControllerUserImpulseData& UserImpulse, float DeltaTime, XMFLOAT3& InOutCameraRotation);
	void UpdatePosition(const FCameraControllerUserImpulseData& UserImpulse, float MovementSpeedScale, float DeltaTime,
		XMFLOAT3& InOutCameraPosition, const XMFLOAT3& CameraRotation);

public:
	FCameraControllerConfig& GetConfig() { return m_Config; }
	const FCameraControllerConfig& GetConfig() const { return m_Config; }

	// 1 フレーム分のシミュレーション。
	// 位置 [m] と回転 [rad, x = pitch / y = yaw / z = roll] を更新する。
	// MovementSpeedScale = フライト速度段階 (ACameraActor::GetCameraSpeed) x スカラー。
	void UpdateSimulation(const FCameraControllerUserImpulseData& UserImpulseData, float DeltaTime, float MovementSpeedScale,
		XMFLOAT3& InOutCameraPosition, XMFLOAT3& InOutCameraRotation);

	// 速度と未放出量を破棄する (外部からのテレポート後など)
	void ResetVelocity();

	const XMFLOAT3& GetMovementVelocity() const { return m_MovementVelocity; }
};
