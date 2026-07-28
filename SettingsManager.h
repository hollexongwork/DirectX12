#pragma once
#include <string>
#include <vector>
#include <DirectXMath.h>
#include "PostProcessSettings.h"
#include "AutoExposure.h"

using namespace DirectX;

// ============================================================
//  SettingsManager
//  ImGui で編集したパラメータの永続化。
//
//    起動時 (Initialize) :
//      1. CaptureDefaults — コード上の初期値 (コンストラクタ既定値 +
//         GameManager での初期設定) をスナップショット
//      2. Saved/Config/EngineSettings.ini が在れば読み込み、
//         各オブジェクトへ適用 (無ければ初期値のまま = 初回起動)
//    終了時 (SaveCurrent) : 現在値を INI へ書き出し (自動保存)
//    随時 (Reset 系)      : スナップショットへ巻き戻し = Default に戻す
//
//  対象:
//    - APostProcessVolume   : PP_SETTINGS の永続化対象フィールド + EV
//    - AutoExposure         : Params 一式
//    - ColorGradingLUTBaker : Artist LUT パス + Weight
//    - ワールド内の全アクター ([Actor.N] セクション):
//        アクターラベル / APostProcessVolume 固有プロパティ /
//        全所有コンポーネント (C<i>. プレフィックス) の
//          - USceneComponent    : Location / Rotation / Scale
//          - UPrimitiveComponent: Visible / CastShadow / AffectDistanceField
//          - UCameraComponent   : FOV / NearClip / FarClip
//          - UPolygon2DComponent: VertexColor
//          - マテリアルスロット  : M<j>. プレフィックス
//            (UStaticMeshComponent 全スロット / UFieldQuadComponent)
//          - ULightComponent 系  : 型別プロパティ一式
//
//  同定はスポーン順のインデックス ([Actor.N]) + クラス名一致で行い、
//  クラス名が一致しないセクション / コンポーネントは安全のため
//  読み飛ばす (レベル構成を変えた後に古い INI を読んでも壊れない)。
//  旧フォーマットの [Light.N] セクションは読まなくなった
//  (初回はコード初期値で起動し、次回保存から [Actor.N] に移行する)。
//
//  ライト / プリミティブの適用はすべて公開セッター経由なので、
//  変更は自動で MarkRenderStateDirty -> 次フレームのプロキシ
//  再生成に乗る。マテリアルの直接代入後は明示的に
//  MarkRenderStateDirty を呼ぶ。
// ============================================================

class UWorld;
class FSceneRenderer;
class APostProcessVolume;
class ColorGradingLUTBaker;
class ConfigFile;
class AActor;
class UActorComponent;
class ULightComponent;
class Material;

class SettingsManager
{
private:
	// ---- マテリアル 1 スロット分のスナップショット ----
	struct MaterialSnapshot
	{
		XMFLOAT4 BaseColor = { 1.0f, 1.0f, 1.0f, 1.0f };
		XMFLOAT4 EmissionColor = { 0.0f, 0.0f, 0.0f, 0.0f };
		float    Metallic = 0.0f;
		float    Specular = 0.5f;
		float    Roughness = 0.5f;
		float    NormalWeight = 1.0f;
		bool     Unlit = false;

		// ---- Blend Mode / Two Sided ----
		int      BlendMode = 0;				// EBlendMode (0=Opaque 1=Masked 2=Translucent 3=Additive)
		bool     TwoSided = false;
		float    Opacity = 1.0f;
		float    OpacityMaskClipValue = 0.3333f;

		// ---- Substrate Slab BSDF (UE5.8) ----
		// 既定値は Material.cpp のコンストラクタと一致させる。
		bool     bUseSubstrate = false;
		XMFLOAT4 SubstrateDiffuseAlbedo = { 0.18f, 0.18f, 0.18f, 1.0f };
		XMFLOAT4 SubstrateF0 = { 0.04f, 0.04f, 0.04f, 1.0f };
		XMFLOAT4 SubstrateF90 = { 1.0f, 1.0f, 1.0f, 1.0f };
		XMFLOAT4 SubstrateTransmittanceColor = { 0.5f, 0.5f, 0.5f, 1.0f };	// w = SSSMFPScale
		XMFLOAT4 SubstrateFuzzColor = { 1.0f, 1.0f, 1.0f, 0.0f };			// w = FuzzAmount
		float    SubstrateAnisotropy = 0.0f;
		float    SubstrateSSSPhaseAnisotropy = 0.0f;
		float    SubstrateThickness = 1.0f;		// [cm] (参照厚 1cm と同値 = Transmittance Color そのまま)
		int      SubstrateSSSType = 0;			// ESubstrateSSSType (0=None .. 5=SimpleVolume)
		float    SubstrateSecondRoughness = 0.5f;
		float    SubstrateSecondRoughnessWeight = 0.0f;
		float    SubstrateFuzzRoughness = 0.5f;
		bool     SubstrateIsThin = false;

