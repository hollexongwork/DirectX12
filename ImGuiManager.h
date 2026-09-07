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
	class AActor*          m_SelectedActor = nullptr;
	class UActorComponent* m_SelectedComponent = nullptr;
	char                   m_LabelBuffer[128] = {};

	void BufferWindow();
	void LightGridWindow();
	void LumenWindow();
	void CullingWindow();
	void OutlinerWindow();
	void DetailsWindow();

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
