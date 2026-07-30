#pragma once

class InputManager
{
private:
public:
	InputManager(HWND hWnd);
	~InputManager();

	void Update();

	// フレーム末尾に呼び出し、マウスの前フレームステートを退避する。
	// (クリックトリガー判定のエッジ検出用。Update() より前に呼んではならない)
	void PostUpdate();
};