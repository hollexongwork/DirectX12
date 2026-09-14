#include "Main.h"
#include "RenderManager.h"
#include "ImGUI/imgui_impl_dx12.h"
#include "ImGUI/imgui.h"
#include "ImGuiManager.h"
#include "GameManager.h"
#include "Camera.h"
#include "CameraComponent.h"
#include "Polygon2D.h"
#include "Field.h"
#include "StaticMeshComponent.h"
#include "PostProcessVolume.h"
#include "SceneRenderer.h"
#include "ShadowRendering.h"
#include "LightGridInjection.h"
#include "LumenScene.h"
#include "ColorGradingLUTBaker.h"
#include "AutoExposure.h"
#include "World.h"
#include "Light.h"
#include "LightComponent.h"
#include "SettingsManager.h"
#include "Input.h"

#include <filesystem>
#include <cctype>

using namespace ImGui;

// ---- 大文字小文字を無視した部分一致 (Outliner フィルタ用) ----
static bool ContainsCaseInsensitive(const std::string& Haystack, const char* Needle)
{
	if (Needle == nullptr || Needle[0] == '\0')
		return true;

	std::string h = Haystack;
	std::string n = Needle;
	for (auto& c : h) c = (char)tolower((unsigned char)c);
	for (auto& c : n) c = (char)tolower((unsigned char)c);

	return h.find(n) != std::string::npos;
}

ImGuiManager::ImGuiManager()
{

}

void ImGuiManager::Start()
{
	m_SceneRenderer = GameManager::GetInstance()->GetSceneRenderer();

	// ワールドからアクターを検索する
	UWorld* world = GameManager::GetInstance()->GetWorld();
	m_World = world;
	m_PostProcess = world->GetActorOfClass<APostProcessVolume>();
	m_LUTBaker = m_SceneRenderer->GetColorGradingLUTBaker();
	m_AutoExposure = m_SceneRenderer->GetAutoExposure();
	m_Settings = GameManager::GetInstance()->GetSettingsManager();
}


void ImGuiManager::Draw()
{
	// "-" キーによるメニューバーのトグル (ImGui NewFrame 後・ウィンドウ構築前)
	UpdateMenuBarToggle();

	if (m_Layout.bShowMainMenuBar)
	{
		MainMenuBar();
	}

	// ---- Edit ----
	if (m_Layout.bShowOutliner)  OutlinerWindow();   // Outliner (上) + Details (下)

	// ---- Debug ----
	if (m_Layout.bShowGBuffer)   BufferWindow();
	if (m_Layout.bShowLightGrid) LightGridWindow();
	if (m_Layout.bShowLumen)     LumenWindow();
	if (m_Layout.bShowCulling)   CullingWindow();
}


// ============================================================
//  メインメニューバー
// ============================================================

// ENG キーボードの "-" キー (VK_OEM_MINUS) でメニューバーの表示/非表示をトグル。
// ImGui のテキスト入力 (Outliner のフィルタ等) にフォーカスがある間は
// "-" の打鍵を入力として優先し、トグルしない。
void ImGuiManager::UpdateMenuBarToggle()
{
	if (ImGui::GetIO().WantTextInput)
		return;

	if (Input::GetKeyTrigger(VK_OEM_MINUS))
	{
		m_Layout.bShowMainMenuBar = !m_Layout.bShowMainMenuBar;
	}
}

void ImGuiManager::MainMenuBar()
{
	if (!ImGui::BeginMainMenuBar())
		return;

	if (ImGui::BeginMenu("Edit"))
	{
		EditMenu();
		ImGui::EndMenu();
	}

	if (ImGui::BeginMenu("Debug"))
	{
		DebugMenu();
		ImGui::EndMenu();
	}

	// 右端にトグルキーのヒントを表示
	{
		const char* hint = "[-] Hide Menu Bar";
		const float  width = ImGui::CalcTextSize(hint).x + ImGui::GetStyle().ItemSpacing.x;
		ImGui::SameLine(ImGui::GetWindowWidth() - width);
		ImGui::TextDisabled("%s", hint);
	}

	ImGui::EndMainMenuBar();
}

// Edit: シーン編集用パネルの表示切替 + 設定の保存/リセット
void ImGuiManager::EditMenu()
{
	ImGui::MenuItem("Outliner / Details", nullptr, &m_Layout.bShowOutliner);

	ImGui::Separator();

	if (ImGui::MenuItem("Save Settings", nullptr, false, m_Settings != nullptr))
	{
		m_Settings->SaveCurrent();
	}

	if (ImGui::BeginMenu("Reset to Default", m_Settings != nullptr))
	{
		if (ImGui::MenuItem("Selected Actor", nullptr, false, m_SelectedActor != nullptr))
		{
			m_Settings->ResetActor(m_SelectedActor);
			strncpy_s(m_LabelBuffer, m_SelectedActor->GetActorLabel().c_str(), _TRUNCATE);
		}
		if (ImGui::MenuItem("All Actors"))     m_Settings->ResetAllActors();
		if (ImGui::MenuItem("All Lights"))     m_Settings->ResetAllLights();
		if (ImGui::MenuItem("Post Process"))   m_Settings->ResetPostProcess();
		if (ImGui::MenuItem("Auto Exposure"))  m_Settings->ResetAutoExposure();
		if (ImGui::MenuItem("Lumen"))          m_Settings->ResetLumen();
		if (ImGui::MenuItem("Debug Windows"))  m_Settings->ResetImGuiLayout();

		ImGui::Separator();

		if (ImGui::MenuItem("Everything"))     m_Settings->ResetAll();

		ImGui::EndMenu();
	}
}

// Debug: レンダラのデバッグウィンドウの表示切替
void ImGuiManager::DebugMenu()
{
	ImGui::MenuItem("G-Buffer", nullptr, &m_Layout.bShowGBuffer);
	ImGui::MenuItem("Light Grid", nullptr, &m_Layout.bShowLightGrid);
	ImGui::MenuItem("Lumen", nullptr, &m_Layout.bShowLumen);
	ImGui::MenuItem("Culling", nullptr, &m_Layout.bShowCulling);

	ImGui::Separator();

	if (ImGui::MenuItem("Show All"))
	{
		m_Layout.bShowGBuffer = m_Layout.bShowLightGrid = m_Layout.bShowLumen = m_Layout.bShowCulling = true;
	}
	if (ImGui::MenuItem("Hide All"))
	{
		m_Layout.bShowGBuffer = m_Layout.bShowLightGrid = m_Layout.bShowLumen = m_Layout.bShowCulling = false;
	}
}


void ImGuiManager::BufferWindow()
{
	ImGui::Begin("G-Buffer", &m_Layout.bShowGBuffer);

	ImGui::Text("GBufferC (BaseColor)");
	ImGui::Image((void*)m_SceneRenderer->GetSceneTextures()->GBufferC->SRVHandle.ptr, ImVec2(200.0f, 100.0f));

	ImGui::Text("GBufferA (Normal)");
	ImGui::Image((void*)m_SceneRenderer->GetSceneTextures()->GBufferA->SRVHandle.ptr, ImVec2(200.0f, 100.0f));

	ImGui::Text("GBufferB (MSR/AO)");
	ImGui::Image((void*)m_SceneRenderer->GetSceneTextures()->GBufferB->SRVHandle.ptr, ImVec2(200.0f, 100.0f));

	ImGui::Text("LinearDepth");
	ImGui::Image((void*)m_SceneRenderer->GetSceneTextures()->LinearDepthDisplaySRVHandle.ptr, ImVec2(200.0f, 100.0f));

	ImGui::End();
}

