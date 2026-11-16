#pragma once
#include <memory>
#include <DirectXMath.h>
#include "PrimitiveComponent.h"
#include "BoxSphereBounds.h"

using namespace DirectX;

// ============================================================
//  Heterogeneous Volume (ゲーム側 + レンダー側へ渡すデータ)
//  UHeterogeneousVolumeComponent / AHeterogeneousVolume
//  (HeterogeneousVolumeComponent.h) に相当。
//
//  ボリュームは単位ボックス [-1, 1]^3 で、コンポーネントの
//  トランスフォームがワールドの OBB へ写す (スケール = 半幅 [m])。
//  UE は Sparse Volume Texture (OpenVDB) の密度を Volume ドメインの
//  マテリアルで評価するが、本エンジンは VDB / SVT / マテリアルグラフを
//  持たないので:
//    - 密度     : コンピュートでベイクした Texture3D [PORT]。ソースは 2 種類:
//                   ProceduralCloud : FProceduralCloudParams (単体の積雲。静的)
//                   ProceduralFire  : FProceduralFireParams (焚き火の炎。密度 + 熱を
//                                     毎フレームベイクし、熱を黒体放射で光らせる)
//    - マテリアル : FHeterogeneousVolumeMaterial (Albedo / Extinction /
//                 Emissive + 位相関数 / マルチスキャッタ) の固定機能 [PORT]
//  描画は FHeterogeneousVolumeRenderer (HeterogeneousVolumes.h) が
//  ライティングキャッシュ + レイマーチで行う。
// ============================================================

struct FHeterogeneousVolumeRenderResource;	// HeterogeneousVolumes.h (GPU リソース)

// ------------------------------------------------------------
//  FProceduralCloudParams
//  単体の積雲の形。形状空間 (ボックス空間 x 半幅 / 最大半幅) で定義する:
//  球 (パフ) 群のスムースユニオン + 平らな雲底 + Worley / Perlin の
//  ビロー + 高周波のディテール侵食
//  (Shader/HeterogeneousVolumeCloudBake_CS.hlsl)。
//  長さは特記が無ければ形状空間 (最大半幅 = 1)。
// ------------------------------------------------------------
struct FProceduralCloudParams
{
	static constexpr unsigned int MaxPuffs = 32;	// HLSL HETEROGENEOUS_VOLUME_MAX_PUFFS

	unsigned int Seed = 7;				// パフの配置とノイズのオフセット
	int   NumPuffs = 22;				// パフの数 (1..MaxPuffs)
	float PuffRadiusMin = 0.40f;		// パフ半径の下限 (垂直半幅比)
	float PuffRadiusMax = 0.90f;		// パフ半径の上限 (垂直半幅比)
	float Spread = 0.85f;				// 水平の広がり (水平半幅比 0..1)
	float TowerHeight = 0.85f;			// 中央の盛り上がり (0 = 平ら, 1 = ボックス上端まで)
	float BottomHeight = -0.55f;		// 雲底の高さ (垂直半幅比 -1..1)
	float BottomSoftness = 0.05f;		// 雲底の丸み
	float SmoothUnion = 0.07f;			// パフ同士のなじみ幅
	float EdgeSoftness = 0.018f;		// 表面から密度 1 になるまでの幅

	float BillowFrequency = 2.8f;		// ビロー (こぶ) の周波数
	float BillowStrength = 0.12f;		// ビローの押し出し量
	int   BillowOctaves = 3;
	float BillowPerlinMix = 0.25f;		// 0 = Worley のみ, 1 = Perlin のみ

	float DetailFrequency = 8.0f;		// ディテール侵食の周波数
	float DetailStrength = 0.05f;		// ディテール侵食の量
	int   DetailOctaves = 2;

	float DensityVariation = 0.3f;		// 内部の密度のムラ (0..1)
	float BorderFade = 0.04f;			// ボックス端のフェード幅 (ボックス空間)

	// 範囲外の値を生成可能な範囲へ丸める (INI の手編集対策)
	void Sanitize();

	// 全フィールドの FNV-1a ハッシュ (密度ベイクのキー)
	unsigned long long ComputeHash() const;

	// シードからパフ (xyz = 中心, w = 半径。形状空間) を生成する。戻り値 = パフ数。
	// AspectScale = 半幅 / 最大半幅。パフはビローの押し出し分を見込んで
	// ボックスの内側に収める
	unsigned int GeneratePuffs(const XMFLOAT3& AspectScale, XMFLOAT4 OutPuffs[MaxPuffs]) const;

	// シード由来のノイズ空間オフセット
	XMFLOAT3 ComputeNoiseOffset() const;
};

// ------------------------------------------------------------
//  EHeterogeneousVolumeSource
//  密度ボリュームの作り方 (Sparse Volume Texture アセットの代わり)
// ------------------------------------------------------------
enum class EHeterogeneousVolumeSource : int
{
	ProceduralCloud = 0,	// 積雲 (R16F: 密度)。パラメータが変わった時だけベイク
	ProceduralFire = 1,		// 炎 (RG16F: 密度 + 熱)。毎フレームベイク (アニメーション)
	MAX,
};

