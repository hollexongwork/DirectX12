#include "Main.h"
#include "SettingsManager.h"
#include "ConfigFile.h"
#include "World.h"
#include "SceneRenderer.h"
#include "PostProcessVolume.h"
#include "ColorGradingLUTBaker.h"
#include "Light.h"
#include "LightComponent.h"
#include "StaticMeshComponent.h"
#include "Field.h"
#include "Polygon2D.h"
#include "Camera.h"
#include "CameraComponent.h"

#include <filesystem>
#include <cmath>

// ============================================================
//  SettingsManager : ImGui パラメータの永続化 (Saved/Config INI)。
//  ワールド内の全アクターを [Actor.N] セクションへ書き出す。
// ============================================================

// 保存先
static const char* CONFIG_PATH = "Saved/Config/EngineSettings.ini";

const char* SettingsManager::GetConfigPath()
{
	return CONFIG_PATH;
}

// ------------------------------------------------------------
//  初期化: Default スナップショット -> INI 読み込み -> 適用
// ------------------------------------------------------------
void SettingsManager::Initialize(UWorld* World, FSceneRenderer* Renderer)
{
	m_World = World;
	m_PostProcess = World ? World->GetActorOfClass<APostProcessVolume>() : nullptr;
	m_AutoExposure = Renderer ? Renderer->GetAutoExposure() : nullptr;
	m_LUTBaker = Renderer ? Renderer->GetColorGradingLUTBaker() : nullptr;
	m_SceneRenderer = Renderer;

	// INI を適用する「前」に必ずスナップショットする。
	// これが Reset to Default の戻り先 (= コード上の初期値) になる。
	CaptureDefaults();
	LoadAndApply();

	m_Initialized = true;
}

void SettingsManager::CaptureDefaults()
{
	if (m_PostProcess)
	{
		m_DefaultPP = m_PostProcess->Settings();
		m_DefaultEV = m_PostProcess->EV();
	}

	if (m_AutoExposure)
	{
		m_DefaultAE = m_AutoExposure->GetParams();
	}

	if (m_LUTBaker)
	{
		m_DefaultLUTPath = m_LUTBaker->GetArtistLUTPath();
		m_DefaultLUTWeight = m_LUTBaker->ArtistLUTWeight();
	}

	// ---- ワールド内全アクター (m_DefaultActors[i] = スポーン順 i 番) ----
	m_DefaultActors.clear();
	if (m_World)
	{
		const auto& actors = m_World->GetActors();
		m_DefaultActors.reserve(actors.size());
		for (const auto& actor : actors)
		{
			m_DefaultActors.push_back(CaptureActor(actor.get()));
		}
	}
}

void SettingsManager::LoadAndApply()
{
	ConfigFile ini;
	if (!ini.Load(CONFIG_PATH))
		return;	// 初回起動などファイルが無ければコード初期値のまま

	// ---- PostProcess (PP_SETTINGS + EV) ----
	if (m_PostProcess && ini.HasSection("PostProcess"))
	{
		PP_SETTINGS& s = m_PostProcess->Settings();
		float ev = m_PostProcess->EV();
		ReadPostProcess(ini, s, ev);
		m_PostProcess->EV() = ev;
		// Tick 前の最初のフレームでも正しい値になるよう即時反映
		s.Exposure = powf(2.0f, ev);
	}

	// ---- Translucency (ソートポリシー / 軸) ----
	if (m_SceneRenderer && ini.HasSection("Translucency"))
	{
		FSceneRenderer::FTranslucencyParams& t = m_SceneRenderer->GetTranslucencyParams();

		int policy = (int)t.SortPolicy;
		ini.GetInt("Translucency", "SortPolicy", policy);
		if (policy < 0 || policy > (int)FSceneRenderer::ETranslucentSortPolicy::SortAlongAxis)
			policy = (int)FSceneRenderer::ETranslucentSortPolicy::SortByDistance;
		t.SortPolicy = (FSceneRenderer::ETranslucentSortPolicy)policy;

		ini.GetFloat3("Translucency", "SortAxis", t.SortAxis);
	}

	// ---- Auto Exposure ----
	if (m_AutoExposure && ini.HasSection("AutoExposure"))
	{
		AutoExposure::Params& p = m_AutoExposure->GetParams();
		ini.GetFloat("AutoExposure", "MinLogLuminance", p.MinLogLuminance);
		ini.GetFloat("AutoExposure", "MaxLogLuminance", p.MaxLogLuminance);
		ini.GetFloat("AutoExposure", "LowPercent", p.LowPercent);
		ini.GetFloat("AutoExposure", "HighPercent", p.HighPercent);
		ini.GetFloat("AutoExposure", "MinBrightness", p.MinBrightness);
		ini.GetFloat("AutoExposure", "MaxBrightness", p.MaxBrightness);
		ini.GetFloat("AutoExposure", "SpeedUp", p.SpeedUp);
		ini.GetFloat("AutoExposure", "SpeedDown", p.SpeedDown);
		ini.GetFloat("AutoExposure", "ExposureCompensation", p.ExposureCompensation);
	}

	// ---- Artist LUT ----
	if (m_LUTBaker && ini.HasSection("ArtistLUT"))
	{
		std::string path = m_LUTBaker->GetArtistLUTPath();
		float weight = m_LUTBaker->ArtistLUTWeight();
		ini.GetString("ArtistLUT", "Path", path);
		ini.GetFloat("ArtistLUT", "Weight", weight);

		std::error_code ec;
		if (path.empty())
		{
			m_LUTBaker->ClearArtistLUT();
			m_LUTBaker->ArtistLUTWeight() = weight;
		}
		else if (std::filesystem::exists(path, ec))
		{
			if (path != m_LUTBaker->GetArtistLUTPath())
				m_LUTBaker->LoadArtistLUT(path.c_str());
			m_LUTBaker->ArtistLUTWeight() = weight;
		}
		// パスが記録されているのにファイルが消えている場合は
		// 何もしない (既定のまま)。
	}

	// ---- 全アクター ([Actor.N] セクション、スポーン順で同定) ----
	if (m_World)
	{
		const auto& actors = m_World->GetActors();
		for (int i = 0; i < (int)actors.size(); ++i)
		{
			char section[32];
			sprintf_s(section, "Actor.%d", i);
			if (!ini.HasSection(section))
				continue;

			AActor* actor = actors[i].get();

			// クラス名の一致チェック (レベル構成が変わった後の安全弁)
			std::string savedClass;
			ini.GetString(section, "Class", savedClass);
			if (savedClass != UWorld::GetClassDisplayName(actor))
				continue;

			// 現在値をベースに、INI に在るキーだけ上書きして適用
			ActorSnapshot snap = CaptureActor(actor);
			ReadActor(ini, section, snap);
			ApplyActor(actor, snap);
		}
	}
}