// ============================================================
//  ライトグリッド (タイルドライトカリング) デバッグウィンドウ
// ============================================================
void ImGuiManager::LightGridWindow()
{
	ImGui::Begin("Light Grid", &m_Layout.bShowLightGrid);

	FLightGridInjection* grid = m_SceneRenderer ? m_SceneRenderer->GetLightGrid() : nullptr;
	if (grid == nullptr)
	{
		ImGui::TextUnformatted("Light grid is not available.");
		ImGui::End();
		return;
	}

	// ---- 統計 ----
	ImGui::Text("Grid Size : %u x %u x %u (%u cells)",
		grid->GetGridSizeX(), grid->GetGridSizeY(), grid->GetGridSizeZ(), grid->GetNumCells());
	ImGui::Text("Tile      : %u px / Max %u lights per cell",
		LIGHT_GRID_PIXEL_SIZE, MAX_CULLED_LIGHTS_PER_CELL);

	ImGui::Separator();

	// ---- 制御 ----
	FLightGridInjection::Params& params = grid->GetParams();

	ImGui::Checkbox("Use Light Grid", &params.bUseLightGrid);
	if (!params.bUseLightGrid)
	{
		ImGui::SameLine();
		ImGui::TextDisabled("(fallback: all-lights loop)");
	}

	const char* debugModes[] = { "Off", "Light Complexity", "Z Slices" };
	int debugMode = (int)params.DebugMode;
	if (ImGui::Combo("Debug View", &debugMode, debugModes, 3))
	{
		params.DebugMode = (unsigned int)debugMode;
	}

	ImGui::End();
}


// ============================================================
//  Lumen (Surface Cache / スクリーン GI) デバッグウィンドウ
// ============================================================
void ImGuiManager::LumenWindow()
{
	ImGui::Begin("Lumen", &m_Layout.bShowLumen);

	FLumenSceneData* lumen = m_SceneRenderer ? m_SceneRenderer->GetLumenScene() : nullptr;
	if (lumen == nullptr)
	{
		ImGui::TextUnformatted("Lumen scene is not available.");
		ImGui::End();
		return;
	}

	FLumenSceneData::Params& params = lumen->GetParams();
	const FLumenSceneData::Stats& stats = lumen->GetStats();

	// ---- 統計 ----
	ImGui::Text("Objects    : %u / %u", stats.NumObjects, MAX_LUMEN_OBJECTS);
	ImGui::Text("Cards      : %u valid / %u pending capture",
		stats.NumValidCards, stats.NumPendingCaptures);
	ImGui::Text("Captured   : %u this frame", stats.NumCapturedThisFrame);
	ImGui::Text("Atlas      : %u x %u (%u px cards)",
		LUMEN_ATLAS_WIDTH, LUMEN_ATLAS_HEIGHT, LUMEN_CARD_RESOLUTION);
	ImGui::Text("Probes     : %u x %u (16px, octa 8x8)",
		stats.NumProbesX, stats.NumProbesY);

	if (lumen->IsHardwareRayTracingSupported())
	{
		ImGui::Text("HWRT       : %s (%u TLAS instances)",
			stats.bHardwareRayTracingActive ? "ACTIVE (RayQuery)" : "idle (SWRT)",
			stats.NumTLASInstances);
	}
	else
	{
		ImGui::TextDisabled("HWRT       : not supported (SWRT: Mesh SDF + Global SDF)");
	}

	ImGui::Separator();

	// ---- 制御 ----
	ImGui::Checkbox("Enable Lumen", &params.bEnabled);

	const char* gatherModes[] = { "Off", "Per-Pixel Cone Trace", "Screen Probe Gather" };
	ImGui::Combo("Gather Mode", &params.GatherMode, gatherModes, 3);

	if (lumen->IsHardwareRayTracingSupported())
	{
		ImGui::Checkbox("Hardware Ray Tracing (DXR)", &params.bUseHardwareRayTracing);
	}

	ImGui::SliderFloat("GI Intensity", &params.GIIntensity, 0.0f, 4.0f);
	ImGui::SliderFloat("Emissive Boost", &params.EmissiveBoost, 0.0f, 8.0f);
	ImGui::SliderFloat("Max Trace Distance", &params.MaxTraceDistance, 1.0f, 100.0f);
	ImGui::SliderFloat("Surface Bias", &params.SurfaceBias, 0.0f, 0.3f);
	ImGui::SliderFloat("Sky Occlusion", &params.SkyOcclusionStrength, 0.0f, 1.0f);

	// Debug View は永続化対象外 (SettingsManager の [Lumen] に書かない。毎回 Off で起動)
	const char* debugModes[] = { "Off", "GI Radiance", "Sky Visibility", "GI Diffuse" };
	int debugMode = (int)params.DebugMode;
	if (ImGui::Combo("Debug View", &debugMode, debugModes, 4))
	{
		params.DebugMode = (unsigned int)debugMode;
	}
	ImGui::SameLine();
	ImGui::TextDisabled("(not saved)");

	// ---- Surface Cache ----
	if (ImGui::CollapsingHeader("Surface Cache"))
	{
		ImGui::SliderInt("Radiosity Rays", &params.NumRadiosityRays, 1, 16);
		ImGui::SliderInt("Radiosity Cards/Frame", &params.RadiosityCardsPerFrame, 1, (int)MAX_LUMEN_CARDS);
		ImGui::SliderFloat("Radiosity Temporal Alpha", &params.RadiosityTemporalAlpha, 0.02f, 1.0f);
		ImGui::SliderInt("Capture Budget/Frame", &params.CaptureBudgetPerFrame, 1, 64);
	}

	// ---- トレース (Global SDF / Screen Trace) ----
	if (ImGui::CollapsingHeader("Tracing"))
	{
		ImGui::Checkbox("Global Distance Field", &params.bGlobalSDF);
		ImGui::SliderFloat("Clipmap0 Extent [m]", &params.GlobalSDFExtent0, 4.0f, 50.0f);
		ImGui::SliderFloat("Detail Trace Distance [m]", &params.DetailTraceDistance, 0.5f, 10.0f);
		ImGui::Checkbox("Screen Space Trace", &params.bScreenSpaceTrace);
		if (params.DebugMode != 0)
		{
			// DebugMode 中は SceneColor がデバッグ画像に置き換わるため、その履歴を
			// 採光するスクリーントレースは C++ 側 (MakeBasePassParams) で無効化される
			ImGui::SameLine();
			ImGui::TextDisabled("(off while Debug View)");
		}
		ImGui::SliderFloat("Screen Trace Thickness [m]", &params.ScreenTraceThickness, 0.05f, 1.0f);
		ImGui::SliderInt("Per-Pixel Cones", &params.NumScreenCones, 1, 8);
	}

	// ---- Screen Probe Gather ----
	if (ImGui::CollapsingHeader("Screen Probe Gather"))
	{
		ImGui::SliderFloat("Temporal Alpha", &params.TemporalAlpha, 0.02f, 1.0f);
		ImGui::SliderFloat("Screen Temporal Alpha", &params.ScreenTemporalAlpha, 0.02f, 1.0f);
		ImGui::Checkbox("Probe Placement Jitter", &params.bProbeJitter);
		ImGui::SliderFloat("Sky Sample Mip", &params.SkySampleMip, 0.0f, 4.0f);
	}

	// ---- Reflections ----
	if (ImGui::CollapsingHeader("Reflections"))
	{
		ImGui::Checkbox("Enable Reflections", &params.bReflections);
		ImGui::SliderFloat("Max Roughness", &params.ReflectionMaxRoughness, 0.05f, 1.0f);
		ImGui::SliderFloat("Fade Start", &params.ReflectionFadeStart, 0.0f, 1.0f);
		ImGui::SliderFloat("Reflection Intensity", &params.ReflectionIntensity, 0.0f, 2.0f);
	}

	// ---- Radiance Cache / Translucency GI ----
	if (ImGui::CollapsingHeader("Radiance Cache"))
	{
		ImGui::Checkbox("Enable Radiance Cache", &params.bRadianceCache);
		ImGui::Checkbox("Translucency GI", &params.bTranslucencyGI);
		ImGui::SliderFloat("Translucency GI Intensity", &params.TranslucencyGIIntensity, 0.0f, 4.0f);
		ImGui::SliderFloat("Probe Spacing [m]", &params.RadianceCacheSpacing, 0.25f, 4.0f);
		ImGui::SliderInt("Probes/Frame", &params.RadianceCacheProbesPerFrame, 16, 1024);
	}

	// ---- プレビュー ----
	if (ImGui::CollapsingHeader("Surface Cache Atlas"))
	{
		const float previewWidth = 320.0f;
		const float previewHeight = previewWidth *
			(float)LUMEN_ATLAS_HEIGHT / (float)LUMEN_ATLAS_WIDTH;

		ImGui::Text("Albedo");
		ImGui::Image((void*)lumen->GetAlbedoAtlasSRVHandle().ptr, ImVec2(previewWidth, previewHeight));

		ImGui::Text("Normal (card space)");
		ImGui::Image((void*)lumen->GetNormalAtlasSRVHandle().ptr, ImVec2(previewWidth, previewHeight));

		ImGui::Text("Emissive");
		ImGui::Image((void*)lumen->GetEmissiveAtlasSRVHandle().ptr, ImVec2(previewWidth, previewHeight));

		ImGui::Text("Final Lighting (Direct + Radiosity + Emissive)");
		ImGui::Image((void*)lumen->GetFinalLightingSRVHandle().ptr, ImVec2(previewWidth, previewHeight));
	}

	if (ImGui::CollapsingHeader("Screen GI Buffers"))
	{
		const float previewWidth = 320.0f;

		ImGui::Text("Probe Radiance (filtered)");
		ImGui::Image((void*)lumen->GetProbeRadianceSRVHandle().ptr, ImVec2(previewWidth, 180.0f));

		ImGui::Text("Diffuse Indirect (integrated)");
		ImGui::Image((void*)lumen->GetDiffuseIndirectSRVHandle().ptr, ImVec2(previewWidth, 180.0f));

		ImGui::Text("Reflections");
		ImGui::Image((void*)lumen->GetReflectionSRVHandle().ptr, ImVec2(previewWidth, 180.0f));
	}

	ImGui::End();
}


