#pragma once

// ============================================================
//  InputManager
//  キーボード (Input) とマウス (Mouse) の毎フレーム更新と、
//  ビューポート (カメラ) と ImGui の入力の振り分けを行う。
//
//	ImGui の WantCaptureMouse / WantCaptureKeyboard
//  (前フレームの NewFrame で確定した値) をその代わりに使う:
//    - 右 / 中ボタンのドラッグは ImGui 外で押し始めた時だけビューポートが
//      所有し、その間だけマウスを相対座標モード (カーソル非表示 + Raw Input)
//      にする。ImGui ウィンドウ上で押し始めたドラッグはカメラを動かさない。
//    - ホイール / キーボードは各読み手が IsMouseCapturedByUI /
//      IsKeyboardCapturedByUI で ImGui 使用中かを確認する。
// ============================================================

class InputManager
{
private:
	bool m_bMouseCapturedByUI = false;		// ImGui がマウスを使用中 (ホバー / ドラッグ中)
	bool m_bKeyboardCapturedByUI = false;	// ImGui がキーボードを使用中 (テキスト入力 / アクティブ項目)
	bool m_bViewportDragActive = false;		// 右 / 中ボタンのドラッグをビューポートが所有中

public:
	InputManager(HWND hWnd);
	~InputManager();

	void Update();

	// フレーム末尾に呼び出し、マウスの前フレームステートを退避し、
	// 相対移動量 / ホイール差分をゼロに戻す。
	// (クリックトリガー判定のエッジ検出用。Update() より前に呼んではならない)
	void PostUpdate();

	bool IsMouseCapturedByUI() const { return m_bMouseCapturedByUI; }
	bool IsKeyboardCapturedByUI() const { return m_bKeyboardCapturedByUI; }

	// 右 / 中ボタンのドラッグをビューポート (カメラ) が所有しているか。
	// true の間はマウスが相対座標モードで、Mouse_GetState の x / y が移動量になる。
	bool IsViewportDragActive() const { return m_bViewportDragActive; }
};
