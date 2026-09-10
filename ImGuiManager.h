#pragma once


class ImGuiManager
{
private:
	class FSceneRenderer* m_SceneRenderer = nullptr;

	class UWorld* m_World = nullptr;
	class APostProcessVolume* m_PostProcess = nullptr;
	class ColorGradingLUTBaker* m_LUTBaker = nullptr;
	class AutoExposure* m_AutoExposure = nullptr;
	class SettingsManager* m_Settings = nullptr;

	// ---- Outliner / Details 選択状態 ----
	class AActor* m_SelectedActor = nullptr;
	class UActorComponent* m_SelectedComponent = nullptr;
	char                   m_LabelBuffer[128] = {};

	// ---- メインメニューバー ----
	// ENG キーボードの "-" キー (VK_OEM_MINUS) で表示/非表示をトグルする。
	// バー非表示中でも各ウィンドウの表示状態 (m_bShow*) は保持される。
	bool m_bShowMainMenuBar = true;

	// ---- ウィンドウ表示フラグ (メニューバーの Edit / Debug から切り替え) ----
	// Edit  : シーン編集用パネル (Outliner + Details を 1 ウィンドウに統合)
	// Debug : レンダラのデバッグ表示 (G-Buffer / Light Grid / Lumen / Culling)
	bool m_bShowOutliner = true;
	bool m_bShowGBuffer = true;
	bool m_bShowLightGrid = true;
	bool m_bShowLumen = true;
	bool m_bShowCulling = true;

	void UpdateMenuBarToggle();
	void MainMenuBar();
	void EditMenu();
	void DebugMenu();

	void BufferWindow();
	void LightGridWindow();
	void LumenWindow();
	void CullingWindow();
	// ---- Outliner / Details 統合ウィンドウ ----
	// 上段 = Outliner (アクター一覧)、下段 = Details (選択アクターのプロパティ)。
	// 中央のスプリッタをドラッグして上下の比率を変更できる。
	float m_OutlinerSplitRatio = 0.35f;   // ウィンドウ内容領域に対する Outliner ペインの高さ比

	void OutlinerWindow();               // 統合ウィンドウ本体 (Begin/End + 2 ペイン + スプリッタ)
	void DrawOutlinerSection();          // 上段: アクター一覧 (ウィンドウを開かない)
	void DrawDetailsSection();           // 下段: 選択アクターの Details (ウィンドウを開かない)

	// ---- Outliner / Details ヘルパ ----
	void SelectActor(class AActor* Actor);
	void ValidateSelection();
	void DrawComponentTree(class AActor* Actor);
	void DrawComponentTreeNode(class USceneComponent* Component);

	// ---- Details セクション描画 ----
	void DrawTransformSection(class USceneComponent* Component);
	void DrawPrimitiveSection(class UPrimitiveComponent* Component);
	void DrawCameraSection(class UCameraComponent* Component);
	void DrawStaticMeshSection(class UStaticMeshComponent* Component);
	void DrawFieldQuadSection(class UFieldQuadComponent* Component);
	void DrawPolygon2DSection(class UPolygon2DComponent* Component);
	void DrawPostProcessVolumeSection(class APostProcessVolume* Volume);

	// ライト共通プロパティ (Lights ウィンドウと Details で共用)
	void DrawLightComponentSection(class ULightComponent* Light);

	// マテリアル 1 スロット分のエディタ。変更があれば true を返す
	// (呼び出し側で MarkRenderStateDirty すること)。
	bool DrawMaterialEditor(class Material& Mat);

public:
	ImGuiManager();
	~ImGuiManager() = default;

	void Start();
	void Draw();

};