void ImGuiManager::CullingWindow()
{
	ImGui::Begin("Culling", &m_Layout.bShowCulling);

	if (m_SceneRenderer == nullptr)
	{
		ImGui::TextUnformatted("Scene renderer is not available.");
		ImGui::End();
		return;
	}

	// ---- 制御 ----
	FSceneRenderer::FCullingParams& params = m_SceneRenderer->GetCullingParams();

	ImGui::Checkbox("Frustum Culling", &params.bEnableFrustumCulling);
	if (!params.bEnableFrustumCulling)
	{
		ImGui::SameLine();
		ImGui::TextDisabled("(disabled: draw everything)");
	}

	ImGui::Checkbox("Freeze Frustum", &params.bFreezeFrustum);
	ImGui::SameLine();
	ImGui::TextDisabled(params.bFreezeFrustum
		? "(frozen: fly the camera to inspect culling)"
		: "(r.FreezeRendering)");

	ImGui::Separator();

	// ---- ビュー統計 (ComputeViewVisibility が毎フレーム更新) ----
	const FSceneRenderer::FCullingStats& stats = m_SceneRenderer->GetCullingStats();
	ImGui::Text("Primitives Processed : %d", stats.NumProcessed);
	ImGui::Text("  Visible            : %d", stats.NumVisible);
	ImGui::Text("  Frustum Culled     : %d", stats.NumFrustumCulled);
	ImGui::Text("  Distance Culled    : %d", stats.NumDistanceCulled);

	// ---- シャドウ統計 (全シャドウビュー累積) ----
	if (FShadowSceneRenderer* shadowRenderer = m_SceneRenderer->GetShadowRenderer())
	{
		const FShadowSceneRenderer::FShadowCullingStats& shadowStats =
			shadowRenderer->GetCullingStats();

		ImGui::Separator();
		ImGui::Text("Shadow Views         : %d", shadowStats.NumViews);
		ImGui::Text("  Casters Processed  : %d", shadowStats.NumProcessed);
		ImGui::Text("  Casters Drawn      : %d", shadowStats.NumDrawn);
		ImGui::Text("  Casters Culled     : %d", shadowStats.NumCulled);
	}

	ImGui::End();
}


// ============================================================
//  ライト共通プロパティ (Lights ウィンドウ / Details 共用)
// ============================================================
void ImGuiManager::DrawLightComponentSection(ULightComponent* Light)
{
	if (!Light)
		return;

	ULightComponent* light = Light;

	// ---- 共通プロパティ ----
	bool affectsWorld = light->GetAffectsWorld();
	if (Checkbox("Affects World", &affectsWorld))
	{
		light->SetAffectsWorld(affectsWorld);
	}

	// ---- シャドウ (全ライト共通) ----
	bool castShadows = light->GetCastShadows();
	if (Checkbox("Cast Shadows", &castShadows))
	{
		light->SetCastShadows(castShadows);
	}
	if (castShadows)
	{
		float shadowBias = light->GetShadowBias();
		if (DragFloat("Shadow Bias", &shadowBias, 0.01f, 0.0f, 10.0f))
		{
			light->SetShadowBias(shadowBias);
		}

		float shadowSlopeBias = light->GetShadowSlopeBias();
		if (DragFloat("Shadow Slope Bias", &shadowSlopeBias, 0.01f, 0.0f, 10.0f))
		{
			light->SetShadowSlopeBias(shadowSlopeBias);
		}

		bool dfShadows = light->GetUseRayTracedDistanceFieldShadows();
		if (Checkbox("RayTraced DF Shadows", &dfShadows))
		{
			light->SetUseRayTracedDistanceFieldShadows(dfShadows);
		}
	}

	// ---- Directional (CSM) ----
	if (auto* directional = dynamic_cast<UDirectionalLightComponent*>(light))
	{
		if (directional->GetCastShadows())
		{
			float shadowDistance = directional->GetDynamicShadowDistance();
			if (DragFloat("Dynamic Shadow Distance (m)", &shadowDistance, 1.0f, 5.0f, 500.0f))
			{
				directional->SetDynamicShadowDistance(shadowDistance);
			}

			int cascades = directional->GetDynamicShadowCascades();
			if (SliderInt("Shadow Cascades", &cascades, 1, 4))
			{
				directional->SetDynamicShadowCascades(cascades);
			}

			float exponent = directional->GetCascadeDistributionExponent();
			if (DragFloat("Cascade Distribution Exponent", &exponent, 0.05f, 1.0f, 5.0f))
			{
				directional->SetCascadeDistributionExponent(exponent);
			}

			float fade = directional->GetShadowDistanceFadeoutFraction();
			if (SliderFloat("Shadow Fade Fraction", &fade, 0.0f, 0.5f))
			{
				directional->SetShadowDistanceFadeoutFraction(fade);
			}

			if (directional->GetUseRayTracedDistanceFieldShadows())
			{
				float dfDistance = directional->GetDistanceFieldShadowDistance();
				if (DragFloat("DF Shadow Distance (m)", &dfDistance, 1.0f, 10.0f, 2000.0f))
				{
					directional->SetDistanceFieldShadowDistance(dfDistance);
				}

				float dfTrace = directional->GetDistanceFieldTraceDistance();
				if (DragFloat("DF Trace Distance (m)", &dfTrace, 1.0f, 1.0f, 1000.0f))
				{
					directional->SetDistanceFieldTraceDistance(dfTrace);
				}

				float srcAngle = directional->GetLightSourceAngle();
				if (DragFloat("Light Source Angle (deg)", &srcAngle, 0.05f, 0.05f, 20.0f))
				{
					directional->SetLightSourceAngle(srcAngle);
				}
			}
		}
	}

	float intensity = light->GetIntensity();
	if (DragFloat("Intensity", &intensity, 10.0f, 0.0f, 1000000.0f))
	{
		light->SetIntensity(intensity);
	}

	XMFLOAT4 color = light->GetLightColor();
	if (ColorEdit3("Light Color", &color.x))
	{
		light->SetLightColor(color);
	}

	bool useTemperature = light->GetUseTemperature();
	if (Checkbox("Use Temperature", &useTemperature))
	{
		light->SetUseTemperature(useTemperature);
	}
	if (useTemperature)
	{
		float temperature = light->GetTemperature();
		if (SliderFloat("Temperature (K)", &temperature, 1500.0f, 15000.0f))
		{
			light->SetTemperature(temperature);
		}
	}

	float specularScale = light->GetSpecularScale();
	if (SliderFloat("Specular Scale", &specularScale, 0.0f, 1.0f))
	{
		light->SetSpecularScale(specularScale);
	}

	// ---- ローカルライト共通 (Point / Spot / Rect) ----
	if (auto* local = dynamic_cast<ULocalLightComponent*>(light))
	{
		Separator();

		float radius = local->GetAttenuationRadius();
		if (DragFloat("Attenuation Radius (m)", &radius, 0.1f, 0.01f, 1000.0f))
		{
			local->SetAttenuationRadius(radius);
		}

		const char* unitNames[] = { "Unitless", "Candelas", "Lumens", "EV" };
		int units = (int)local->GetIntensityUnits();
		if (Combo("Intensity Units", &units, unitNames, 4))
		{
			local->SetIntensityUnits((ELightUnits)units);
		}
	}

	// ---- Point / Spot ----
	if (auto* point = dynamic_cast<UPointLightComponent*>(light))
	{
		float sourceRadius = point->GetSourceRadius();
		if (DragFloat("Source Radius (m)", &sourceRadius, 0.01f, 0.0f, 10.0f))
		{
			point->SetSourceRadius(sourceRadius);
		}

		float softRadius = point->GetSoftSourceRadius();
		if (DragFloat("Soft Source Radius (m)", &softRadius, 0.01f, 0.0f, 10.0f))
		{
			point->SetSoftSourceRadius(softRadius);
		}

		float sourceLength = point->GetSourceLength();
		if (DragFloat("Source Length (m)", &sourceLength, 0.01f, 0.0f, 10.0f))
		{
			point->SetSourceLength(sourceLength);
		}

		bool inverseSquared = point->GetUseInverseSquaredFalloff();
		if (Checkbox("Use Inverse Squared Falloff", &inverseSquared))
		{
			point->SetUseInverseSquaredFalloff(inverseSquared);
		}
		if (!inverseSquared)
		{
			float exponent = point->GetLightFalloffExponent();
			if (DragFloat("Light Falloff Exponent", &exponent, 0.1f, 1.0f, 16.0f))
			{
				point->SetLightFalloffExponent(exponent);
			}
		}
	}

	// ---- Spot ----
	if (auto* spot = dynamic_cast<USpotLightComponent*>(light))
	{
		float inner = spot->GetInnerConeAngle();
		if (SliderFloat("Inner Cone Angle", &inner, 0.0f, 80.0f))
		{
			spot->SetInnerConeAngle(inner);
		}

		float outer = spot->GetOuterConeAngle();
		if (SliderFloat("Outer Cone Angle", &outer, 1.0f, 80.0f))
		{
			spot->SetOuterConeAngle(outer);
		}
	}

	// ---- Rect ----
	if (auto* rect = dynamic_cast<URectLightComponent*>(light))
	{
		float width = rect->GetSourceWidth();
		if (DragFloat("Source Width (m)", &width, 0.01f, 0.01f, 20.0f))
		{
			rect->SetSourceWidth(width);
		}

		float height = rect->GetSourceHeight();
		if (DragFloat("Source Height (m)", &height, 0.01f, 0.01f, 20.0f))
		{
			rect->SetSourceHeight(height);
		}

		float barnAngle = rect->GetBarnDoorAngle();
		if (SliderFloat("Barn Door Angle", &barnAngle, 0.0f, 88.0f))
		{
			rect->SetBarnDoorAngle(barnAngle);
		}

		float barnLength = rect->GetBarnDoorLength();
		if (DragFloat("Barn Door Length (m)", &barnLength, 0.01f, 0.0f, 10.0f))
		{
			rect->SetBarnDoorLength(barnLength);
		}
	}
}

