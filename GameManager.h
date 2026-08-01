#pragma once

#include "RenderManager.h"
#include "AssetManager.h"
#include "SceneRenderer.h"
#include "Time.h"
#include "InputManager.h"
#include "World.h"

#include "ImGuiManager.h"
#include "SettingsManager.h"

// ============================================================
//  GameManager
//  UEngine + UGameInstance に相当する薄いエンジン層。
//  各サブシステム (RHI / SceneRenderer / Time / Input / ImGui) と
//  UWorld を所有する。
//  Phase 2: 描画のオーケストレーションは FSceneRenderer
//  (FDeferredShadingSceneRenderer 相当) に移管。RenderManager は
//  RHI 層 (デバイス / ヒープ / PSO / バインド API) に純化した。
// ============================================================

class GameManager
{
private:
	static GameManager* m_Instance;

	struct SelfInitializer
	{
		SelfInitializer(GameManager* self)
		{
			GameManager::m_Instance = self;
		}
	} m_SelfInit;

	// RHI 層 (FD3D12DynamicRHI 相当)。SceneRenderer より先に構築される。
	RenderManager m_RenderManager;

	// アセットキャッシュ (UStaticMesh / UTexture2D 共有に相当)。
	// RenderManager の直後に宣言すること (破棄は宣言の逆順なので、
	// キャッシュ内アセットの GPU リソースが RenderManager の
	// 遅延削除キューへ安全に回る)。
	FAssetManager m_AssetManager;

	// フレームオーケストレータ (FDeferredShadingSceneRenderer 相当)。
	FSceneRenderer m_SceneRenderer;

	Time m_Time;
	InputManager m_InputManager;
	ImGuiManager m_ImGuiManager;

	// UWorld アクターと FScene を所有する。
	UWorld m_World;

	// ImGui パラメータの永続化 (Saved/Config/EngineSettings.ini)。
	// 起動時に読み込み・終了時に自動保存する。
	SettingsManager m_SettingsManager;

public:
	static GameManager* GetInstance() { return m_Instance; }

	GameManager(HWND hWnd);
	~GameManager();

	UWorld* GetWorld() { return &m_World; }
	FAssetManager* GetAssetManager() { return &m_AssetManager; }
	FSceneRenderer* GetSceneRenderer() { return &m_SceneRenderer; }
	SettingsManager* GetSettingsManager() { return &m_SettingsManager; }

	void Begin();
	void Update();
	void Draw();
};
