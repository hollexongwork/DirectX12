#include "Main.h"
#include "Input.h"
#include "Mouse.h"
#include "GameManager.h"
#include "ImGUI/imgui.h"

#include "Camera.h"
#include "Sky.h"
#include "Field.h"
#include "Table.h"
#include "Cat.h"
#include "Lion.h"
#include "Horse.h"
#include "Polygon2D.h"
#include "PostProcessVolume.h"
#include "DirectionalLight.h"
#include "PointLight.h"
#include "SpotLight.h"
#include "RectLight.h"

GameManager* GameManager::m_Instance = nullptr;

GameManager::GameManager(HWND hWnd)
	: m_SelfInit(this)
	, m_SceneRenderer(&m_RenderManager)
	, m_InputManager(hWnd)
{
	m_Instance = this;

	// ---- レベルロード相当: 初期アクターのスポーン ----
	// (スポーン順 = FScene への登録順 = ベースパスの描画順)
	m_World.SpawnActor<APostProcessVolume>();	// グローバルポスプロ (bUnbound)

	// ---- ライト (ALight アクター + ULightComponent) ----
	{
		// ディレクショナル: 旧 FScene 既定値 (方向 (3,5,-3) / 強度 3) と同じ見た目。
		// 既定は 10 lux , Lights パネルで調整できる
		ADirectionalLight* sun = m_World.SpawnActor<ADirectionalLight>();
		sun->SetActorRotation(ULightComponentBase::DirectionToRotator({ -3.0f, -5.0f, 3.0f }));	// 発光方向 = 旧 LightDirection の逆
		sun->GetLightComponent()->SetIntensity(3.0f);	// lux

		// ポイント: 暖色の電球 (800 lm ≒ 60W 白熱球)
		APointLight* pointLight = m_World.SpawnActor<APointLight>();
		pointLight->SetActorLocation({ -2.5f, 5.0f, -2.0f });
		pointLight->GetPointLightComponent()->SetIntensity(800.0f);		// lm
		pointLight->GetPointLightComponent()->SetSourceRadius(0.05f);
		pointLight->GetPointLightComponent()->SetUseTemperature(true);
		pointLight->GetPointLightComponent()->SetTemperature(3000.0f);	// 電球色

		// スポット: 真上からのダウンライト
		ASpotLight* spotLight = m_World.SpawnActor<ASpotLight>();
		spotLight->SetActorLocation({ 2.0f, 8.0f, 1.0f });	// 向きはコンストラクタ既定 (真下)
		spotLight->GetSpotLightComponent()->SetIntensity(2000.0f);		// lm
		spotLight->GetSpotLightComponent()->SetOuterConeAngle(30.0f);
		spotLight->GetSpotLightComponent()->SetInnerConeAngle(15.0f);

		// レクト: 寒色のパネルライト (-Z を向けてシーン中央へ)
		ARectLight* rectLight = m_World.SpawnActor<ARectLight>();
		rectLight->SetActorLocation({ 0.0f, 5.0f, 4.0f });
		rectLight->SetActorRotation({ XMConvertToRadians(15.0f), XMConvertToRadians(180.0f), 0.0f });
		rectLight->GetRectLightComponent()->SetIntensity(1500.0f);		// lm
		rectLight->GetRectLightComponent()->SetSourceWidth(1.5f);
		rectLight->GetRectLightComponent()->SetSourceHeight(1.0f);
		rectLight->GetRectLightComponent()->SetUseTemperature(true);
		rectLight->GetRectLightComponent()->SetTemperature(9000.0f);	// 寒色
	}

	m_World.SpawnActor<ACameraActor>();
	m_World.SpawnActor<ASky>();
	m_World.SpawnActor<AField>();
	m_World.SpawnActor<ATable>();
	m_World.SpawnActor<ACat>();
	m_World.SpawnActor<ALion>();
	m_World.SpawnActor<AHorse>();
	//m_World.SpawnActor<APolygon2D>();	// 2D オーバーレイ (使う場合は必ず最後にスポーン)
}

GameManager::~GameManager()
{
	// 終了時に ImGui で編集したパラメータを自動保存 (CPU 値のみ)。
	m_SettingsManager.SaveCurrent();

	m_RenderManager.WaitGPU();
}

void GameManager::Begin()
{
	m_World.BeginPlay();

	// 設定の永続化: コード初期値をスナップショット (Reset の戻り先) した後、
	// Saved/Config/EngineSettings.ini が存在すれば読み込んで適用する。
	m_SettingsManager.Initialize(&m_World, &m_SceneRenderer);

	m_ImGuiManager.Start();
}

void GameManager::Update()
{
	m_Time.Update();
	m_InputManager.Update();

	// APostProcessVolume はワールド内アクターになったので、
	// grain / EV 更新は World.Tick 内の Tick() で行われる。
	m_World.Tick(Time::GetDeltaTime());

	// 変更されたレンダーステート / トランスフォームをプロキシへ反映
	// (UWorld::SendAllEndOfFrameUpdates)
	m_World.SendAllEndOfFrameUpdates();
}

void GameManager::Draw()
{
	// FDeferredShadingSceneRenderer::Render に相当するパス列。
	m_SceneRenderer.BeginFrame();                          // RHI 準備 + G-Buffer オープン + ImGui NewFrame
	m_SceneRenderer.RenderBasePass(m_World.GetScene());    // ビュー/環境定数 + プリミティブ -> G-Buffer
	m_SceneRenderer.RenderShadowDepths(m_World.GetScene());// CSM + ローカルシャドウ深度 -> シャドウマップ
	m_SceneRenderer.RenderLighting();                      // LinearDepth + デファード -> SceneColor
	m_SceneRenderer.RenderTranslucency(m_World.GetScene());// Translucent/Additive -> SceneColor (後→前フォワード合成)
	m_SceneRenderer.RenderPostProcessing();                // DOF -> AutoExposure -> Bloom -> LUT -> Tonemap
	m_ImGuiManager.Draw();                                 // UI 構築 (描画は EndFrame 内)
	m_SceneRenderer.EndFrame();                            // ImGui 描画 + Present
}