// ============================================================
//  Outliner / Details 統合ウィンドウ
//  上段: Outliner (アクター一覧)  下段: Details (選択アクター)
//  2 つの子領域を縦に並べ、間の水平スプリッタをドラッグで
//  高さ比 (m_Layout.OutlinerSplitRatio) を変更する。
// ============================================================
void ImGuiManager::OutlinerWindow()
{
	if (!m_World)
		return;

	ValidateSelection();

	Begin("Outliner", &m_Layout.bShowOutliner);

	const ImGuiStyle& style = GetStyle();
	const float splitterHeight = 4.0f;
	const float minPaneHeight = GetFrameHeightWithSpacing() * 3.0f;   // 各ペインの最小高さ
	const float availHeight = GetContentRegionAvail().y;

	// ---- 上下ペインの高さ計算 (比率 → ピクセル、両端は最小高さでクランプ) ----
	float outlinerHeight = availHeight * m_Layout.OutlinerSplitRatio;
	const float maxOutlinerHeight = availHeight - splitterHeight - style.ItemSpacing.y * 2.0f - minPaneHeight;
	if (maxOutlinerHeight > minPaneHeight)
	{
		outlinerHeight = (std::max)(minPaneHeight, (std::min)(outlinerHeight, maxOutlinerHeight));
	}

	// ---- 上段: Outliner ----
	TextDisabled("Outliner");
	BeginChild("##OutlinerPane", ImVec2(0.0f, outlinerHeight), true);
	DrawOutlinerSection();
	EndChild();

	// ---- スプリッタ (見えないボタンをドラッグして比率を変更) ----
	InvisibleButton("##OutlinerDetailsSplitter", ImVec2(-1.0f, splitterHeight));
	if (IsItemHovered() || IsItemActive())
	{
		SetMouseCursor(ImGuiMouseCursor_ResizeNS);
	}
	if (IsItemActive() && availHeight > 0.0f)
	{
		outlinerHeight += GetIO().MouseDelta.y;
		if (maxOutlinerHeight > minPaneHeight)
		{
			outlinerHeight = (std::max)(minPaneHeight, (std::min)(outlinerHeight, maxOutlinerHeight));
		}
		m_Layout.OutlinerSplitRatio = outlinerHeight / availHeight;
	}
	// スプリッタの視覚表示 (ホバー / ドラッグ中は強調)
	{
		const ImVec2 min = GetItemRectMin();
		const ImVec2 max = GetItemRectMax();
		const ImU32  col = GetColorU32(IsItemActive() ? ImGuiCol_SeparatorActive :
			IsItemHovered() ? ImGuiCol_SeparatorHovered :
			ImGuiCol_Separator);
		const float  midY = (min.y + max.y) * 0.5f;
		GetWindowDrawList()->AddLine(ImVec2(min.x, midY), ImVec2(max.x, midY), col, 1.0f);
	}

	// ---- 下段: Details (残り全部) ----
	TextDisabled("Details");
	BeginChild("##DetailsPane", ImVec2(0.0f, 0.0f), true);
	DrawDetailsSection();
	EndChild();

	End();
}

// ============================================================
//  Outliner セクション (統合ウィンドウ上段)
//  ワールド内の全アクターをスポーン順に列挙する
//  (World Outliner 相当)。行クリックで Details の対象を選択。
// ============================================================
void ImGuiManager::DrawOutlinerSection()
{
	// ---- 検索フィルタ (ラベル / クラス名の部分一致) ----
	static char filter[64] = {};
	PushItemWidth(-60);
	InputText("Filter", filter, sizeof(filter));
	PopItemWidth();

	Separator();

	const auto& actors = m_World->GetActors();
	int index = 0;
	for (const auto& actorPtr : actors)
	{
		AActor* actor = actorPtr.get();
		if (actor->IsPendingKill())
		{
			++index;
			continue;
		}

		const std::string& label = actor->GetActorLabel();
		std::string typeName = UWorld::GetClassDisplayName(actor);

		if (!ContainsCaseInsensitive(label, filter) &&
			!ContainsCaseInsensitive(typeName, filter))
		{
			++index;
			continue;
		}

		PushID(index);

		// ---- 行本体 (ラベル + クラス名) ----
		char row[256];
		sprintf_s(row, "%s  (%s)", label.c_str(), typeName.c_str());

		bool selected = (actor == m_SelectedActor);
		if (Selectable(row, selected))
		{
			SelectActor(actor);
		}

		PopID();
		++index;
	}
}

