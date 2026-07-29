#include "Main.h"
#include "Input.h"
#include "Mouse.h"
#include "InputManager.h"

InputManager::InputManager(HWND hWnd)
{
	Input::Init();
	Mouse_Initialize(hWnd);
	Mouse_SetMode(MOUSE_POSITION_MODE_ABSOLUTE);
}

InputManager::~InputManager()
{
	Mouse_SetMode(MOUSE_POSITION_MODE_ABSOLUTE);
	Mouse_Finalize();
	Input::Uninit();
}

void InputManager::Update()
{
	Input::Update();
	Mouse_UpdatePrevState();

	if (IsRightClick()||IsMiddleClick())
	{
		Mouse_SetMode(MOUSE_POSITION_MODE_RELATIVE);
	}
	else
	{
		Mouse_SetMode(MOUSE_POSITION_MODE_ABSOLUTE);
	}
}