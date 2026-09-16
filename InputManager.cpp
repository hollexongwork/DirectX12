#include "Main.h"
#include "Input.h"
#include "Mouse.h"
#include "InputManager.h"

#include "ImGUI/imgui.h"

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

	// ImGui の入力要求 (直前の NewFrame で確定した値 = 1 フレーム前のホバー / アクティブ状態)。
	// コンテキストは RenderManager が生成済みだが、最初の NewFrame 前は両方 false。
	{
		const bool imguiReady = ImGui::GetCurrentContext() != nullptr;
		m_bMouseCapturedByUI = imguiReady && ImGui::GetIO().WantCaptureMouse;
		m_bKeyboardCapturedByUI = imguiReady && ImGui::GetIO().WantCaptureKeyboard;
	}

	// ---- ビューポートのドラッグ所有 ----
	// ImGui 外で右 / 中ボタンを押し始めた時だけ所有を開始し、両ボタンが離れるまで保持する
	// (ドラッグ中に ImGui ウィンドウの上を通過しても途切れない)。
	// ImGui 上で押し始めたドラッグはボタンを離すまで無視する。
	const bool dragButtonHeld = IsRightClick() || IsMiddleClick();
	if (!dragButtonHeld)
	{
		m_bViewportDragActive = false;
	}
	else if (!m_bViewportDragActive && !m_bMouseCapturedByUI && (IsRightClickTrigger() || IsMiddleClickTrigger()))
	{
		m_bViewportDragActive = true;
	}

	Mouse_SetMode(m_bViewportDragActive ? MOUSE_POSITION_MODE_RELATIVE : MOUSE_POSITION_MODE_ABSOLUTE);
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

	// 相対移動量 / ホイール差分はフレーム内で累積し、ここでゼロに戻す
	// (次のフレームまでに届く WM_INPUT / WM_MOUSEWHEEL がまた加算していく)。
	Mouse_EndOfInputFrame();
}