		// ---- Refraction (UE5.8) ----
		int      RefractionMethod = 0;			// ERefractionMethod (0=None 1=IOR 2=PixelNormalOffset 3=2DOffset)
		bool     RefractionUseF0 = false;		// IOR を Substrate F0 から導出
		float    RefractionDataX = 1.5f;		// IOR / 法線強度 / 2D オフセット X
		float    RefractionDataY = 0.0f;		// 2D オフセット Y
		float    RefractionDepthBias = 0.0f;	// [m]
	};

	// ---- コンポーネント 1 個分のスナップショット ----
	// 全型のプロパティを平坦に保持する (該当しない型のフィールドは
	// 既定値のまま使われない)。ClassName で型を同定する。
	struct ComponentSnapshot
	{
		std::string ClassName;

		// USceneComponent
		bool     bScene = false;
		XMFLOAT3 Location = { 0.0f, 0.0f, 0.0f };
		XMFLOAT3 Rotation = { 0.0f, 0.0f, 0.0f };	// ラジアン
		XMFLOAT3 Scale = { 1.0f, 1.0f, 1.0f };

		// UPrimitiveComponent
		bool bPrimitive = false;
		bool Visible = true;
		bool CastShadow = true;
		bool AffectDistanceField = true;
		float MinDrawDistance = 0.0f;	// 距離カリング (0 = 無制限)
		float MaxDrawDistance = 0.0f;	// 距離カリング (0 = 無制限, LDMaxDrawDistance)
		int   TranslucencySortPriority = 0;	// 低い = 奥, 高い = 手前 (不透明では無視)

		// UCameraComponent
		bool  bCamera = false;
		float FOV = 45.0f;			// 度
		float NearClip = 0.1f;		// m
		float FarClip = 500.0f;		// m

		// UPolygon2DComponent
		bool     bPolygon2D = false;
		XMFLOAT4 VertexColor = { 1.0f, 1.0f, 1.0f, 1.0f };

		// マテリアルスロット (UStaticMeshComponent / UFieldQuadComponent)
		std::vector<MaterialSnapshot> Materials;

		// ---- ULightComponent 系 ----
		bool        bLight = false;
		std::string LightTypeName;				// "Directional" / "Point" / "Spot" / "Rect"

		// ULightComponentBase / ULightComponent
		bool     AffectsWorld = true;
		float    Intensity = 0.0f;
		XMFLOAT4 LightColor = { 1.0f, 1.0f, 1.0f, 1.0f };
		bool     UseTemperature = false;
		float    Temperature = 6500.0f;
		float    SpecularScale = 1.0f;

		// シャドウ (ULightComponentBase)
		bool  CastShadows = true;
		float ShadowBias = 0.5f;
		float ShadowSlopeBias = 0.5f;
		bool  UseRayTracedDistanceFieldShadows = false;

		// ULocalLightComponent
		float AttenuationRadius = 10.0f;
		int   IntensityUnits = 2;				// ELightUnits::Lumens

		// UPointLightComponent (Spot も含む)
		float SourceRadius = 0.0f;
		float SoftSourceRadius = 0.0f;
		float SourceLength = 0.0f;
		float LightFalloffExponent = 8.0f;
		bool  UseInverseSquaredFalloff = true;

		// USpotLightComponent
		float InnerConeAngle = 0.0f;			// 度
		float OuterConeAngle = 44.0f;			// 度

		// URectLightComponent
		float SourceWidth = 0.64f;				// m
		float SourceHeight = 0.64f;				// m
		float BarnDoorAngle = 88.0f;			// 度
		float BarnDoorLength = 0.2f;			// m

		// UDirectionalLightComponent (CSM / Distance Field Shadows)
		float DynamicShadowDistance = 100.0f;			// m
		int   DynamicShadowCascades = 4;
		float CascadeDistributionExponent = 3.0f;
		float ShadowDistanceFadeoutFraction = 0.1f;
		float DistanceFieldShadowDistance = 300.0f;		// m
		float DistanceFieldTraceDistance = 100.0f;		// m
		float LightSourceAngle = 1.0f;					// 度
	};