// ------------------------------------------------------------
//  保存: 現在値 -> INI
// ------------------------------------------------------------
bool SettingsManager::SaveCurrent() const
{
	if (!m_Initialized)
		return false;

	ConfigFile ini;

	if (m_PostProcess)
	{
		WritePostProcess(ini, m_PostProcess->Settings(), m_PostProcess->EV());
	}

	if (m_AutoExposure)
	{
		const AutoExposure::Params& p = m_AutoExposure->GetParams();
		ini.SetFloat("AutoExposure", "MinLogLuminance", p.MinLogLuminance);
		ini.SetFloat("AutoExposure", "MaxLogLuminance", p.MaxLogLuminance);
		ini.SetFloat("AutoExposure", "LowPercent", p.LowPercent);
		ini.SetFloat("AutoExposure", "HighPercent", p.HighPercent);
		ini.SetFloat("AutoExposure", "MinBrightness", p.MinBrightness);
		ini.SetFloat("AutoExposure", "MaxBrightness", p.MaxBrightness);
		ini.SetFloat("AutoExposure", "SpeedUp", p.SpeedUp);
		ini.SetFloat("AutoExposure", "SpeedDown", p.SpeedDown);
		ini.SetFloat("AutoExposure", "ExposureCompensation", p.ExposureCompensation);
	}

	if (m_LUTBaker)
	{
		ini.SetString("ArtistLUT", "Path", m_LUTBaker->GetArtistLUTPath());
		ini.SetFloat("ArtistLUT", "Weight", m_LUTBaker->ArtistLUTWeight());
	}

	if (m_SceneRenderer)
	{
		const FSceneRenderer::FTranslucencyParams& t = m_SceneRenderer->GetTranslucencyParams();
		ini.SetInt("Translucency", "SortPolicy", (int)t.SortPolicy);
		ini.SetFloat3("Translucency", "SortAxis", t.SortAxis);
	}

	if (m_World)
	{
		const auto& actors = m_World->GetActors();
		for (int i = 0; i < (int)actors.size(); ++i)
		{
			char section[32];
			sprintf_s(section, "Actor.%d", i);
			WriteActor(ini, section, CaptureActor(actors[i].get()));
		}
	}

	// Saved/Config ディレクトリを掘ってから書き出し
	std::error_code ec;
	std::filesystem::create_directories(
		std::filesystem::path(CONFIG_PATH).parent_path(), ec);

	return ini.Save(CONFIG_PATH);
}

// ------------------------------------------------------------
//  Reset 系: Default スナップショットへ巻き戻す
// ------------------------------------------------------------
void SettingsManager::ResetPostProcess()
{
	if (m_PostProcess)
	{
		// グレイン時刻はランタイム値なので現在値を引き継ぐ
		const float grainTime = m_PostProcess->Settings().FilmGrainTime;
		m_PostProcess->Settings() = m_DefaultPP;
		m_PostProcess->Settings().FilmGrainTime = grainTime;
		m_PostProcess->EV() = m_DefaultEV;
		m_PostProcess->Settings().Exposure = powf(2.0f, m_DefaultEV);
	}

	// Artist LUT も PostProcess ウィンドウ配下なので一緒に戻す。
	// グレーディング値の変化は ColorGradingLUTBaker::UpdateIfDirty の
	// ParamsChanged が検出するので、明示的な MarkDirty は不要。
	if (m_LUTBaker)
	{
		std::error_code ec;
		if (!m_DefaultLUTPath.empty() && std::filesystem::exists(m_DefaultLUTPath, ec))
		{
			if (m_LUTBaker->GetArtistLUTPath() != m_DefaultLUTPath)
				m_LUTBaker->LoadArtistLUT(m_DefaultLUTPath.c_str());
		}
		else
		{
			m_LUTBaker->ClearArtistLUT();
		}
		m_LUTBaker->ArtistLUTWeight() = m_DefaultLUTWeight;
	}
}

void SettingsManager::ResetAutoExposure()
{
	if (m_AutoExposure)
	{
		m_AutoExposure->GetParams() = m_DefaultAE;
	}
}

void SettingsManager::ResetActor(AActor* Actor)
{
	if (!m_World || Actor == nullptr)
		return;

	// スポーン順インデックスで Default スナップショットと対応付ける
	const auto& actors = m_World->GetActors();
	for (int i = 0; i < (int)actors.size(); ++i)
	{
		if (actors[i].get() != Actor)
			continue;

		if (i >= (int)m_DefaultActors.size())
			return;	// Default キャプチャ後にスポーンされたアクター

		const ActorSnapshot& snap = m_DefaultActors[i];
		if (snap.ClassName != UWorld::GetClassDisplayName(Actor))
			return;	// 構成が変わっている場合は何もしない

		ApplyActor(Actor, snap);
		return;
	}
}

void SettingsManager::ResetAllActors()
{
	if (!m_World)
		return;

	const auto& actors = m_World->GetActors();
	for (const auto& actor : actors)
	{
		ResetActor(actor.get());
	}
}

void SettingsManager::ResetLight(int Index)
{
	if (!m_World || Index < 0)
		return;

	std::vector<ALight*> lights;
	m_World->GetActorsOfClass<ALight>(lights);
	if (Index >= (int)lights.size())
		return;

	ResetActor(lights[Index]);
}

void SettingsManager::ResetAllLights()
{
	if (!m_World)
		return;

	std::vector<ALight*> lights;
	m_World->GetActorsOfClass<ALight>(lights);
	for (ALight* light : lights)
	{
		ResetActor(light);
	}
}

void SettingsManager::ResetAll()
{
	ResetPostProcess();
	ResetAutoExposure();
	ResetAllActors();
}

int SettingsManager::GetDefaultLightCount() const
{
	int count = 0;
	for (const ActorSnapshot& actor : m_DefaultActors)
	{
		for (const ComponentSnapshot& component : actor.Components)
		{
			if (component.bLight)
			{
				++count;
				break;
			}
		}
	}
	return count;
}

// ------------------------------------------------------------
//  アクターのキャプチャ / 適用
// ------------------------------------------------------------
SettingsManager::ActorSnapshot SettingsManager::CaptureActor(AActor* Actor)
{
	ActorSnapshot snap;
	if (Actor == nullptr)
		return snap;

	snap.ClassName = UWorld::GetClassDisplayName(Actor);
	snap.Label = Actor->GetActorLabel();

	if (auto* volume = dynamic_cast<APostProcessVolume*>(Actor))
	{
		snap.bPostProcessVolume = true;
		snap.PPEnabled = volume->bEnabled;
		snap.PPUnbound = volume->bUnbound;
		snap.PPBlendWeight = volume->BlendWeight;
	}

	for (const auto& component : Actor->GetComponents())
	{
		snap.Components.push_back(CaptureComponent(component.get()));
	}

	return snap;
}

