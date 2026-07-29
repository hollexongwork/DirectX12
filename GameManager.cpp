#include "Main.h"
#include "GameManager.h"
#include "ImGUI/imgui.h"

#include "Camera.h"
#include "Sky.h"
#include "Field.h"
#include "StaticMeshActor.h"
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

	// ---- スタティックメッシュ配置 (AStaticMeshActor) ----
	// 旧 ATable / ACat / ALion / AHorse (同型ボイラープレート) は
	// UE5 の AStaticMeshActor へ統合し、レベルロード相当のここで
	// メッシュ / テクスチャ / トランスフォーム / ラベルを構成する。
	// スポーン順 (Table -> Cat -> Lion -> Horse) は旧実装のまま。
	{
		// 旧 ACat / AHorse / ATable が設定していた共通マテリアル値
		auto SetDefaultLitMaterial = [](Material& material)
		{
			material.Params.BaseColor = { 0.5f, 0.0f, 0.5f, 1.0f };
			material.Params.EmissionColor = { 0.0f, 0.0f, 0.0f, 0.0f };
			material.Params.Metallic = 0.0f;
			material.Params.Specular = 0.0f;
			material.Params.Roughness = 1.0f;
			material.Params.NormalWeight = 1.0f;
			material.Params.Unlit = FALSE;
		};

		// テーブル (旧 ATable)
		AStaticMeshActor* table = m_World.SpawnActor<AStaticMeshActor>();
		table->SetActorLabel("Table");
		{
			UStaticMeshComponent* mesh = table->GetStaticMeshComponent();
			mesh->SetStaticMesh("Asset/Model/wooden_picnic_table_4k.fbx");
			mesh->SetNumMaterialSlots(2);

			table->SetActorLocation({ 0.0f, 0.0f, 0.0f });
			table->SetActorRotation({ XMConvertToRadians(90.0f), XMConvertToRadians(90.0f), 0.0f });
			table->SetActorScale3D({ 5.0f, 5.0f, 5.0f });

			mesh->SetBaseColorTexture(0, "Asset/Texture/wooden_picnic_table_bottom_diff_4k.dds");
			mesh->SetNormalTexture(0, "Asset/Texture/wooden_picnic_table_bottom_nor_dx_4k.dds");
			mesh->SetARMTexture(0, "Asset/Texture/wooden_picnic_table_bottom_arm_4k.dds");

			mesh->SetBaseColorTexture(1, "Asset/Texture/wooden_picnic_table_top_diff_4k.dds");
			mesh->SetNormalTexture(1, "Asset/Texture/wooden_picnic_table_top_nor_dx_4k.dds");
			mesh->SetARMTexture(1, "Asset/Texture/wooden_picnic_table_top_arm_4k.dds");

			for (unsigned int i = 0; i < 2; ++i)
			{
				SetDefaultLitMaterial(mesh->GetMaterial(i));
			}
			mesh->MarkRenderStateDirty();	// GetMaterial() 直接書き換えの反映
		}

		// 猫の石像 (旧 ACat)
		AStaticMeshActor* cat = m_World.SpawnActor<AStaticMeshActor>();
		cat->SetActorLabel("Cat");
		{
			UStaticMeshComponent* mesh = cat->GetStaticMeshComponent();
			mesh->SetStaticMesh("Asset/Model/concrete_cat_statue_4k.fbx");

			cat->SetActorLocation({ -1.0f, 3.75f, 0.0f });
			cat->SetActorRotation({ 1.57f, 0.0f, 0.0f });
			cat->SetActorScale3D({ 7.0f, 7.0f, 7.0f });

			mesh->SetBaseColorTexture(0, "Asset/Texture/concrete_cat_statue_diff_4k.dds");
			mesh->SetNormalTexture(0, "Asset/Texture/concrete_cat_statue_nor_dx_4k.dds");
			mesh->SetARMTexture(0, "Asset/Texture/concrete_cat_statue_arm_4k.dds");

			SetDefaultLitMaterial(mesh->GetMaterial(0));
			mesh->MarkRenderStateDirty();	// GetMaterial() 直接書き換えの反映
		}

		// ライオンの頭像 (旧 ALion)
		AStaticMeshActor* lion = m_World.SpawnActor<AStaticMeshActor>();
		lion->SetActorLabel("Lion");
		{
			UStaticMeshComponent* mesh = lion->GetStaticMeshComponent();
			mesh->SetStaticMesh("Asset/Model/lion_head_4k.fbx");

			lion->SetActorLocation({ 2.0f, 3.75f, 1.0f });
			lion->SetActorRotation({ XMConvertToRadians(90.0f), 0.0f, 0.0f });
			lion->SetActorScale3D({ 7.0f, 7.0f, 7.0f });

			mesh->SetBaseColorTexture(0, "Asset/Texture/lion_head_diff_4k.dds");
			mesh->SetNormalTexture(0, "Asset/Texture/lion_head_nor_dx_4k.dds");
			mesh->SetARMTexture(0, "Asset/Texture/lion_head_arm_4k.dds");

			// マテリアルはスロット既定値 (旧 ALion と同じくデフォルトのまま)
		}

		// 馬の像 (旧 AHorse)
		AStaticMeshActor* horse = m_World.SpawnActor<AStaticMeshActor>();
		horse->SetActorLabel("Horse");
		{
			UStaticMeshComponent* mesh = horse->GetStaticMeshComponent();
			mesh->SetStaticMesh("Asset/Model/horse_statue_01_4k.fbx");

			horse->SetActorLocation({ -1.0f, 3.7f, 0.0f });
			horse->SetActorRotation({ 1.57f, 0.0f, 0.0f });
			horse->SetActorScale3D({ 10.0f, 10.0f, 10.0f });

			mesh->SetBaseColorTexture(0, "Asset/Texture/horse_statue_01_diff_4k.dds");
			mesh->SetNormalTexture(0, "Asset/Texture/horse_statue_01_nor_dx_4k.dds");
			mesh->SetARMTexture(0, "Asset/Texture/horse_statue_01_arm_4k.dds");

			SetDefaultLitMaterial(mesh->GetMaterial(0));
			mesh->MarkRenderStateDirty();	// GetMaterial() 直接書き換えの反映
		}
	}

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