// ------------------------------------------------------------
//  FProceduralFireParams
//  炎 (焚き火) の形と動き。ボックスの底面が火床で、+Y へ昇る
//  (Shader/HeterogeneousVolumeFireBake_CS.hlsl)。
//  半径 / 横揺れはボックス空間 (XZ の半幅 = 1)、ノイズと速度はメートル。
// ------------------------------------------------------------
struct FProceduralFireParams
{
	unsigned int Seed = 3;				// ノイズのオフセット
	int   bAnimate = 1;					// 0 = 時間を止める (静止した炎)

	float BaseRadius = 0.80f;			// 火床の半径 (ボックス空間 XZ, 0..1)
	float TipRadiusScale = 0.45f;		// 炎の先端の半径 (BaseRadius 比)
	float TaperPower = 1.2f;			// 細くなり方 (1 = 直線, 大きいほど根元が太い)
	float FlameHeight = 0.92f;			// 炎の高さ (ボックスの高さ比 0..1)
	float Turbulence = 0.30f;			// 中心軸の横揺れと断面のうねり (ボックス空間)

	float NoiseFrequency = 4.0f;		// 侵食ノイズの周波数 [1/m]
	int   NoiseOctaves = 3;
	float NoiseStrength = 0.7f;			// 侵食の量
	float HeightErosion = 0.8f;			// 高さによる侵食の増え方 (大きいほど上で裂ける)

	float RiseSpeed = 2.2f;				// 炎が昇る速さ [m/s]
	float EvolutionSpeed = 1.2f;		// 形が変わる速さ (4D ノイズの 4 軸目) [1/s]

	float HeatGain = 3.0f;				// 熱の持ち上げ (大きいほど芯が太く明るい)
	float FlameOpacity = 0.5f;			// 炎のすすの密度 (熱 x この値)
	float SmokeDensity = 0.15f;			// 煙の密度
	float SmokeStart = 0.6f;			// 煙が出始める高さ (ボックスの高さ比)

	void Sanitize();
	unsigned long long ComputeHash() const;

	// シード由来のノイズ空間オフセット
	float ComputeNoiseOffset() const;
};

// ------------------------------------------------------------
//  FHeterogeneousVolumeMaterial
//  Volume ドメインのマテリアル出力 (Albedo / Extinction / Emissive) と
//  散乱の設定。Extinction = 密度 x ExtinctionScale。
//  位相関数 / マルチスキャッタ / 天空光の項目は UE の Heterogeneous Volume
//  には無い (UE は等方位相 + 単散乱) が、雲らしい見た目に必要なので
//  Volumetric Cloud のマテリアルと同じ考え方で持つ [PORT]
// ------------------------------------------------------------
struct FHeterogeneousVolumeMaterial
{
	XMFLOAT3 Albedo = { 1.0f, 1.0f, 1.0f };
	float    Extinction = 6.0f;						// [1/m] (密度 1 のとき)
	XMFLOAT3 Emissive = { 0.0f, 0.0f, 0.0f };		// [/m]

	// 2 ローブ Henyey-Greenstein 位相関数
	float PhaseGForward = 0.6f;						// 前方ローブの g
	float PhaseGBackward = -0.3f;					// 後方ローブの g
	float PhaseForwardWeight = 0.7f;				// 前方ローブの重み (0..1)

	// マルチスキャッタのオクターブ近似 (Wrenninge 2015 / Volumetric Cloud と同じ)
	int   MultiScatteringOctaves = 4;				// 1 = 単散乱のみ (1..4)
	float MultiScatteringContribution = 0.75f;		// オクターブごとの寄与の倍率
	float MultiScatteringOcclusion = 0.35f;			// オクターブごとの遮蔽 (光学的深さ) の倍率
	float MultiScatteringEccentricity = 0.5f;		// オクターブごとの位相関数 g の倍率

	// 天空光 (IBL irradiance) と地面の照り返し
	float AmbientIntensity = 1.0f;
	float AmbientOcclusionScale = 1.0f;				// 天空光の遮蔽に使う光学的深さの倍率
	float GroundBounce = 0.5f;						// 下半球 (地面) の寄与 (0..1)

	float DirectionalLightIntensityScale = 1.0f;	// 太陽光の倍率

	// 黒体放射 (BlackBody ノード相当)。密度ボリュームの熱 (0..1) を
	// TemperatureMin..Max [K] へ写し、色 x pow(0.0004 T, 4) x Intensity x 熱 で発光する。
	// Intensity = 0 で無効 (雲)
	float BlackbodyTemperatureMin = 1200.0f;		// 熱 0 の温度 [K]
	float BlackbodyTemperatureMax = 2700.0f;		// 熱 1 の温度 [K]
	float BlackbodyIntensity = 0.0f;				// 発光の倍率 [/m]
};

// ------------------------------------------------------------
//  FHeterogeneousVolumeData
//  IHeterogeneousVolumeInterface / FHeterogeneousVolumeData 相当。
//  プロキシが持つ生成時スナップショットで、レンダラはこれだけを読む。
//  LocalToWorld / Bounds はプロキシのトランスフォームプッシュに追従する。
// ------------------------------------------------------------
struct FHeterogeneousVolumeData
{
	XMFLOAT4X4       LocalToWorld{};	// 単位ボックス -> ワールド (転置前)
	FBoxSphereBounds Bounds;			// ワールド境界 (後→前ソート用)