void SettingsManager::ApplyActor(AActor* Actor, const ActorSnapshot& Snap)
{
	if (Actor == nullptr)
		return;
	if (Snap.ClassName != UWorld::GetClassDisplayName(Actor))
		return;

	if (!Snap.Label.empty())
	{
		Actor->SetActorLabel(Snap.Label);
	}

	if (auto* volume = dynamic_cast<APostProcessVolume*>(Actor))
	{
		if (Snap.bPostProcessVolume)
		{
			volume->bEnabled = Snap.PPEnabled;
			volume->bUnbound = Snap.PPUnbound;
			volume->BlendWeight = Snap.PPBlendWeight;
		}
	}

	// コンポーネントは生成順インデックスで対応付け、
	// クラス名が一致するものだけ適用する
	const auto& components = Actor->GetComponents();
	const size_t count = std::min<size_t>(components.size(), Snap.Components.size());
	for (size_t i = 0; i < count; ++i)
	{
		UActorComponent* component = components[i].get();
		if (Snap.Components[i].ClassName != UWorld::GetClassDisplayName(component))
			continue;

		ApplyComponent(component, Snap.Components[i]);
	}
}

// ------------------------------------------------------------
//  コンポーネントのキャプチャ / 適用
// ------------------------------------------------------------
SettingsManager::ComponentSnapshot SettingsManager::CaptureComponent(UActorComponent* Component)
{
	ComponentSnapshot snap;
	if (Component == nullptr)
		return snap;

	snap.ClassName = UWorld::GetClassDisplayName(Component);

	if (auto* scene = dynamic_cast<USceneComponent*>(Component))
	{
		snap.bScene = true;
		snap.Location = scene->GetRelativeLocation();
		snap.Rotation = scene->GetRelativeRotation();
		snap.Scale = scene->GetRelativeScale3D();
	}

	if (auto* primitive = dynamic_cast<UPrimitiveComponent*>(Component))
	{
		snap.bPrimitive = true;
		snap.Visible = primitive->IsVisible();
		snap.CastShadow = primitive->GetCastShadow();
		snap.AffectDistanceField = primitive->GetAffectDistanceFieldLighting();
		snap.MinDrawDistance = primitive->GetMinDrawDistance();
		snap.MaxDrawDistance = primitive->GetCachedMaxDrawDistance();
		snap.TranslucencySortPriority = primitive->GetTranslucentSortPriority();
	}

	if (auto* camera = dynamic_cast<UCameraComponent*>(Component))
	{
		snap.bCamera = true;
		snap.FOV = camera->GetFieldOfView();
		snap.NearClip = camera->GetNearClip();
		snap.FarClip = camera->GetFarClip();
	}

	if (auto* polygon = dynamic_cast<UPolygon2DComponent*>(Component))
	{
		snap.bPolygon2D = true;
		snap.VertexColor = polygon->GetVertexColor();
	}

	// マテリアルスロット (StaticMesh は全スロット / FieldQuad は 1 枚)
	if (auto* mesh = dynamic_cast<UStaticMeshComponent*>(Component))
	{
		const unsigned int num = mesh->GetNumMaterialSlots();
		snap.Materials.resize(num);
		for (unsigned int i = 0; i < num; ++i)
		{
			snap.Materials[i] = CaptureMaterial(mesh->GetMaterial(i));
		}
	}
	else if (auto* quad = dynamic_cast<UFieldQuadComponent*>(Component))
	{
		snap.Materials.resize(1);
		snap.Materials[0] = CaptureMaterial(quad->GetMaterial());
	}

	if (auto* light = dynamic_cast<ULightComponent*>(Component))
	{
		snap.bLight = true;
		CaptureLightComponent(light, snap);
	}

	return snap;
}

void SettingsManager::ApplyComponent(UActorComponent* Component, const ComponentSnapshot& Snap)
{
	if (Component == nullptr)
		return;

	if (auto* scene = dynamic_cast<USceneComponent*>(Component))
	{
		if (Snap.bScene)
		{
			scene->SetRelativeLocation(Snap.Location);
			scene->SetRelativeRotation(Snap.Rotation);
			scene->SetRelativeScale3D(Snap.Scale);
		}
	}

	if (auto* primitive = dynamic_cast<UPrimitiveComponent*>(Component))
	{
		if (Snap.bPrimitive)
		{
			primitive->SetVisibility(Snap.Visible);
			primitive->SetCastShadow(Snap.CastShadow);
			primitive->SetAffectDistanceFieldLighting(Snap.AffectDistanceField);
			primitive->SetMinDrawDistance(Snap.MinDrawDistance);
			primitive->SetCachedMaxDrawDistance(Snap.MaxDrawDistance);
			primitive->SetTranslucentSortPriority(Snap.TranslucencySortPriority);
		}
	}

	if (auto* camera = dynamic_cast<UCameraComponent*>(Component))
	{
		if (Snap.bCamera)
		{
			camera->SetFieldOfView(Snap.FOV);
			camera->SetNearClip(Snap.NearClip);
			camera->SetFarClip(Snap.FarClip);
		}
	}

	if (auto* polygon = dynamic_cast<UPolygon2DComponent*>(Component))
	{
		if (Snap.bPolygon2D)
		{
			polygon->SetVertexColor(Snap.VertexColor);
		}
	}

	// マテリアル (直接代入なので明示的に MarkRenderStateDirty する)
	if (auto* mesh = dynamic_cast<UStaticMeshComponent*>(Component))
	{
		const unsigned int num = std::min<unsigned int>(
			mesh->GetNumMaterialSlots(), (unsigned int)Snap.Materials.size());
		for (unsigned int i = 0; i < num; ++i)
		{
			ApplyMaterial(mesh->GetMaterial(i), Snap.Materials[i]);
		}
		if (num > 0)
		{
			mesh->MarkRenderStateDirty();
		}
	}
	else if (auto* quad = dynamic_cast<UFieldQuadComponent*>(Component))
	{
		if (!Snap.Materials.empty())
		{
			ApplyMaterial(quad->GetMaterial(), Snap.Materials[0]);
			quad->MarkRenderStateDirty();
		}
	}

	if (auto* light = dynamic_cast<ULightComponent*>(Component))
	{
		if (Snap.bLight)
		{
			ApplyLightComponent(light, Snap);
		}
	}
}