// ============================================================
//  Details セクション (統合ウィンドウ下段)
//  選択アクターのラベル / コンポーネントツリー / 選択
//  コンポーネントのプロパティを編集する (Details パネル相当)。
//  編集は全て公開セッター経由 (マテリアルのみ直接編集 +
//  MarkRenderStateDirty) なので次フレームのプロキシへ反映される。
// ============================================================
void ImGuiManager::DrawDetailsSection()
{
	if (!m_SelectedActor)
	{
		TextDisabled("Select an actor in the Outliner.");
		return;
	}

	AActor* actor = m_SelectedActor;

	// ---- アクターヘッダ ----
	Text("Class : %s", UWorld::GetClassDisplayName(actor).c_str());

	if (InputText("Label", m_LabelBuffer, sizeof(m_LabelBuffer),
		ImGuiInputTextFlags_EnterReturnsTrue) ||
		IsItemDeactivatedAfterEdit())
	{
		if (m_LabelBuffer[0] != '\0')
		{
			actor->SetActorLabel(m_LabelBuffer);
		}
	}

	if (m_Settings)
	{
		if (Button("Save Settings"))
		{
			m_Settings->SaveCurrent();
		}
		SameLine();
		if (Button("Reset Actor"))
		{
			m_Settings->ResetActor(actor);
			strncpy_s(m_LabelBuffer, actor->GetActorLabel().c_str(), _TRUNCATE);
		}
	}

	Separator();

	// ---- コンポーネントツリー ----
	DrawComponentTree(actor);

	Separator();

	// ---- 選択コンポーネントのプロパティ ----
	if (UActorComponent* component = m_SelectedComponent)
	{
		Text("%s", UWorld::GetClassDisplayName(component).c_str());
		Separator();

		if (auto* scene = dynamic_cast<USceneComponent*>(component))
		{
			DrawTransformSection(scene);
		}

		if (auto* light = dynamic_cast<ULightComponent*>(component))
		{
			if (CollapsingHeader("Light", ImGuiTreeNodeFlags_DefaultOpen))
			{
				DrawLightComponentSection(light);
			}
		}

		if (auto* camera = dynamic_cast<UCameraComponent*>(component))
		{
			DrawCameraSection(camera);
		}

		if (auto* primitive = dynamic_cast<UPrimitiveComponent*>(component))
		{
			DrawPrimitiveSection(primitive);
		}

		if (auto* mesh = dynamic_cast<UStaticMeshComponent*>(component))
		{
			DrawStaticMeshSection(mesh);
		}

		if (auto* quad = dynamic_cast<UFieldQuadComponent*>(component))
		{
			DrawFieldQuadSection(quad);
		}

		if (auto* polygon = dynamic_cast<UPolygon2DComponent*>(component))
		{
			DrawPolygon2DSection(polygon);
		}
	}

	// ---- アクター固有 (コンポーネントを持たないアクター) ----
	if (auto* volume = dynamic_cast<APostProcessVolume*>(actor))
	{
		DrawPostProcessVolumeSection(volume);
	}
}

// ------------------------------------------------------------
//  選択管理
// ------------------------------------------------------------
void ImGuiManager::SelectActor(AActor* Actor)
{
	m_SelectedActor = Actor;
	m_SelectedComponent = nullptr;
	m_LabelBuffer[0] = '\0';

	if (Actor)
	{
		// 既定選択は Root。Root が無ければ先頭の所有コンポーネント。
		m_SelectedComponent = Actor->GetRootComponent();
		if (m_SelectedComponent == nullptr && !Actor->GetComponents().empty())
		{
			m_SelectedComponent = Actor->GetComponents().front().get();
		}

		strncpy_s(m_LabelBuffer, Actor->GetActorLabel().c_str(), _TRUNCATE);
	}
}

void ImGuiManager::ValidateSelection()
{
	if (!m_SelectedActor)
	{
		m_SelectedComponent = nullptr;
		return;
	}

	// アクターの生存確認 (Destroy 済み / ワールド外なら選択解除)
	if (!m_World || !m_World->ContainsActor(m_SelectedActor) || m_SelectedActor->IsPendingKill())
	{
		SelectActor(nullptr);
		return;
	}

	// 選択コンポーネントが選択アクターの所有物か確認
	bool owned = false;
	if (m_SelectedComponent)
	{
		for (const auto& component : m_SelectedActor->GetComponents())
		{
			if (component.get() == m_SelectedComponent)
			{
				owned = true;
				break;
			}
		}
	}

	if (!owned)
	{
		m_SelectedComponent = m_SelectedActor->GetRootComponent();
		if (m_SelectedComponent == nullptr && !m_SelectedActor->GetComponents().empty())
		{
			m_SelectedComponent = m_SelectedActor->GetComponents().front().get();
		}
	}
}

// ------------------------------------------------------------
//  コンポーネントツリー
//  Root 以下のアタッチ階層をツリー表示し、Root ツリーに
//  属さないコンポーネント (未アタッチ Scene / 非 Scene) は
//  フラットに並べる。
// ------------------------------------------------------------
void ImGuiManager::DrawComponentTree(AActor* Actor)
{
	TextDisabled("Components");

	USceneComponent* root = Actor->GetRootComponent();
	if (root)
	{
		DrawComponentTreeNode(root);
	}

	for (const auto& componentPtr : Actor->GetComponents())
	{
		UActorComponent* component = componentPtr.get();
		auto* scene = dynamic_cast<USceneComponent*>(component);

		// Root ツリーで描画済みのものはスキップ
		if (scene && (scene == root || scene->GetAttachParent() != nullptr))
		{
			continue;
		}

		if (scene)
		{
			// 未アタッチの SceneComponent は独立ツリーとして描画
			DrawComponentTreeNode(scene);
		}
		else
		{
			// 非 Scene コンポーネントは葉として描画
			ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_Leaf;
			if (component == m_SelectedComponent)
			{
				flags |= ImGuiTreeNodeFlags_Selected;
			}

			std::string name = UWorld::GetClassDisplayName(component);
			bool open = TreeNodeEx((void*)component, flags, "%s", name.c_str());
			if (IsItemClicked())
			{
				m_SelectedComponent = component;
			}
			if (open)
			{
				TreePop();
			}
		}
	}

	if (Actor->GetComponents().empty())
	{
		TextDisabled("(no components)");
	}
}

void ImGuiManager::DrawComponentTreeNode(USceneComponent* Component)
{
	const auto& children = Component->GetAttachChildren();

	ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_DefaultOpen;
	if (children.empty())
	{
		flags |= ImGuiTreeNodeFlags_Leaf;
	}
	if (Component == m_SelectedComponent)
	{
		flags |= ImGuiTreeNodeFlags_Selected;
	}

	std::string name = UWorld::GetClassDisplayName(Component);
	bool open = TreeNodeEx((void*)Component, flags, "%s", name.c_str());
	if (IsItemClicked())
	{
		m_SelectedComponent = Component;
	}

	if (open)
	{
		for (USceneComponent* child : children)
		{
			DrawComponentTreeNode(child);
		}
		TreePop();
	}
}

// ------------------------------------------------------------
//  Details セクション描画
// ------------------------------------------------------------
void ImGuiManager::DrawTransformSection(USceneComponent* Component)
{
	if (!CollapsingHeader("Transform", ImGuiTreeNodeFlags_DefaultOpen))
		return;

	XMFLOAT3 location = Component->GetRelativeLocation();
	if (DragFloat3("Location (m)", &location.x, 0.1f))
	{
		Component->SetRelativeLocation(location);
	}

	XMFLOAT3 rotation = Component->GetRelativeRotation();
	XMFLOAT3 rotationDeg = {
		XMConvertToDegrees(rotation.x),
		XMConvertToDegrees(rotation.y),
		XMConvertToDegrees(rotation.z) };
	if (DragFloat3("Rotation (deg)", &rotationDeg.x, 1.0f))
	{
		Component->SetRelativeRotation({
			XMConvertToRadians(rotationDeg.x),
			XMConvertToRadians(rotationDeg.y),
			XMConvertToRadians(rotationDeg.z) });
	}

	XMFLOAT3 scale = Component->GetRelativeScale3D();
	if (DragFloat3("Scale", &scale.x, 0.01f))
	{
		Component->SetRelativeScale3D(scale);
	}
}

