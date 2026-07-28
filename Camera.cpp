#include "Main.h"
#include "RenderManager.h"
#include "Time.h"
#include "Input.h"
#include "Mouse.h"
#include "Camera.h"
#include "Function.h"
#include "DirectxMathOperators.h"

ACameraActor::ACameraActor()
{
	m_CameraComponent = CreateDefaultSubobject<UCameraComponent>();

	SetActorRotation({ XMConvertToRadians(20.0f), 0.0f, 0.0f });
	SetActorLocation({ 0.0f, 7.0f, -15.0f });
}

void ACameraActor::Tick(float DeltaTime)
{
	Mouse_State mouseState;
	Mouse_GetState(&mouseState);

	if (mouseState.positionMode == MOUSE_POSITION_MODE_RELATIVE)
	{
		if (mouseState.rightButton)  UpdateRotation(mouseState.x, mouseState.y);
		if (mouseState.middleButton) UpdatePan(mouseState.x, mouseState.y);
	}

	UpdateMovement(DeltaTime);
}

void ACameraActor::UpdateRotation(float dx, float dy)
{
	XMFLOAT3 rotation = GetActorRotation();

	rotation.y += dx * ROTATION_SENSITIVITY;
	rotation.x += dy * ROTATION_SENSITIVITY;
	rotation.x = std::clamp(rotation.x,
		XMConvertToRadians(-PITCH_LIMIT),
		XMConvertToRadians(PITCH_LIMIT));

	SetActorRotation(rotation);
}

void ACameraActor::UpdatePan(float dx, float dy)
{
	XMFLOAT3 location = GetActorLocation();

	location += -GetActorRightVector() * dx * PAN_SENSITIVITY;
	location += GetActorUpVector() * dy * PAN_SENSITIVITY;

	SetActorLocation(location);
}

void ACameraActor::UpdateMovement(float dt)
{
	const float speed = MOVE_SPEED * dt;

	XMFLOAT3 location = GetActorLocation();

	if (Input::GetKeyPress('W')) location += GetActorForwardVector() * speed;
	if (Input::GetKeyPress('S')) location -= GetActorForwardVector() * speed;
	if (Input::GetKeyPress('A')) location -= GetActorRightVector() * speed;
	if (Input::GetKeyPress('D')) location += GetActorRightVector() * speed;

	SetActorLocation(location);
}