// ------------------------------------------------------------
//  マテリアルのキャプチャ / 適用
// ------------------------------------------------------------
SettingsManager::MaterialSnapshot SettingsManager::CaptureMaterial(const Material& Mat)
{
	MaterialSnapshot snap;
	snap.BaseColor = Mat.Params.BaseColor;
	snap.EmissionColor = Mat.Params.EmissionColor;
	snap.Metallic = Mat.Params.Metallic;
	snap.Specular = Mat.Params.Specular;
	snap.Roughness = Mat.Params.Roughness;
	snap.NormalWeight = Mat.Params.NormalWeight;
	snap.Unlit = (Mat.Params.Unlit != FALSE);

	snap.BlendMode = (int)Mat.Params.BlendMode;
	snap.TwoSided = Mat.IsTwoSided();
	snap.Opacity = Mat.Params.Opacity;
	snap.OpacityMaskClipValue = Mat.Params.OpacityMaskClipValue;

	// ---- Substrate Slab BSDF ----
	snap.bUseSubstrate = Mat.IsSubstrateEnabled();
	snap.SubstrateDiffuseAlbedo = Mat.Params.SubstrateDiffuseAlbedo;
	snap.SubstrateF0 = Mat.Params.SubstrateF0;
	snap.SubstrateF90 = Mat.Params.SubstrateF90;
	snap.SubstrateTransmittanceColor = Mat.Params.SubstrateTransmittanceColor;
	snap.SubstrateFuzzColor = Mat.Params.SubstrateFuzzColor;
	snap.SubstrateAnisotropy = Mat.Params.SubstrateAnisotropy;
	snap.SubstrateSSSPhaseAnisotropy = Mat.Params.SubstrateSSSPhaseAnisotropy;
	snap.SubstrateThickness = Mat.Params.SubstrateThickness;
	snap.SubstrateSSSType = (int)Mat.Params.SubstrateSSSType;
	snap.SubstrateSecondRoughness = Mat.Params.SubstrateSecondRoughness;
	snap.SubstrateSecondRoughnessWeight = Mat.Params.SubstrateSecondRoughnessWeight;
	snap.SubstrateFuzzRoughness = Mat.Params.SubstrateFuzzRoughness;
	snap.SubstrateIsThin = (Mat.Params.SubstrateIsThin != FALSE);

	// ---- Refraction ----
	snap.RefractionMethod = (int)Mat.Params.RefractionMethod;
	snap.RefractionUseF0 = Mat.IsRefractionUseF0();
	snap.RefractionDataX = Mat.Params.RefractionData.x;
	snap.RefractionDataY = Mat.Params.RefractionData.y;
	snap.RefractionDepthBias = Mat.Params.RefractionDepthBias;
	return snap;
}

void SettingsManager::ApplyMaterial(Material& Mat, const MaterialSnapshot& Snap)
{
	Mat.Params.BaseColor = Snap.BaseColor;
	Mat.Params.EmissionColor = Snap.EmissionColor;
	Mat.Params.Metallic = Snap.Metallic;
	Mat.Params.Specular = Snap.Specular;
	Mat.Params.Roughness = Snap.Roughness;
	Mat.Params.NormalWeight = Snap.NormalWeight;
	Mat.Params.Unlit = Snap.Unlit ? TRUE : FALSE;

	// ---- Blend Mode / Two Sided (範囲外値は Opaque に丸める) ----
	int blendMode = Snap.BlendMode;
	if (blendMode < 0 || blendMode >= (int)EBlendMode::BLEND_MAX)
		blendMode = (int)EBlendMode::BLEND_Opaque;
	Mat.Params.BlendMode = (EBlendMode)blendMode;

	Mat.SetTwoSided(Snap.TwoSided);
	Mat.Params.Opacity = Snap.Opacity;
	Mat.Params.OpacityMaskClipValue = Snap.OpacityMaskClipValue;

	// ---- Substrate Slab BSDF ----
	Mat.SetUseSubstrate(Snap.bUseSubstrate);
	Mat.Params.SubstrateDiffuseAlbedo = Snap.SubstrateDiffuseAlbedo;
	Mat.Params.SubstrateF0 = Snap.SubstrateF0;
	Mat.Params.SubstrateF90 = Snap.SubstrateF90;
	Mat.Params.SubstrateTransmittanceColor = Snap.SubstrateTransmittanceColor;
	Mat.Params.SubstrateFuzzColor = Snap.SubstrateFuzzColor;
	Mat.Params.SubstrateAnisotropy = Snap.SubstrateAnisotropy;
	Mat.Params.SubstrateSSSPhaseAnisotropy = Snap.SubstrateSSSPhaseAnisotropy;
	Mat.Params.SubstrateThickness = Snap.SubstrateThickness;

	// SSSType (範囲外値は None に丸める)
	int sssType = Snap.SubstrateSSSType;
	if (sssType < 0 || sssType >= (int)ESubstrateSSSType::MAX)
		sssType = (int)ESubstrateSSSType::None;
	Mat.SetSubstrateSSSType((ESubstrateSSSType)sssType);

	Mat.Params.SubstrateSecondRoughness = Snap.SubstrateSecondRoughness;
	Mat.Params.SubstrateSecondRoughnessWeight = Snap.SubstrateSecondRoughnessWeight;
	Mat.Params.SubstrateFuzzRoughness = Snap.SubstrateFuzzRoughness;
	Mat.Params.SubstrateIsThin = Snap.SubstrateIsThin ? TRUE : FALSE;

	// ---- Refraction (範囲外値は None に丸める) ----
	int refractionMethod = Snap.RefractionMethod;
	if (refractionMethod < 0 || refractionMethod >= (int)ERefractionMethod::MAX)
		refractionMethod = (int)ERefractionMethod::None;
	Mat.SetRefractionMethod((ERefractionMethod)refractionMethod);

	Mat.Params.RefractionData = { Snap.RefractionDataX, Snap.RefractionDataY };
	Mat.Params.RefractionDepthBias = Snap.RefractionDepthBias;
	Mat.SetRefractionUseF0(Snap.RefractionUseF0);
}

// ------------------------------------------------------------
//  ライトコンポーネントのキャプチャ / 適用
// ------------------------------------------------------------
const char* SettingsManager::LightTypeNameOf(const ULightComponent* Light)
{
	if (!Light)
		return "None";

	switch (Light->GetLightType())
	{
	case ELightType::Directional:	return "Directional";
	case ELightType::Point:			return "Point";
	case ELightType::Spot:			return "Spot";
	case ELightType::Rect:			return "Rect";
	}
	return "None";
}

void SettingsManager::CaptureLightComponent(const ULightComponent* Light, ComponentSnapshot& InOut)
{
	if (!Light)
		return;

	InOut.LightTypeName = LightTypeNameOf(Light);

	InOut.AffectsWorld = Light->GetAffectsWorld();
	InOut.Intensity = Light->GetIntensity();
	InOut.LightColor = Light->GetLightColor();
	InOut.UseTemperature = Light->GetUseTemperature();
	InOut.Temperature = Light->GetTemperature();
	InOut.SpecularScale = Light->GetSpecularScale();

	InOut.CastShadows = Light->GetCastShadows();
	InOut.ShadowBias = Light->GetShadowBias();
	InOut.ShadowSlopeBias = Light->GetShadowSlopeBias();
	InOut.UseRayTracedDistanceFieldShadows = Light->GetUseRayTracedDistanceFieldShadows();

	if (auto* directional = dynamic_cast<const UDirectionalLightComponent*>(Light))
	{
		InOut.DynamicShadowDistance = directional->GetDynamicShadowDistance();
		InOut.DynamicShadowCascades = directional->GetDynamicShadowCascades();
		InOut.CascadeDistributionExponent = directional->GetCascadeDistributionExponent();
		InOut.ShadowDistanceFadeoutFraction = directional->GetShadowDistanceFadeoutFraction();
		InOut.DistanceFieldShadowDistance = directional->GetDistanceFieldShadowDistance();
		InOut.DistanceFieldTraceDistance = directional->GetDistanceFieldTraceDistance();
		InOut.LightSourceAngle = directional->GetLightSourceAngle();
	}

	if (auto* local = dynamic_cast<const ULocalLightComponent*>(Light))
	{
		InOut.AttenuationRadius = local->GetAttenuationRadius();
		InOut.IntensityUnits = (int)local->GetIntensityUnits();
	}

	if (auto* point = dynamic_cast<const UPointLightComponent*>(Light))
	{
		InOut.SourceRadius = point->GetSourceRadius();
		InOut.SoftSourceRadius = point->GetSoftSourceRadius();
		InOut.SourceLength = point->GetSourceLength();
		InOut.LightFalloffExponent = point->GetLightFalloffExponent();
		InOut.UseInverseSquaredFalloff = point->GetUseInverseSquaredFalloff();
	}

	if (auto* spot = dynamic_cast<const USpotLightComponent*>(Light))
	{
		InOut.InnerConeAngle = spot->GetInnerConeAngle();
		InOut.OuterConeAngle = spot->GetOuterConeAngle();
	}

	if (auto* rect = dynamic_cast<const URectLightComponent*>(Light))
	{
		InOut.SourceWidth = rect->GetSourceWidth();
		InOut.SourceHeight = rect->GetSourceHeight();
		InOut.BarnDoorAngle = rect->GetBarnDoorAngle();
		InOut.BarnDoorLength = rect->GetBarnDoorLength();
	}
}

