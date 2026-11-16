#include "Main.h"
#include "GameManager.h"
#include "Input.h"
#include "Camera.h"
#include "Sky.h"
#include "Field.h"
#include "Polygon2D.h"
#include "PostProcessVolume.h"
#include "SunLight.h"
#include "BulbLight.h"
#include "DownLight.h"
#include "PanelLight.h"
#include "VolumetricHeightFog.h"
#include "Table.h"
#include "Cat.h"
#include "Lion.h"
#include "Horse.h"
#include "Tree.h"
#include "ProceduralChain.h"
#include "Manny.h"
#include "FogVolume.h"
#include "Cloud.h"
#include "Campfire.h"

GameManager* GameManager::m_Instance = nullptr;

GameManager::GameManager(HWND hWnd, const wchar_t* CmdLine)
	: m_SelfInit(this)
	, m_SceneRenderer(&m_RenderManager)
	, m_InputManager(hWnd)
	, m_TestDriver(CmdLine)
{
	// ---- レベルロード相当: 初期アクターのスポーン (登録のみ) ----
	// 各アクターのメッシュ / マテリアル / ライトの値 / 既定の配置は、それぞれの
	// 専用クラスのコンストラクタが持つ。ここはシーンに何を置くかの一覧だけ。
	//
	// スポーン順 = FScene への登録順 = ベースパスの描画順 = INI の [Actor.N] の添字。
	// SettingsManager は [Actor.N] を添字とクラス名で対応付けるので、
	// 既存の行の順序は変えず、新しいアクターは末尾に足すこと
	m_World.SpawnActor<APostProcessVolume>();		// [0]  グローバルポスプロ (bUnbound)

	m_World.SpawnActor<ASunLight>();				// [1]  太陽 (3 lux)
	m_World.SpawnActor<ABulbLight>();				// [2]  暖色の電球 (800 lm)
	m_World.SpawnActor<ADownLight>();				// [3]  真上からのダウンライト (2000 lm)
	m_World.SpawnActor<APanelLight>();				// [4]  寒色のパネルライト (1500 lm)

	m_World.SpawnActor<ACameraActor>();				// [5]
	m_World.SpawnActor<ASky>();						// [6]
	m_World.SpawnActor<AField>();					// [7]

	m_World.SpawnActor<ATable>();					// [8]  ピクニックテーブル
	m_World.SpawnActor<ACat>();						// [9]  猫の石像
	m_World.SpawnActor<ALion>();					// [10] ライオンの頭像
	m_World.SpawnActor<AHorse>();					// [11] 馬の像

	m_World.SpawnActor<AVolumetricHeightFog>();		// [12] 高さフォグ + Volumetric Fog
	m_World.SpawnActor<ATree>();					// [13] プロシージャルツリー
	m_World.SpawnActor<AProceduralChain>();			// [14] プロシージャルなチェーンのリグ (GPU スキニング)
	m_World.SpawnActor<AManny>();					// [15] マネキン
	m_World.SpawnActor<AFogVolume>();				// [16] Volume マテリアルの霧 (Volumetric Fog へボクセル化)
	m_World.SpawnActor<ACloud>();					// [17] Heterogeneous Volume の積雲
	m_World.SpawnActor<ACampfire>();				// [18] 焚き火 (Heterogeneous Volume の炎 + ライト)

	//m_World.SpawnActor<APolygon2D>();				// 2D オーバーレイ (使う場合は必ず最後にスポーン)
}

GameManager::~GameManager()
{
	// 終了時に ImGui で編集したパラメータを自動保存 (CPU 値のみ)。
	// テストドライバ実行中は SetSaveEnabled(false) で何も書かない。
	m_SettingsManager.SaveCurrent();

	// 記録済み (最後の Present で実行済み) で未書き出しの F9 / ボタンのキャプチャを書き出す
	// (無ければ何もしない。テストドライバは Finish で書き出し済み)
	m_SceneRenderer.FlushScreenshots();

	m_RenderManager.WaitGPU();
}

