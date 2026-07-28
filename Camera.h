#pragma once
#include "Actor.h"
#include "CameraComponent.h"

// ============================================================
//  ACameraActor
//  UCameraComponent を Root に持ち、
//  エディタ風の軌道 / パン / フライ操作 (旧 Camera を移植) を
//  Tick で行う。ビュー定数のアップロードは FScene 側の責務。
// ============================================================

class ACameraActor : public AActor
{
private:
	static constexpr float PAN_SENSITIVITY = 0.05f;
	static constexpr float ROTATION_SENSITIVITY = 0.01f;
	static constexpr float MOVE_SPEED = 15.0f;
	static constexpr float PITCH_LIMIT = 89.0f;

	UCameraComponent* m_CameraComponent = nullptr;

	void UpdateRotation(float dx, float dy);
	void UpdatePan(float dx, float dy);
	void UpdateMovement(float dt);

public:
	ACameraActor();

	void Tick(float DeltaTime) override;

	UCameraComponent* GetCameraComponent() const { return m_CameraComponent; }
};