void SettingsManager::ApplyLightComponent(ULightComponent* Light, const ComponentSnapshot& Snap)
{
	if (!Light)
		return;

	// セッター経由なので変更は自動で MarkRenderStateDirty され、
	// 次フレームのプロキシ再生成に乗る。
	Light->SetAffectsWorld(Snap.AffectsWorld);
	Light->SetIntensity(Snap.Intensity);
	Light->SetLightColor(Snap.LightColor);
	Light->SetUseTemperature(Snap.UseTemperature);
	Light->SetTemperature(Snap.Temperature);
	Light->SetSpecularScale(Snap.SpecularScale);

	Light->SetCastShadows(Snap.CastShadows);
	Light->SetShadowBias(Snap.ShadowBias);
	Light->SetShadowSlopeBias(Snap.ShadowSlopeBias);
	Light->SetUseRayTracedDistanceFieldShadows(Snap.UseRayTracedDistanceFieldShadows);

	if (auto* directional = dynamic_cast<UDirectionalLightComponent*>(Light))
	{
		directional->SetDynamicShadowDistance(Snap.DynamicShadowDistance);
		directional->SetDynamicShadowCascades(Snap.DynamicShadowCascades);
		directional->SetCascadeDistributionExponent(Snap.CascadeDistributionExponent);
		directional->SetShadowDistanceFadeoutFraction(Snap.ShadowDistanceFadeoutFraction);
		directional->SetDistanceFieldShadowDistance(Snap.DistanceFieldShadowDistance);
		directional->SetDistanceFieldTraceDistance(Snap.DistanceFieldTraceDistance);
		directional->SetLightSourceAngle(Snap.LightSourceAngle);
	}

	if (auto* local = dynamic_cast<ULocalLightComponent*>(Light))
	{
		local->SetAttenuationRadius(Snap.AttenuationRadius);
		local->SetIntensityUnits((ELightUnits)Snap.IntensityUnits);
	}

	if (auto* point = dynamic_cast<UPointLightComponent*>(Light))
	{
		point->SetSourceRadius(Snap.SourceRadius);
		point->SetSoftSourceRadius(Snap.SoftSourceRadius);
		point->SetSourceLength(Snap.SourceLength);
		point->SetLightFalloffExponent(Snap.LightFalloffExponent);
		point->SetUseInverseSquaredFalloff(Snap.UseInverseSquaredFalloff);
	}

	if (auto* spot = dynamic_cast<USpotLightComponent*>(Light))
	{
		spot->SetInnerConeAngle(Snap.InnerConeAngle);
		spot->SetOuterConeAngle(Snap.OuterConeAngle);
	}

	if (auto* rect = dynamic_cast<URectLightComponent*>(Light))
	{
		rect->SetSourceWidth(Snap.SourceWidth);
		rect->SetSourceHeight(Snap.SourceHeight);
		rect->SetBarnDoorAngle(Snap.BarnDoorAngle);
		rect->SetBarnDoorLength(Snap.BarnDoorLength);
	}
}

// ------------------------------------------------------------
//  ActorSnapshot <-> INI セクション ([Actor.N])
// ------------------------------------------------------------
void SettingsManager::WriteActor(ConfigFile& Ini, const std::string& Section, const ActorSnapshot& Snap)
{
	Ini.SetString(Section, "Class", Snap.ClassName);
	Ini.SetString(Section, "Label", Snap.Label);

	if (Snap.bPostProcessVolume)
	{
		Ini.SetBool(Section, "Enabled", Snap.PPEnabled);
		Ini.SetBool(Section, "Unbound", Snap.PPUnbound);
		Ini.SetFloat(Section, "BlendWeight", Snap.PPBlendWeight);
	}

	Ini.SetInt(Section, "NumComponents", (int)Snap.Components.size());

	for (size_t i = 0; i < Snap.Components.size(); ++i)
	{
		const std::string prefix = "C" + std::to_string(i) + ".";
		WriteComponent(Ini, Section, prefix, Snap.Components[i]);
	}
}

void SettingsManager::ReadActor(const ConfigFile& Ini, const std::string& Section, ActorSnapshot& InOut)
{
	// 在るキーだけ上書き (InOut は呼び出し側で現在値に初期化済み)
	Ini.GetString(Section, "Label", InOut.Label);

	if (InOut.bPostProcessVolume)
	{
		Ini.GetBool(Section, "Enabled", InOut.PPEnabled);
		Ini.GetBool(Section, "Unbound", InOut.PPUnbound);
		Ini.GetFloat(Section, "BlendWeight", InOut.PPBlendWeight);
	}

	for (size_t i = 0; i < InOut.Components.size(); ++i)
	{
		const std::string prefix = "C" + std::to_string(i) + ".";

		// コンポーネント単位のクラス名一致チェック (構成変更後の安全弁)
		std::string savedClass;
		if (!Ini.GetString(Section, prefix + "Class", savedClass))
			continue;
		if (savedClass != InOut.Components[i].ClassName)
			continue;

		ReadComponent(Ini, Section, prefix, InOut.Components[i]);
	}
}