	XMUINT3 VolumeResolution = { 192, 128, 192 };
	float   StepFactor = 1.0f;
	float   ShadowStepFactor = 2.0f;
	float   ShadowBiasFactor = 0.5f;
	float   LightingDownsampleFactor = 2.0f;

	EHeterogeneousVolumeSource   Source = EHeterogeneousVolumeSource::ProceduralCloud;
	FProceduralCloudParams       Cloud;
	FProceduralFireParams        Fire;
	FHeterogeneousVolumeMaterial Material;

	// 密度ボリューム / ライティングキャッシュ (コンポーネントと共有。プロキシを
	// 作り直しても GPU リソースは引き継がれる)
	std::shared_ptr<FHeterogeneousVolumeRenderResource> RenderResource;
};

// ============================================================
//  UHeterogeneousVolumeComponent
// ============================================================
class UHeterogeneousVolumeComponent : public UPrimitiveComponent
{
private:
	// ---- UHeterogeneousVolumeComponent の同名プロパティ ----
	XMUINT3 m_VolumeResolution = { 192, 128, 192 };	// 密度ボリュームの解像度 (既定 128^3)
	float   m_StepFactor = 1.0f;					// レイマーチのステップ = 最小ボクセル幅 x StepFactor
	float   m_ShadowStepFactor = 2.0f;				// ライティングキャッシュのステップ = ボクセル幅 x ShadowStepFactor
	float   m_ShadowBiasFactor = 0.5f;				// シャドウレイの開始オフセット = ボクセル幅 x ShadowBiasFactor
	float   m_LightingDownsampleFactor = 2.0f;		// ライティングキャッシュ解像度 = 密度解像度 / この値

	EHeterogeneousVolumeSource   m_Source = EHeterogeneousVolumeSource::ProceduralCloud;
	FProceduralCloudParams       m_Cloud;
	FProceduralFireParams        m_Fire;
	FHeterogeneousVolumeMaterial m_Material;

	std::shared_ptr<FHeterogeneousVolumeRenderResource> m_RenderResource;

public:
	UHeterogeneousVolumeComponent();

	// ---- プロパティ (変更はプロキシ再生成で反映。GPU リソースは引き継ぐ) ----
	void           SetVolumeResolution(const XMUINT3& Resolution);
	const XMUINT3& GetVolumeResolution() const { return m_VolumeResolution; }

	void  SetStepFactor(float Value) { m_StepFactor = Value; MarkRenderStateDirty(); }
	float GetStepFactor() const { return m_StepFactor; }

	void  SetShadowStepFactor(float Value) { m_ShadowStepFactor = Value; MarkRenderStateDirty(); }
	float GetShadowStepFactor() const { return m_ShadowStepFactor; }

	void  SetShadowBiasFactor(float Value) { m_ShadowBiasFactor = Value; MarkRenderStateDirty(); }
	float GetShadowBiasFactor() const { return m_ShadowBiasFactor; }

	void  SetLightingDownsampleFactor(float Value) { m_LightingDownsampleFactor = Value; MarkRenderStateDirty(); }
	float GetLightingDownsampleFactor() const { return m_LightingDownsampleFactor; }

	// ---- 密度のソース (雲 / 炎) ----
	void SetVolumeSource(EHeterogeneousVolumeSource Source) { m_Source = Source; MarkRenderStateDirty(); }
	EHeterogeneousVolumeSource GetVolumeSource() const { return m_Source; }

	const FProceduralFireParams& GetFireParams() const { return m_Fire; }
	void SetFireParams(const FProceduralFireParams& Params) { m_Fire = Params; m_Fire.Sanitize(); MarkRenderStateDirty(); }

	// 直接書き換えた場合は MarkRenderStateDirty() を呼ぶこと
	FProceduralCloudParams& GetCloudParams() { return m_Cloud; }
	const FProceduralCloudParams& GetCloudParams() const { return m_Cloud; }
	void SetCloudParams(const FProceduralCloudParams& Params) { m_Cloud = Params; m_Cloud.Sanitize(); MarkRenderStateDirty(); }

	FHeterogeneousVolumeMaterial& GetVolumeMaterial() { return m_Material; }
	const FHeterogeneousVolumeMaterial& GetVolumeMaterial() const { return m_Material; }
	void SetVolumeMaterial(const FHeterogeneousVolumeMaterial& Material) { m_Material = Material; MarkRenderStateDirty(); }

	const std::shared_ptr<FHeterogeneousVolumeRenderResource>& GetRenderResource() const { return m_RenderResource; }

	// ---- 境界 (単位ボックスをワールドへ) ----
	FBoxSphereBounds CalcBounds(const XMMATRIX& LocalToWorld) const override;

	// ---- レンダー側ミラー生成 ----
	FPrimitiveSceneProxy* CreateSceneProxy() override;
};