void GameManager::Begin()
{
	m_World.BeginPlay();

	// 設定の永続化: コード初期値をスナップショット (Reset の戻り先) した後、
	// Saved/Config/EngineSettings.ini が存在すれば読み込んで適用する。
	// Lumen Params と ImGui のレイアウト (ウィンドウ表示フラグ) もここで復元。
	m_SettingsManager.Initialize(&m_World, &m_SceneRenderer, &m_ImGuiManager);

	// テストドライバ (-taatest): INI 適用後に設定を上書きし、INI 保存停止 /
	// 固定 dt / カメラ入力停止 / ログ作成を行う (通常起動では何もしない)
	m_TestDriver.OnBegin(m_World, m_SceneRenderer, m_SettingsManager);

	m_ImGuiManager.Start();
}

void GameManager::Update()
{
	m_Time.Update();
	m_InputManager.Update();

	// F9: スクリーンショット (UI 無しのバックバッファ -> Saved/Screenshots/*.bmp)。
	// ImGui のテキスト入力 / アクティブ項目 (WantCaptureKeyboard) 中は無視。
	// テストドライバ実行中はキャプチャをドライバが管理するので無効。
	if (!m_TestDriver.IsActive() && !m_InputManager.IsKeyboardCapturedByUI() && Input::GetKeyTrigger(VK_F9))
	{
		m_SceneRenderer.RequestScreenshot("");
	}

	// テストドライバ: シナリオのカメラ / アクタートランスフォームとキャプチャ要求。
	// World.Tick より前に適用する (ASky::Tick がこのフレームのカメラ位置を追従するため)
	m_TestDriver.PreWorldTick(m_SceneRenderer);

	// APostProcessVolume はワールド内アクターになったので、
	// grain / EV 更新は World.Tick 内の Tick() で行われる。
	m_World.Tick(Time::GetDeltaTime());

	// 変更されたレンダーステート / トランスフォームをプロキシへ反映
	// (UWorld::SendAllEndOfFrameUpdates)
	m_World.SendAllEndOfFrameUpdates();

	// マウスの前フレームステート退避(クリックトリガーのエッジ検出用)。
	// 必ず全ゲームコードの入力読み取り後 = フレーム末尾で行うこと。
	m_InputManager.PostUpdate();
}

void GameManager::Draw()
{
	// ---- FSceneView 構築 (ゲーム側フェーズ) ----
	// アクティブカメラ + APostProcessVolume をフレームに 1 回だけ
	// 値スナップショットへ解決し、レンダラのパス列へ渡す。
	// 以後レンダラはゲームオブジェクトに一切触れない。
	const float aspectRatio =
		(float)m_RenderManager.GetBackBufferWidth() / m_RenderManager.GetBackBufferHeight();
	const FSceneView sceneView = m_World.CalcSceneView(aspectRatio);

	// FDeferredShadingSceneRenderer::Render に相当するパス列。
	m_SceneRenderer.BeginFrame();                                     // RHI 準備 + G-Buffer オープン + ImGui NewFrame
	m_SceneRenderer.RenderBasePass(m_World.GetScene(), sceneView);    // ビュー/環境定数 + プリミティブ -> G-Buffer
	m_SceneRenderer.RenderVelocities(m_World.GetScene());             // 動いたプリミティブのベロシティ -> Velocity (TAA / 可視化で必要な時のみ)
	m_SceneRenderer.RenderShadowDepths(m_World.GetScene(), sceneView);// CSM + ローカルシャドウ深度 -> シャドウマップ
	m_SceneRenderer.RenderLumenScene(m_World.GetScene());             // Lumen: カードキャプチャ + Surface Cache ライティング (Emissive 光源化)
	m_SceneRenderer.RenderLighting();                                 // LinearDepth + デファード -> SceneColor
	m_SceneRenderer.RenderTranslucency(m_World.GetScene());           // Translucent/Additive -> SceneColor (後→前フォワード合成)
	m_SceneRenderer.RenderPostProcessing();                           // SceneColor 履歴 (Lumen) -> DOF -> Temporal AA -> AutoExposure -> Bloom -> LUT -> Tonemap (+ スクリーンショットのコピー)
	if (!m_TestDriver.IsActive())
	{
		m_ImGuiManager.Draw();                                        // UI 構築 (描画は EndFrame 内)。テストドライバ実行中は構築しない
	}
	m_SceneRenderer.EndFrame();                                       // ImGui 描画 + Present

	// テストドライバ: ログ 1 行 / 最終フレームならキャプチャを書き出して終了要求
	m_TestDriver.PostFrame(m_SceneRenderer, m_RenderManager);
}