	// ---- アクター 1 体分のスナップショット ----
	struct ActorSnapshot
	{
		std::string ClassName;
		std::string Label;

		// APostProcessVolume 固有 (アクター直下のプロパティ)
		bool  bPostProcessVolume = false;
		bool  PPEnabled = true;
		bool  PPUnbound = true;
		float PPBlendWeight = 1.0f;

		std::vector<ComponentSnapshot> Components;
	};

	UWorld*               m_World = nullptr;
	APostProcessVolume*   m_PostProcess = nullptr;
	AutoExposure*         m_AutoExposure = nullptr;
	ColorGradingLUTBaker* m_LUTBaker = nullptr;
	class FSceneRenderer* m_SceneRenderer = nullptr;	// トランスルーセンシーソート設定の永続化用

	// ---- Default スナップショット (INI 適用「前」のコード初期値) ----
	PP_SETTINGS          m_DefaultPP{};
	float                m_DefaultEV = 0.0f;
	AutoExposure::Params m_DefaultAE{};
	std::string          m_DefaultLUTPath;
	float                m_DefaultLUTWeight = 1.0f;

	// ワールド内全アクター (m_DefaultActors[i] = スポーン順 i 番のアクター)
	std::vector<ActorSnapshot> m_DefaultActors;

	bool m_Initialized = false;

	// ---- 内部ヘルパ ----
	void CaptureDefaults();
	void LoadAndApply();

	// アクター / コンポーネントのキャプチャと適用
	static ActorSnapshot     CaptureActor(AActor* Actor);
	static void              ApplyActor(AActor* Actor, const ActorSnapshot& Snap);
	static ComponentSnapshot CaptureComponent(UActorComponent* Component);
	static void              ApplyComponent(UActorComponent* Component, const ComponentSnapshot& Snap);

	static MaterialSnapshot CaptureMaterial(const Material& Mat);
	static void             ApplyMaterial(Material& Mat, const MaterialSnapshot& Snap);

	static const char* LightTypeNameOf(const ULightComponent* Light);
	static void        CaptureLightComponent(const ULightComponent* Light, ComponentSnapshot& InOut);
	static void        ApplyLightComponent(ULightComponent* Light, const ComponentSnapshot& Snap);

	// ActorSnapshot <-> INI セクション ([Actor.N])
	static void WriteActor(ConfigFile& Ini, const std::string& Section, const ActorSnapshot& Snap);
	static void ReadActor (const ConfigFile& Ini, const std::string& Section, ActorSnapshot& InOut);

	// ComponentSnapshot <-> INI キー群 (Prefix = "C<i>.")
	static void WriteComponent(ConfigFile& Ini, const std::string& Section, const std::string& Prefix, const ComponentSnapshot& Snap);
	static void ReadComponent (const ConfigFile& Ini, const std::string& Section, const std::string& Prefix, ComponentSnapshot& InOut);

	// PP_SETTINGS のうち永続化対象フィールドのみ INI と往復する。
	// Exposure (EV から毎 Tick 再計算) / FilmGrainTime / SceneTexelSize /
	// DofPad / パディングはランタイム値なので対象外。
	static void WritePostProcess(ConfigFile& Ini, const PP_SETTINGS& s, float EV);
	static void ReadPostProcess (const ConfigFile& Ini, PP_SETTINGS& s, float& EV);

public:
	// World.BeginPlay 後・ImGuiManager.Start 前に 1 回だけ呼ぶ。
	void Initialize(UWorld* World, FSceneRenderer* Renderer);

	// 現在値を INI へ保存。GameManager のデストラクタから自動で
	// 呼ばれる (= 終了時自動保存)。ImGui の手動保存ボタンからも可。
	bool SaveCurrent() const;

	// ---- Default (コード初期値) へ巻き戻す ----
	void ResetPostProcess();		// PP_SETTINGS + EV + Artist LUT
	void ResetAutoExposure();		// AutoExposure Params
	void ResetActor(AActor* Actor);	// アクター 1 体 (ラベル + 全コンポーネント)
	void ResetAllActors();
	void ResetLight(int Index);		// ライト 1 灯 (ライトのスポーン順インデックス)
	void ResetAllLights();
	void ResetAll();

	int GetDefaultLightCount() const;

	static const char* GetConfigPath();
};