void ImGuiManager::DrawPrimitiveSection(UPrimitiveComponent* Component)
{
	if (!CollapsingHeader("Rendering", ImGuiTreeNodeFlags_DefaultOpen))
		return;

	bool visible = Component->IsVisible();
	if (Checkbox("Visible", &visible))
	{
		Component->SetVisibility(visible);
	}

	bool castShadow = Component->GetCastShadow();
	if (Checkbox("Cast Shadow", &castShadow))
	{
		Component->SetCastShadow(castShadow);
	}

	bool affectDF = Component->GetAffectDistanceFieldLighting();
	if (Checkbox("Affect Distance Field", &affectDF))
	{
		Component->SetAffectDistanceFieldLighting(affectDF);
	}

	// ---- 描画距離カリング (0 = 無制限) ----
	float minDraw = Component->GetMinDrawDistance();
	if (DragFloat("Min Draw Distance", &minDraw, 0.1f, 0.0f, 100000.0f, "%.1f m"))
	{
		Component->SetMinDrawDistance(fmaxf(minDraw, 0.0f));
	}

	float maxDraw = Component->GetCachedMaxDrawDistance();
	if (DragFloat("Max Draw Distance", &maxDraw, 0.1f, 0.0f, 100000.0f, "%.1f m"))
	{
		Component->SetCachedMaxDrawDistance(fmaxf(maxDraw, 0.0f));
	}

	// ---- Translucency Sort Priority (UPrimitiveComponent 同名) ----
	// 低い値が奥、高い値が手前。同値内は Translucency ウィンドウの
	// ソートポリシーで後→前に並ぶ。不透明では無視される。既定 0。
	int sortPriority = Component->GetTranslucentSortPriority();
	if (DragInt("Translucency Sort Priority", &sortPriority, 0.1f))
	{
		Component->SetTranslucentSortPriority(sortPriority);
	}

	// ---- ワールド境界 (CalcBounds の結果。毎フレーム更新) ----
	const FBoxSphereBounds& bounds = Component->GetBounds();
	TextDisabled("Bounds Origin  (%.2f, %.2f, %.2f)",
		bounds.Origin.x, bounds.Origin.y, bounds.Origin.z);
	TextDisabled("Bounds Extent  (%.2f, %.2f, %.2f)",
		bounds.BoxExtent.x, bounds.BoxExtent.y, bounds.BoxExtent.z);
	TextDisabled("Sphere Radius  %.2f m", bounds.SphereRadius);
}

void ImGuiManager::DrawCameraSection(UCameraComponent* Component)
{
	if (!CollapsingHeader("Camera", ImGuiTreeNodeFlags_DefaultOpen))
		return;

	float fov = Component->GetFieldOfView();
	if (SliderFloat("Field of View (deg)", &fov, 5.0f, 120.0f))
	{
		Component->SetFieldOfView(fov);
	}

	float nearClip = Component->GetNearClip();
	if (DragFloat("Near Clip (m)", &nearClip, 0.01f, 0.001f, 10.0f))
	{
		Component->SetNearClip(nearClip);
	}

	float farClip = Component->GetFarClip();
	if (DragFloat("Far Clip (m)", &farClip, 1.0f, 10.0f, 100000.0f))
	{
		Component->SetFarClip(farClip);
	}
}

bool ImGuiManager::DrawMaterialEditor(Material& Mat)
{
	bool changed = false;

	// ---- Blend Mode / Two Sided ----
	// Opaque / Masked はベースパス (G-Buffer)、Translucent / Additive は
	// トランスルーセンシーパス (SceneColor へフォワード合成) で描かれる。
	static const char* blendModeNames[] = { "Opaque", "Masked", "Translucent", "Additive" };
	int blendMode = (int)Mat.Params.BlendMode;
	if (Combo("Blend Mode", &blendMode, blendModeNames, IM_ARRAYSIZE(blendModeNames)))
	{
		Mat.Params.BlendMode = (EBlendMode)blendMode;
		changed = true;
	}

	bool twoSided = Mat.IsTwoSided();
	if (Checkbox("Two Sided", &twoSided))
	{
		Mat.SetTwoSided(twoSided);
		changed = true;
	}

	// Masked のみ: OpacityMask の clip しきい値
	if (IsMaskedBlendMode(Mat.Params.BlendMode))
	{
		changed |= SliderFloat("Opacity Mask Clip Value", &Mat.Params.OpacityMaskClipValue, 0.0f, 1.0f);
	}

	// Translucent / Additive のみ: 不透明度
	if (IsTranslucentBlendMode(Mat.Params.BlendMode))
	{
		changed |= SliderFloat("Opacity", &Mat.Params.Opacity, 0.0f, 1.0f);
	}

	changed |= ColorEdit4("Base Color", &Mat.Params.BaseColor.x, ImGuiColorEditFlags_Float);
	changed |= ColorEdit4("Emission", &Mat.Params.EmissionColor.x, ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR);
	changed |= SliderFloat("Metallic", &Mat.Params.Metallic, 0.0f, 1.0f);
	changed |= SliderFloat("Specular", &Mat.Params.Specular, 0.0f, 1.0f);
	changed |= SliderFloat("Roughness", &Mat.Params.Roughness, 0.0f, 1.0f);
	changed |= SliderFloat("Normal Weight", &Mat.Params.NormalWeight, 0.0f, 2.0f);

	bool unlit = (Mat.Params.Unlit != FALSE);
	if (Checkbox("Unlit", &unlit))
	{
		Mat.Params.Unlit = unlit ? TRUE : FALSE;
		changed = true;
	}

	// ============================================================
	//  Substrate Slab BSDF
	//  bUseSubstrate で Slab ワークフローに切り替える。レガシーの
	//  Metallic / Specular は無視され、F0 / F90 が界面を定義する。
	// ============================================================
	if (CollapsingHeader("Substrate (Slab BSDF)"))
	{
		bool useSubstrate = Mat.IsSubstrateEnabled();
		if (Checkbox("Use Substrate", &useSubstrate))
		{
			Mat.SetUseSubstrate(useSubstrate);
			changed = true;
		}

		if (Mat.IsSubstrateEnabled())
		{
			changed |= ColorEdit3("Diffuse Albedo", &Mat.Params.SubstrateDiffuseAlbedo.x, ImGuiColorEditFlags_Float);
			changed |= ColorEdit3("F0", &Mat.Params.SubstrateF0.x, ImGuiColorEditFlags_Float);
			changed |= ColorEdit3("F90", &Mat.Params.SubstrateF90.x, ImGuiColorEditFlags_Float);
			changed |= SliderFloat("Anisotropy", &Mat.Params.SubstrateAnisotropy, -1.0f, 1.0f);

			// ---- Sub-Surface (SUBSTRATE_SSS_TYPE_* と 1:1) ----
			// Diffusion / Diffusion Profile はスクリーン空間拡散パス
			// 非対応環境のため非散乱にフォールバックする。
			static const char* sssTypeNames[] =
			{
				"None", "Wrap", "Two Sided Wrap",
				"Diffusion", "Diffusion Profile", "Simple Volume",
			};
			int sssType = (int)Mat.Params.SubstrateSSSType;
			if (Combo("Sub-Surface Type", &sssType, sssTypeNames, IM_ARRAYSIZE(sssTypeNames)))
			{
				Mat.SetSubstrateSSSType((ESubstrateSSSType)sssType);
				changed = true;
			}

			if (Mat.Params.SubstrateSSSType != ESubstrateSSSType::None)
			{
				// MFP は Transmittance Color + Thickness から導出される
				// (TransmittanceToMeanFreePath, Substrate.hlsl)
				// Transmittance Color = 「参照厚 1cm を通過したときの透過率」。
				// Thickness が濃度スケール: 1cm でこの色が厳密に実現され、
				// 2cm で T^2 (濃い)、0.5cm で √T (透ける)
				changed |= ColorEdit3("Transmittance Color", &Mat.Params.SubstrateTransmittanceColor.x, ImGuiColorEditFlags_Float);
				changed |= SliderFloat("Phase Anisotropy (g)", &Mat.Params.SubstrateSSSPhaseAnisotropy, -0.99f, 0.99f);
			}

			// 下限 0.001cm はシェーダ側 SUBSTRATE_MIN_THICKNESS_CM と同値
			changed |= DragFloat("Thickness (cm)", &Mat.Params.SubstrateThickness, 0.001f, 0.001f, 100.0f, "%.4f");

			bool isThin = (Mat.Params.SubstrateIsThin != FALSE);
			if (Checkbox("Is Thin Surface", &isThin))
			{
				Mat.Params.SubstrateIsThin = isThin ? TRUE : FALSE;
				changed = true;
			}

			// ---- 第 2 スペキュラローブ ----
			changed |= SliderFloat("Second Roughness", &Mat.Params.SubstrateSecondRoughness, 0.0f, 1.0f);
			changed |= SliderFloat("Second Roughness Weight", &Mat.Params.SubstrateSecondRoughnessWeight, 0.0f, 1.0f);

			// ---- ファズ (布・産毛) ----
			changed |= SliderFloat("Fuzz Amount", &Mat.Params.SubstrateFuzzColor.w, 0.0f, 1.0f);
			changed |= ColorEdit3("Fuzz Color", &Mat.Params.SubstrateFuzzColor.x, ImGuiColorEditFlags_Float);
			changed |= SliderFloat("Fuzz Roughness", &Mat.Params.SubstrateFuzzRoughness, 0.01f, 1.0f);
		}
	}

	// ============================================================
	//  Refraction
	//  BLEND_Translucent のみ有効 (Additive は対象外)。
	// ============================================================
	if (CollapsingHeader("Refraction"))
	{
		static const char* refractionNames[] =
		{
			"None", "Index Of Refraction", "Pixel Normal Offset", "2D Offset",
		};
		int refractionMethod = (int)Mat.Params.RefractionMethod;
		if (Combo("Refraction Method", &refractionMethod, refractionNames, IM_ARRAYSIZE(refractionNames)))
		{
			Mat.SetRefractionMethod((ERefractionMethod)refractionMethod);
			changed = true;
		}

		if (Mat.Params.RefractionMethod != ERefractionMethod::None)
		{
			if (Mat.Params.BlendMode != EBlendMode::BLEND_Translucent)
			{
				TextDisabled("(requires Blend Mode = Translucent)");
			}

			switch (Mat.Params.RefractionMethod)
			{
			case ERefractionMethod::IndexOfRefraction:
			{
				// ---- Index Of Refraction From F0 ----
				// Substrate では界面を F0 が定義するため、IOR も同じ F0
				// から導出して整合させられる (誘電体逆変換)
				bool useF0 = Mat.IsRefractionUseF0();
				if (Checkbox("Index Of Refraction From F0", &useF0))
				{
					Mat.SetRefractionUseF0(useF0);
					changed = true;
				}

				if (Mat.IsRefractionUseF0())
				{
					if (Mat.IsSubstrateEnabled())
					{
						// シェーダと同一式: DielectricF0ToIor(F0RGBToF0(F0))
						const XMFLOAT4& f0 = Mat.Params.SubstrateF0;
						float f0Avg = (f0.x + f0.y + f0.z) / 3.0f;
						f0Avg = (f0Avg < 0.0f) ? 0.0f : ((f0Avg > 0.99f) ? 0.99f : f0Avg);
						const float sqrtF0 = sqrtf(f0Avg);
						const float derivedIOR = (1.0f + sqrtF0) / (1.0f - sqrtF0);
						Text("Derived IOR: %.3f (from Substrate F0)", derivedIOR);
					}
					else
					{
						// レガシー経路は Slab F0 を持たないため手入力値のまま
						TextDisabled("(requires Use Substrate; manual IOR is used)");
						changed |= SliderFloat("Index Of Refraction", &Mat.Params.RefractionData.x, 1.0f, 3.0f);
					}
				}
				else
				{
					// 1.0 = 空気 (無屈折), 1.33 = 水, 1.52 = ガラス
					changed |= SliderFloat("Index Of Refraction", &Mat.Params.RefractionData.x, 1.0f, 3.0f);
				}
				break;
			}
			case ERefractionMethod::PixelNormalOffset:
				changed |= SliderFloat("Refraction Strength", &Mat.Params.RefractionData.x, 0.0f, 3.0f);
				break;
			case ERefractionMethod::Offset2D:
				changed |= DragFloat2("Screen Offset (pixel)", &Mat.Params.RefractionData.x, 0.1f, -128.0f, 128.0f);
				break;
			default:
				break;
			}

			// 屈折先が「面の深度 + バイアス」より手前なら棄却する
			changed |= DragFloat("Refraction Depth Bias (m)", &Mat.Params.RefractionDepthBias, 0.01f, 0.0f, 10.0f);
		}
	}

	return changed;
}