// ------------------------------------------------------------
//  ComponentSnapshot <-> INI キー群 (Prefix = "C<i>.")
// ------------------------------------------------------------
void SettingsManager::WriteComponent(ConfigFile& Ini, const std::string& Section, const std::string& Prefix, const ComponentSnapshot& Snap)
{
	Ini.SetString(Section, Prefix + "Class", Snap.ClassName);

	if (Snap.bScene)
	{
		Ini.SetFloat3(Section, Prefix + "Location", Snap.Location);
		Ini.SetFloat3(Section, Prefix + "Rotation", Snap.Rotation);
		Ini.SetFloat3(Section, Prefix + "Scale", Snap.Scale);
	}

	if (Snap.bPrimitive)
	{
		Ini.SetBool(Section, Prefix + "Visible", Snap.Visible);
		Ini.SetBool(Section, Prefix + "CastShadow", Snap.CastShadow);
		Ini.SetBool(Section, Prefix + "AffectDistanceField", Snap.AffectDistanceField);
		Ini.SetFloat(Section, Prefix + "MinDrawDistance", Snap.MinDrawDistance);
		Ini.SetFloat(Section, Prefix + "MaxDrawDistance", Snap.MaxDrawDistance);
		Ini.SetInt(Section, Prefix + "TranslucencySortPriority", Snap.TranslucencySortPriority);
	}

	if (Snap.bCamera)
	{
		Ini.SetFloat(Section, Prefix + "FOV", Snap.FOV);
		Ini.SetFloat(Section, Prefix + "NearClip", Snap.NearClip);
		Ini.SetFloat(Section, Prefix + "FarClip", Snap.FarClip);
	}

	if (Snap.bPolygon2D)
	{
		Ini.SetFloat4(Section, Prefix + "VertexColor", Snap.VertexColor);
	}

	if (!Snap.Materials.empty())
	{
		Ini.SetInt(Section, Prefix + "NumMaterials", (int)Snap.Materials.size());
		for (size_t i = 0; i < Snap.Materials.size(); ++i)
		{
			const std::string mp = Prefix + "M" + std::to_string(i) + ".";
			const MaterialSnapshot& mat = Snap.Materials[i];

			Ini.SetFloat4(Section, mp + "BaseColor", mat.BaseColor);
			Ini.SetFloat4(Section, mp + "EmissionColor", mat.EmissionColor);
			Ini.SetFloat(Section, mp + "Metallic", mat.Metallic);
			Ini.SetFloat(Section, mp + "Specular", mat.Specular);
			Ini.SetFloat(Section, mp + "Roughness", mat.Roughness);
			Ini.SetFloat(Section, mp + "NormalWeight", mat.NormalWeight);
			Ini.SetBool(Section, mp + "Unlit", mat.Unlit);

			Ini.SetInt(Section, mp + "BlendMode", mat.BlendMode);
			Ini.SetBool(Section, mp + "TwoSided", mat.TwoSided);
			Ini.SetFloat(Section, mp + "Opacity", mat.Opacity);
			Ini.SetFloat(Section, mp + "OpacityMaskClipValue", mat.OpacityMaskClipValue);

			// ---- Substrate Slab BSDF ----
			Ini.SetBool(Section, mp + "UseSubstrate", mat.bUseSubstrate);
			Ini.SetFloat4(Section, mp + "SubstrateDiffuseAlbedo", mat.SubstrateDiffuseAlbedo);
			Ini.SetFloat4(Section, mp + "SubstrateF0", mat.SubstrateF0);
			Ini.SetFloat4(Section, mp + "SubstrateF90", mat.SubstrateF90);
			Ini.SetFloat4(Section, mp + "SubstrateTransmittanceColor", mat.SubstrateTransmittanceColor);
			Ini.SetFloat4(Section, mp + "SubstrateFuzzColor", mat.SubstrateFuzzColor);
			Ini.SetFloat(Section, mp + "SubstrateAnisotropy", mat.SubstrateAnisotropy);
			Ini.SetFloat(Section, mp + "SubstratePhaseAnisotropy", mat.SubstrateSSSPhaseAnisotropy);
			Ini.SetFloat(Section, mp + "SubstrateThickness", mat.SubstrateThickness);
			Ini.SetInt(Section, mp + "SubstrateSSSType", mat.SubstrateSSSType);
			Ini.SetFloat(Section, mp + "SubstrateSecondRoughness", mat.SubstrateSecondRoughness);
			Ini.SetFloat(Section, mp + "SubstrateSecondRoughnessWeight", mat.SubstrateSecondRoughnessWeight);
			Ini.SetFloat(Section, mp + "SubstrateFuzzRoughness", mat.SubstrateFuzzRoughness);
			Ini.SetBool(Section, mp + "SubstrateIsThin", mat.SubstrateIsThin);

			// ---- Refraction ----
			Ini.SetInt(Section, mp + "RefractionMethod", mat.RefractionMethod);
			Ini.SetBool(Section, mp + "RefractionUseF0", mat.RefractionUseF0);
			Ini.SetFloat(Section, mp + "RefractionDataX", mat.RefractionDataX);
			Ini.SetFloat(Section, mp + "RefractionDataY", mat.RefractionDataY);
			Ini.SetFloat(Section, mp + "RefractionDepthBias", mat.RefractionDepthBias);
		}
	}

	if (Snap.bLight)
	{
		// 型に関係するキーだけ書き出す (Directional に半径は書かない)
		const bool isDirectional = (Snap.LightTypeName == "Directional");
		const bool isLocal = (Snap.LightTypeName != "Directional");
		const bool isPoint = (Snap.LightTypeName == "Point" || Snap.LightTypeName == "Spot");
		const bool isSpot = (Snap.LightTypeName == "Spot");
		const bool isRect = (Snap.LightTypeName == "Rect");

		Ini.SetString(Section, Prefix + "LightType", Snap.LightTypeName);

		Ini.SetBool(Section, Prefix + "AffectsWorld", Snap.AffectsWorld);
		Ini.SetFloat(Section, Prefix + "Intensity", Snap.Intensity);
		Ini.SetFloat4(Section, Prefix + "LightColor", Snap.LightColor);
		Ini.SetBool(Section, Prefix + "UseTemperature", Snap.UseTemperature);
		Ini.SetFloat(Section, Prefix + "Temperature", Snap.Temperature);
		Ini.SetFloat(Section, Prefix + "SpecularScale", Snap.SpecularScale);

		Ini.SetBool(Section, Prefix + "CastShadows", Snap.CastShadows);
		Ini.SetFloat(Section, Prefix + "ShadowBias", Snap.ShadowBias);
		Ini.SetFloat(Section, Prefix + "ShadowSlopeBias", Snap.ShadowSlopeBias);
		Ini.SetBool(Section, Prefix + "UseRayTracedDFShadows", Snap.UseRayTracedDistanceFieldShadows);

		if (isDirectional)
		{
			Ini.SetFloat(Section, Prefix + "DynamicShadowDistance", Snap.DynamicShadowDistance);
			Ini.SetInt(Section, Prefix + "DynamicShadowCascades", Snap.DynamicShadowCascades);
			Ini.SetFloat(Section, Prefix + "CascadeDistributionExponent", Snap.CascadeDistributionExponent);
			Ini.SetFloat(Section, Prefix + "ShadowDistanceFadeoutFraction", Snap.ShadowDistanceFadeoutFraction);
			Ini.SetFloat(Section, Prefix + "DistanceFieldShadowDistance", Snap.DistanceFieldShadowDistance);
			Ini.SetFloat(Section, Prefix + "DistanceFieldTraceDistance", Snap.DistanceFieldTraceDistance);
			Ini.SetFloat(Section, Prefix + "LightSourceAngle", Snap.LightSourceAngle);
		}

		if (isLocal)
		{
			Ini.SetFloat(Section, Prefix + "AttenuationRadius", Snap.AttenuationRadius);
			Ini.SetInt(Section, Prefix + "IntensityUnits", Snap.IntensityUnits);
		}

		if (isPoint)
		{
			Ini.SetFloat(Section, Prefix + "SourceRadius", Snap.SourceRadius);
			Ini.SetFloat(Section, Prefix + "SoftSourceRadius", Snap.SoftSourceRadius);
			Ini.SetFloat(Section, Prefix + "SourceLength", Snap.SourceLength);
			Ini.SetFloat(Section, Prefix + "LightFalloffExponent", Snap.LightFalloffExponent);
			Ini.SetBool(Section, Prefix + "UseInverseSquaredFalloff", Snap.UseInverseSquaredFalloff);
		}

		if (isSpot)
		{
			Ini.SetFloat(Section, Prefix + "InnerConeAngle", Snap.InnerConeAngle);
			Ini.SetFloat(Section, Prefix + "OuterConeAngle", Snap.OuterConeAngle);
		}

		if (isRect)
		{
			Ini.SetFloat(Section, Prefix + "SourceWidth", Snap.SourceWidth);
			Ini.SetFloat(Section, Prefix + "SourceHeight", Snap.SourceHeight);
			Ini.SetFloat(Section, Prefix + "BarnDoorAngle", Snap.BarnDoorAngle);
			Ini.SetFloat(Section, Prefix + "BarnDoorLength", Snap.BarnDoorLength);
		}
	}
}

