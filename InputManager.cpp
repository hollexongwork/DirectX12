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

	if (IsRightClick() || IsMiddleClick())
	{
		Mouse_SetMode(MOUSE_POSITION_MODE_RELATIVE);
	}
	else
	{
		Mouse_SetMode(MOUSE_POSITION_MODE_ABSOLUTE);
	}
}

void InputManager::PostUpdate()
{
	// マウスの前フレームステート退避はフレーム末尾(全ゲームコードの入力読み取り後)に行う。
	//
	// マウスの gState はウィンドウメッセージ(WndProc)経由で随時更新されるため、
	// Update() の先頭で退避すると World.Tick が読む時点で常に gPrevState == gState となり、
	// IsLeftClickTrigger / IsRightClickTrigger のエッジ検出が永久に false になる。
	// (キーボードは Input::Update 内で「旧退避 → GetKeyboardState で新規取得」の順なので問題ない)
	Mouse_UpdatePrevState();
}