void ImGuiManager::DrawStaticMeshSection(UStaticMeshComponent* Component)
{
	if (!CollapsingHeader("Materials", ImGuiTreeNodeFlags_DefaultOpen))
		return;

	const unsigned int num = Component->GetNumMaterialSlots();
	bool changed = false;

	for (unsigned int i = 0; i < num; ++i)
	{
		PushID((int)i);

		char slotName[32];
		sprintf_s(slotName, "Slot %u", i);
		if (TreeNodeEx(slotName, i == 0 ? ImGuiTreeNodeFlags_DefaultOpen : 0))
		{
			changed |= DrawMaterialEditor(Component->GetMaterial(i));
			TreePop();
		}

		PopID();
	}

	// GetMaterial() 経由の直接編集はプロキシに自動反映されないため、
	// 変更があったらレンダーステートをダーティにして再生成させる
	if (changed)
	{
		Component->MarkRenderStateDirty();
	}
}

void ImGuiManager::DrawFieldQuadSection(UFieldQuadComponent* Component)
{
	if (!CollapsingHeader("Material", ImGuiTreeNodeFlags_DefaultOpen))
		return;

	if (DrawMaterialEditor(Component->GetMaterial()))
	{
		Component->MarkRenderStateDirty();
	}
}

void ImGuiManager::DrawPolygon2DSection(UPolygon2DComponent* Component)
{
	if (!CollapsingHeader("Polygon 2D", ImGuiTreeNodeFlags_DefaultOpen))
		return;

	XMFLOAT4 color = Component->GetVertexColor();
	if (ColorEdit4("Vertex Color", &color.x, ImGuiColorEditFlags_Float))
	{
		Component->SetVertexColor(color);
	}
}