void SettingsManager::ReadComponent(const ConfigFile& Ini, const std::string& Section, const std::string& Prefix, ComponentSnapshot& InOut)
{
	// 在るキーだけ上書き (InOut は呼び出し側で現在値に初期化済み)
	Ini.GetFloat3(Section, Prefix + "Location", InOut.Location);
	Ini.GetFloat3(Section, Prefix + "Rotation", InOut.Rotation);
	Ini.GetFloat3(Section, Prefix + "Scale", InOut.Scale);

	Ini.GetBool(Section, Prefix + "Visible", InOut.Visible);
	Ini.GetBool(Section, Prefix + "CastShadow", InOut.CastShadow);
	Ini.GetBool(Section, Prefix + "AffectDistanceField", InOut.AffectDistanceField);
	Ini.GetFloat(Section, Prefix + "MinDrawDistance", InOut.MinDrawDistance);
	Ini.GetFloat(Section, Prefix + "MaxDrawDistance", InOut.MaxDrawDistance);
	Ini.GetInt(Section, Prefix + "TranslucencySortPriority", InOut.TranslucencySortPriority);

	Ini.GetFloat(Section, Prefix + "FOV", InOut.FOV);
	Ini.GetFloat(Section, Prefix + "NearClip", InOut.NearClip);
	Ini.GetFloat(Section, Prefix + "FarClip", InOut.FarClip);

	Ini.GetFloat4(Section, Prefix + "VertexColor", InOut.VertexColor);

	// マテリアル: 現在のスロット数を超える保存分は読み飛ばす
	for (size_t i = 0; i < InOut.Materials.size(); ++i)
	{
		const std::string mp = Prefix + "M" + std::to_string(i) + ".";
		MaterialSnapshot& mat = InOut.Materials[i];

		Ini.GetFloat4(Section, mp + "BaseColor", mat.BaseColor);
		Ini.GetFloat4(Section, mp + "EmissionColor", mat.EmissionColor);
		Ini.GetFloat(Section, mp + "Metallic", mat.Metallic);
		Ini.GetFloat(Section, mp + "Specular", mat.Specular);
		Ini.GetFloat(Section, mp + "Roughness", mat.Roughness);
		Ini.GetFloat(Section, mp + "NormalWeight", mat.NormalWeight);
		Ini.GetBool(Section, mp + "Unlit", mat.Unlit);

		Ini.GetInt(Section, mp + "BlendMode", mat.BlendMode);
		Ini.GetBool(Section, mp + "TwoSided", mat.TwoSided);
		Ini.GetFloat(Section, mp + "Opacity", mat.Opacity);
		Ini.GetFloat(Section, mp + "OpacityMaskClipValue", mat.OpacityMaskClipValue);

		// ---- Substrate Slab BSDF ----
		// キーが無い場合は既定値のまま (旧 INI との後方互換)
		Ini.GetBool(Section, mp + "UseSubstrate", mat.bUseSubstrate);
		Ini.GetFloat4(Section, mp + "SubstrateDiffuseAlbedo", mat.SubstrateDiffuseAlbedo);
		Ini.GetFloat4(Section, mp + "SubstrateF0", mat.SubstrateF0);
		Ini.GetFloat4(Section, mp + "SubstrateF90", mat.SubstrateF90);
		Ini.GetFloat4(Section, mp + "SubstrateTransmittanceColor", mat.SubstrateTransmittanceColor);
		Ini.GetFloat4(Section, mp + "SubstrateFuzzColor", mat.SubstrateFuzzColor);
		Ini.GetFloat(Section, mp + "SubstrateAnisotropy", mat.SubstrateAnisotropy);
		Ini.GetFloat(Section, mp + "SubstratePhaseAnisotropy", mat.SubstrateSSSPhaseAnisotropy);
		Ini.GetFloat(Section, mp + "SubstrateThickness", mat.SubstrateThickness);
		Ini.GetInt(Section, mp + "SubstrateSSSType", mat.SubstrateSSSType);
		Ini.GetFloat(Section, mp + "SubstrateSecondRoughness", mat.SubstrateSecondRoughness);
		Ini.GetFloat(Section, mp + "SubstrateSecondRoughnessWeight", mat.SubstrateSecondRoughnessWeight);
		Ini.GetFloat(Section, mp + "SubstrateFuzzRoughness", mat.SubstrateFuzzRoughness);
		Ini.GetBool(Section, mp + "SubstrateIsThin", mat.SubstrateIsThin);

		// ---- Refraction ----
		Ini.GetInt(Section, mp + "RefractionMethod", mat.RefractionMethod);
		Ini.GetBool(Section, mp + "RefractionUseF0", mat.RefractionUseF0);
		Ini.GetFloat(Section, mp + "RefractionDataX", mat.RefractionDataX);
		Ini.GetFloat(Section, mp + "RefractionDataY", mat.RefractionDataY);
		Ini.GetFloat(Section, mp + "RefractionDepthBias", mat.RefractionDepthBias);
	}

	// ライト (クラス名一致は呼び出し側で確認済み = 型も一致)
	Ini.GetBool(Section, Prefix + "AffectsWorld", InOut.AffectsWorld);
	Ini.GetFloat(Section, Prefix + "Intensity", InOut.Intensity);
	Ini.GetFloat4(Section, Prefix + "LightColor", InOut.LightColor);
	Ini.GetBool(Section, Prefix + "UseTemperature", InOut.UseTemperature);
	Ini.GetFloat(Section, Prefix + "Temperature", InOut.Temperature);
	Ini.GetFloat(Section, Prefix + "SpecularScale", InOut.SpecularScale);

	Ini.GetBool(Section, Prefix + "CastShadows", InOut.CastShadows);
	Ini.GetFloat(Section, Prefix + "ShadowBias", InOut.ShadowBias);
	Ini.GetFloat(Section, Prefix + "ShadowSlopeBias", InOut.ShadowSlopeBias);
	Ini.GetBool(Section, Prefix + "UseRayTracedDFShadows", InOut.UseRayTracedDistanceFieldShadows);

	Ini.GetFloat(Section, Prefix + "DynamicShadowDistance", InOut.DynamicShadowDistance);
	Ini.GetInt(Section, Prefix + "DynamicShadowCascades", InOut.DynamicShadowCascades);
	Ini.GetFloat(Section, Prefix + "CascadeDistributionExponent", InOut.CascadeDistributionExponent);
	Ini.GetFloat(Section, Prefix + "ShadowDistanceFadeoutFraction", InOut.ShadowDistanceFadeoutFraction);
	Ini.GetFloat(Section, Prefix + "DistanceFieldShadowDistance", InOut.DistanceFieldShadowDistance);
	Ini.GetFloat(Section, Prefix + "DistanceFieldTraceDistance", InOut.DistanceFieldTraceDistance);
	Ini.GetFloat(Section, Prefix + "LightSourceAngle", InOut.LightSourceAngle);

	Ini.GetFloat(Section, Prefix + "AttenuationRadius", InOut.AttenuationRadius);
	Ini.GetInt(Section, Prefix + "IntensityUnits", InOut.IntensityUnits);

	Ini.GetFloat(Section, Prefix + "SourceRadius", InOut.SourceRadius);
	Ini.GetFloat(Section, Prefix + "SoftSourceRadius", InOut.SoftSourceRadius);
	Ini.GetFloat(Section, Prefix + "SourceLength", InOut.SourceLength);
	Ini.GetFloat(Section, Prefix + "LightFalloffExponent", InOut.LightFalloffExponent);
	Ini.GetBool(Section, Prefix + "UseInverseSquaredFalloff", InOut.UseInverseSquaredFalloff);

	Ini.GetFloat(Section, Prefix + "InnerConeAngle", InOut.InnerConeAngle);
	Ini.GetFloat(Section, Prefix + "OuterConeAngle", InOut.OuterConeAngle);

	Ini.GetFloat(Section, Prefix + "SourceWidth", InOut.SourceWidth);
	Ini.GetFloat(Section, Prefix + "SourceHeight", InOut.SourceHeight);
	Ini.GetFloat(Section, Prefix + "BarnDoorAngle", InOut.BarnDoorAngle);
	Ini.GetFloat(Section, Prefix + "BarnDoorLength", InOut.BarnDoorLength);
}