void ImGuiManager::DrawPostProcessVolumeSection(APostProcessVolume* Volume)
{
	if (!CollapsingHeader("Post Process Volume", ImGuiTreeNodeFlags_DefaultOpen))
		return;

	PP_SETTINGS& s = m_PostProcess->Settings();

	// ---- 永続化 (Saved/Config/EngineSettings.ini) ----
	// 値は終了時に自動保存され、次回起動時に復元される。
	// Reset to Default はこのウィンドウの内容 (PP 設定 + EV +
	// Artist LUT + AutoExposure) をコード初期値へ戻す。
	if (m_Settings)
	{
		if (ImGui::Button("Save Settings"))
		{
			m_Settings->SaveCurrent();
		}
		ImGui::SameLine();
		if (ImGui::Button("Reset to Default"))
		{
			m_Settings->ResetPostProcess();
			m_Settings->ResetAutoExposure();
		}
		ImGui::TextDisabled("Auto-saved on exit -> %s", SettingsManager::GetConfigPath());
		ImGui::Separator();
	}

	auto flagCheckbox = [&](const char* label, PP_FLAG flag)
		{
			bool on = m_PostProcess->HasFlag(flag);
			if (ImGui::Checkbox(label, &on))
				m_PostProcess->SetFlag(flag, on);
		};

	// ---- Exposure / Tonemapper ----
	if (ImGui::CollapsingHeader("Exposure / Film", ImGuiTreeNodeFlags_DefaultOpen))
	{
		// Exposure
		ImGui::SliderFloat("Exposure (EV)", &m_PostProcess->EV(), -8.0f, 8.0f);
		ImGui::Text("Linear: %.3f", s.Exposure);

		// Tonemapper
		const char* modes[] = { "ACES (Narkowicz)", "ACES (Hill)", "None" };
		int mode = (int)s.TonemapperMode;
		if (ImGui::Combo("Tonemapper", &mode, modes, IM_ARRAYSIZE(modes)))
			s.TonemapperMode = (unsigned int)mode;
	}

	// ---- Auto Exposure ----
	if (ImGui::CollapsingHeader("Auto Exposure (Eye Adaptation)"))
	{
		flagCheckbox("Enable Auto Exposure", PP_FLAG_AUTO_EXPOSURE);
		if (m_AutoExposure)
		{
			m_AutoExposure->UpdateReadback();
			if (m_PostProcess && m_PostProcess->HasFlag(PP_FLAG_AUTO_EXPOSURE))
			{
				ImGui::Separator();
				ImGui::Text("Current Exposure : %.4f  (%.2f EV)",
					m_AutoExposure->GetCurrentExposure(),
					m_AutoExposure->GetCurrentExposureEV());
				ImGui::Text("Avg Luminance    : %.4f",
					m_AutoExposure->GetCurrentAvgLuminance());
				ImGui::Separator();
			}
			auto& p = m_AutoExposure->GetParams();
			ImGui::SliderFloat("Min Log Luminance", &p.MinLogLuminance, -20.0f, 20.0f);
			ImGui::SliderFloat("Max Log Luminance", &p.MaxLogLuminance, -20.0f, 20.0f);
			ImGui::SliderFloat("Low Percent", &p.LowPercent, 0.0f, 1.0f);
			ImGui::SliderFloat("High Percent", &p.HighPercent, 0.0f, 1.0f);
			ImGui::SliderFloat("Min Brightness", &p.MinBrightness, 0.0f, 1.0f);
			ImGui::SliderFloat("Max Brightness", &p.MaxBrightness, 0.1f, 1.0f);
			ImGui::SliderFloat("Speed Up", &p.SpeedUp, 0.1f, 20.0f);
			ImGui::SliderFloat("Speed Down", &p.SpeedDown, 0.1f, 20.0f);
			ImGui::SliderFloat("Exposure Compensation", &p.ExposureCompensation, -10.0f, 10.0f);
			ImGui::TextDisabled("Manual EV is bypassed while Auto Exposure is on.");
			if (m_Settings && ImGui::Button("Reset Auto Exposure"))
				m_Settings->ResetAutoExposure();
		}
	}

	// ---- Bloom ----
	if (ImGui::CollapsingHeader("Bloom", ImGuiTreeNodeFlags_DefaultOpen))
	{
		flagCheckbox("Enable Bloom", PP_FLAG_BLOOM);
		ImGui::SliderFloat("Intensity", &s.BloomIntensity, 0.0f, 4.0f);
		ImGui::SliderFloat("Threshold", &s.BloomThreshold, 0.0f, 8.0f);
	}

	// ---- White Balance ----
	if (ImGui::CollapsingHeader("White Balance"))
	{
		flagCheckbox("Enable White Balance", PP_FLAG_WHITE_BALANCE);
		ImGui::SliderFloat("Temp (K)", &s.WhiteTemp, 1500.0f, 15000.0f);
		ImGui::SliderFloat("Tint", &s.WhiteTint, -1.0f, 1.0f);
	}

	// ---- Color Grading ----
	if (ImGui::CollapsingHeader("Color Grading"))
	{
		flagCheckbox("Enable Color Grading", PP_FLAG_COLOR_GRADING);
		ImGui::PushItemWidth(220);
		ImGui::ColorEdit4("Saturation", &s.ColorSaturation.x, ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR);
		ImGui::ColorEdit4("Contrast", &s.ColorContrast.x, ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR);
		ImGui::ColorEdit4("Gamma", &s.ColorGamma.x, ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR);
		ImGui::ColorEdit4("Gain", &s.ColorGain.x, ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR);
		ImGui::ColorEdit4("Offset", &s.ColorOffset.x, ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR);
		ImGui::PopItemWidth();
		if (ImGui::Button("Reset Grading"))
		{
			s.ColorSaturation = { 1,1,1,1 };
			s.ColorContrast = { 1,1,1,1 };
			s.ColorGamma = { 1,1,1,1 };
			s.ColorGain = { 1,1,1,1 };
			s.ColorOffset = { 0,0,0,1 };
		}


		// ---- Artist LUT ----
		ImGui::Separator();
		ImGui::TextUnformatted("LUT");
		if (m_LUTBaker)
		{
			namespace fs = std::filesystem;
			const char* lutDir = "Asset/Texture/LUTs";

			// フォルダ内の .dds を毎フレーム列挙するのは無駄なので静的にキャッシュ。
			// 「Refresh」ボタンで再スキャンできる。
			static std::vector<std::string> lutFiles;
			static bool scanned = false;
			auto rescan = [&]()
				{
					lutFiles.clear();
					std::error_code ec;
					if (fs::exists(lutDir, ec))
					{
						for (auto& e : fs::directory_iterator(lutDir, ec))
						{
							if (!e.is_regular_file()) continue;
							std::string ext = e.path().extension().string();
							for (auto& c : ext) c = (char)tolower(c);
							if (ext == ".dds")
							{
								// LUT らしきものだけに絞りたい場合はファイル名で
								// フィルタしてもよい（例: 先頭が "LUT_"）。
								lutFiles.push_back(e.path().string());
							}
						}
					}
					scanned = true;
				};
			if (!scanned) rescan();

			// 現在の選択インデックスを求める。
			const std::string& cur = m_LUTBaker->GetArtistLUTPath();
			int curIdx = -1;
			for (int i = 0; i < (int)lutFiles.size(); ++i)
			{
				// パス表記の差異を吸収するため weakly_canonical で比較。
				std::error_code ec;
				if (fs::weakly_canonical(lutFiles[i], ec) ==
					fs::weakly_canonical(cur, ec))
				{
					curIdx = i;
					break;
				}
			}

			// プレビュー文字列（ファイル名のみ表示）。
			auto fileName = [](const std::string& p)
				{
					return fs::path(p).filename().string();
				};
			std::string preview = (curIdx >= 0)
				? fileName(lutFiles[curIdx])
				: (cur.empty() ? "(none)" : fileName(cur));

			if (ImGui::BeginCombo("LUT File", preview.c_str()))
			{
				// 「なし」を選べるようにする。
				bool noneSel = cur.empty();
				if (ImGui::Selectable("(none)", noneSel))
					m_LUTBaker->ClearArtistLUT();
				if (noneSel) ImGui::SetItemDefaultFocus();

				for (int i = 0; i < (int)lutFiles.size(); ++i)
				{
					bool sel = (i == curIdx);
					if (ImGui::Selectable(fileName(lutFiles[i]).c_str(), sel))
					{
						if (i != curIdx)
							m_LUTBaker->LoadArtistLUT(lutFiles[i].c_str());
					}
					if (sel) ImGui::SetItemDefaultFocus();
				}
				ImGui::EndCombo();
			}

			ImGui::SameLine();
			if (ImGui::Button("Refresh"))
				rescan();

			// ロード済みなら Weight を出す。
			if (m_LUTBaker->HasArtistLUT())
			{
				ImGui::SliderFloat("LUT Weight",
					&m_LUTBaker->ArtistLUTWeight(), 0.0f, 1.0f);
			}
			else
			{
				ImGui::TextDisabled("(no LUT loaded)");
			}
		}
	}

	// ---- Lens effects ----
	if (ImGui::CollapsingHeader("Lens"))
	{
		flagCheckbox("Enable Vignette", PP_FLAG_VIGNETTE);
		ImGui::SliderFloat("Vignette", &s.VignetteIntensity, 0.0f, 1.0f);

		flagCheckbox("Enable Chromatic Aberration", PP_FLAG_CHROMATIC);
		ImGui::SliderFloat("CA Strength", &s.ChromaticAberration, 0.0f, 2.0f);

		flagCheckbox("Enable Film Grain", PP_FLAG_GRAIN);
		ImGui::SliderFloat("Grain", &s.FilmGrainIntensity, 0.0f, 0.5f);
	}

	// ---- Depth of Field (Gaussian) ----
	if (ImGui::CollapsingHeader("Depth of Field"))
	{
		flagCheckbox("Enable Depth of Field", PP_FLAG_DOF);
		ImGui::SliderFloat("Focal Distance (m)", &s.FocalDistance, 0.1f, 200.0f);
		ImGui::SliderFloat("Focal Region (m)", &s.FocalRegion, 0.0f, 100.0f);
		ImGui::SliderFloat("Near Transition (m)", &s.NearTransitionRange, 0.1f, 200.0f);
		ImGui::SliderFloat("Far Transition (m)", &s.FarTransitionRange, 0.1f, 400.0f);
		ImGui::Separator();
		ImGui::SliderFloat("Max Blur Size (px)", &s.MaxBlurSize, 1.0f, 16.0f);
		ImGui::SliderFloat("Near Blur Scale", &s.NearBlurScale, 0.0f, 10.0f);
		ImGui::SliderFloat("Far Blur Scale", &s.FarBlurScale, 0.0f, 10.0f);
	}

}