// ------------------------------------------------------------
//  PP_SETTINGS <-> INI ([PostProcess] セクション)
// ------------------------------------------------------------
void SettingsManager::WritePostProcess(ConfigFile& Ini, const PP_SETTINGS& s, float EV)
{
	const char* sec = "PostProcess";

	Ini.SetFloat(sec, "EV", EV);
	Ini.SetUInt(sec, "TonemapperMode", s.TonemapperMode);
	Ini.SetUInt(sec, "Flags", s.Flags);

	Ini.SetFloat(sec, "BloomIntensity", s.BloomIntensity);
	Ini.SetFloat(sec, "BloomThreshold", s.BloomThreshold);

	Ini.SetFloat(sec, "WhiteTemp", s.WhiteTemp);
	Ini.SetFloat(sec, "WhiteTint", s.WhiteTint);
	Ini.SetFloat(sec, "ChromaticAberration", s.ChromaticAberration);
	Ini.SetFloat(sec, "VignetteIntensity", s.VignetteIntensity);

	Ini.SetFloat4(sec, "ColorSaturation", s.ColorSaturation);
	Ini.SetFloat4(sec, "ColorContrast", s.ColorContrast);
	Ini.SetFloat4(sec, "ColorGamma", s.ColorGamma);
	Ini.SetFloat4(sec, "ColorGain", s.ColorGain);
	Ini.SetFloat4(sec, "ColorOffset", s.ColorOffset);

	Ini.SetFloat(sec, "FilmGrainIntensity", s.FilmGrainIntensity);

	Ini.SetFloat(sec, "FocalDistance", s.FocalDistance);
	Ini.SetFloat(sec, "FocalRegion", s.FocalRegion);
	Ini.SetFloat(sec, "NearTransitionRange", s.NearTransitionRange);
	Ini.SetFloat(sec, "FarTransitionRange", s.FarTransitionRange);
	Ini.SetFloat(sec, "MaxBlurSize", s.MaxBlurSize);
	Ini.SetFloat(sec, "NearBlurScale", s.NearBlurScale);
	Ini.SetFloat(sec, "FarBlurScale", s.FarBlurScale);
}

void SettingsManager::ReadPostProcess(const ConfigFile& Ini, PP_SETTINGS& s, float& EV)
{
	const char* sec = "PostProcess";

	Ini.GetFloat(sec, "EV", EV);
	Ini.GetUInt(sec, "TonemapperMode", s.TonemapperMode);
	Ini.GetUInt(sec, "Flags", s.Flags);

	Ini.GetFloat(sec, "BloomIntensity", s.BloomIntensity);
	Ini.GetFloat(sec, "BloomThreshold", s.BloomThreshold);

	Ini.GetFloat(sec, "WhiteTemp", s.WhiteTemp);
	Ini.GetFloat(sec, "WhiteTint", s.WhiteTint);
	Ini.GetFloat(sec, "ChromaticAberration", s.ChromaticAberration);
	Ini.GetFloat(sec, "VignetteIntensity", s.VignetteIntensity);

	Ini.GetFloat4(sec, "ColorSaturation", s.ColorSaturation);
	Ini.GetFloat4(sec, "ColorContrast", s.ColorContrast);
	Ini.GetFloat4(sec, "ColorGamma", s.ColorGamma);
	Ini.GetFloat4(sec, "ColorGain", s.ColorGain);
	Ini.GetFloat4(sec, "ColorOffset", s.ColorOffset);

	Ini.GetFloat(sec, "FilmGrainIntensity", s.FilmGrainIntensity);

	Ini.GetFloat(sec, "FocalDistance", s.FocalDistance);
	Ini.GetFloat(sec, "FocalRegion", s.FocalRegion);
	Ini.GetFloat(sec, "NearTransitionRange", s.NearTransitionRange);
	Ini.GetFloat(sec, "FarTransitionRange", s.FarTransitionRange);
	Ini.GetFloat(sec, "MaxBlurSize", s.MaxBlurSize);
	Ini.GetFloat(sec, "NearBlurScale", s.NearBlurScale);
	Ini.GetFloat(sec, "FarBlurScale", s.FarBlurScale);
}
