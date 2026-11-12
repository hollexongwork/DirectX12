#pragma once
#include "RenderManager.h"
#include "HeterogeneousVolumeComponent.h"
#include <vector>

// ============================================================
//  FHeterogeneousVolumeRenderer
//  FDeferredShadingSceneRenderer::RenderHeterogeneousVolumes
//  (HeterogeneousVolumes.cpp) に相当する Heterogeneous Volume の描画。
//
//  ボリュームごとに 2 枚の Texture3D を持つ:
//    密度ボリューム         : VolumeResolution。プロシージャルのベイク結果
//                             (UE の Sparse Volume Texture の代わり)。
//                             雲 = R16_FLOAT (密度)、炎 = R16G16_FLOAT (密度 + 熱)
//    ライティングキャッシュ : RGBA16F, VolumeResolution / LightingDownsampleFactor。
//                             太陽 / 上空コーン / 真下の光学的長さ ∫density ds [m]
//                             (UE の Lighting Cache = Transmittance Volume)
//
//  フレーム内の流れ:
//    PrepareVolumes (RenderLighting 冒頭, コンピュート):
//      1. 形のパラメータ / 解像度 / 縦横比が変わったボリュームだけ密度をベイク
//         (雲: HeterogeneousVolumeCloudBake_CS)。炎 (HeterogeneousVolumeFireBake_CS) は
//         時間がキーに入るので毎フレームベイクする
//      2. 密度 / ライト方向 / 変換 / シャドウ設定が変わったボリュームだけ
//         ライティングキャッシュを更新 (HeterogeneousVolumeLightingCache_CS)
//    RenderHeterogeneousVolumes (フォグパス直後, SceneColor が RT):
//      後→前の順に、フルスクリーンクアッド + シザー (OBB の画面矩形) で
//      レイマーチし One / SrcAlpha で合成する (PSO "HeterogeneousVolume",
//      DeferredVS + HeterogeneousVolumePS)。b2 = FHeterogeneousVolumePassConstant,
//      t41 = 密度, t42 = ライティングキャッシュ, t3 = 深度。IBL (t6) と
//      フォグ (b7 / t33 / t34) は呼び出し側がバインドする
//
//  コンピュートは FVolumetricFog と同じ依存モデル (独立ルートシグネチャ /
//  フレーム内 Dispatch / コマンドリストは flush しない)。
//
//  [PORT] UE との違い:
//    - 密度は SVT ではなくプロシージャルのベイク
//    - ライトは選択されたフォワードディレクショナルライト 1 灯 + 天空光のみ
//      (ローカルライト未対応)
//    - ボリュームがシーンへ落とす影 (Adaptive Volumetric Shadow Map)、
//      低解像度レンダリング + アップサンプル、SVT のストリーミング /
//      アニメーション再生は無い
// ============================================================

// ------------------------------------------------------------
//  FHeterogeneousVolumeRenderResource
//  ボリューム 1 個分の GPU リソース。UHeterogeneousVolumeComponent が
//  shared_ptr で持ち、プロキシ (FHeterogeneousVolumeData) と共有する。
//  中身は FHeterogeneousVolumeRenderer が遅延生成 / 更新する。
// ------------------------------------------------------------
struct FHeterogeneousVolumeRenderResource
{
	struct FVolumeTexture
	{
		ComPtr<ID3D12Resource> Resource;
		unsigned int           SRVIndex = 0;
		unsigned int           UAVIndex = 0;
		D3D12_RESOURCE_STATES  State = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
		XMUINT3                Size = { 0, 0, 0 };
		DXGI_FORMAT            Format = DXGI_FORMAT_UNKNOWN;
	};

	FVolumeTexture Density;			// 雲 = R16_FLOAT (密度) / 炎 = R16G16_FLOAT (密度 + 熱)
	FVolumeTexture LightingCache;	// R16G16B16A16_FLOAT

	// ベイク済みの入力のキー (一致する間は作り直さない)
	unsigned long long DensityKey = 0;
	unsigned long long LightingKey = 0;
	bool bDensityValid = false;
	bool bLightingValid = false;

	FHeterogeneousVolumeRenderResource() = default;
	~FHeterogeneousVolumeRenderResource();

	FHeterogeneousVolumeRenderResource(const FHeterogeneousVolumeRenderResource&) = delete;
	FHeterogeneousVolumeRenderResource& operator=(const FHeterogeneousVolumeRenderResource&) = delete;

	// リソースとデスクリプタ枠を遅延削除キューへ返す
	static void ReleaseTexture(FVolumeTexture& Texture);
};

class FHeterogeneousVolumeRenderer
{
public:
	// ---- 制御 (ImGui: Heterogeneous Volume コンポーネントの Details から操作) ----
	// r.HeterogeneousVolumes.* に相当
	struct Params
	{
		bool bEnable = true;				// r.HeterogeneousVolumes
		bool bJitter = true;				// r.HeterogeneousVolumes.Jitter (レイ開始位置のジッタ。TAA が解像する)
		int  MaxStepCount = 256;			// r.HeterogeneousVolumes.MaxStepCount (UE 既定 512)
		int  MaxShadowStepCount = 64;		// ライティングキャッシュのシャドウレイの最大ステップ数
		int  AmbientStepCount = 12;			// 天空光 / 地面方向の遮蔽レイのステップ数
		bool bIndirectLighting = true;		// r.HeterogeneousVolumes.IndirectLighting (天空光)
		bool bApplyFog = true;				// r.HeterogeneousVolumes.HeightFog / .VolumetricFog
	};

	// ---- 統計 (ImGui 表示用) ----
	struct Stats
	{
		unsigned int NumVolumes = 0;				// 今フレーム描いたボリューム数
		unsigned int NumDensityBakes = 0;			// 今フレームの密度ベイク数
		unsigned int NumLightingCacheUpdates = 0;	// 今フレームのライティングキャッシュ更新数
		unsigned int TotalDensityBakes = 0;			// 起動からの累計
		unsigned int TotalLightingCacheUpdates = 0;
		unsigned long long VolumeMemoryBytes = 0;	// 描いたボリュームのテクスチャ合計
	};

private:
	// HLSL 側 (HeterogeneousVolumeCommon.hlsl) の cbuffer
	// FHeterogeneousVolumeComputeParams (b0) と 1:1 ミラー必須 (832 bytes)
	struct FHeterogeneousVolumeComputeParams
	{
		XMFLOAT4 VolumeResolution;
		XMFLOAT4 CacheResolution;
		XMFLOAT4 AspectScale;			// xyz = 半幅 / 最大半幅, w = NumPuffs

		XMFLOAT4 ShapeParams;			// SmoothUnion / 底面の高さ / 底面のなめらかさ / EdgeSoftness
		XMFLOAT4 BillowParams;			// Frequency / Strength / Octaves / PerlinMix
		XMFLOAT4 DetailParams;			// Frequency / Strength / Octaves / DensityVariation
		XMFLOAT4 NoiseOffset;			// xyz = ノイズ空間オフセット, w = BorderFade

		XMFLOAT4 SunDirectionBox;		// xyz = ボックス空間の移動量 / m, w = 有効
		XMFLOAT4 ShadowParams;			// ShadowStepSize [m] / MaxShadowStepCount / ShadowBias [m] / AmbientStepCount
		XMFLOAT4 AmbientDirectionBox[5];
		XMFLOAT4 GroundDirectionBox;

		XMFLOAT4 Puffs[FProceduralCloudParams::MaxPuffs];

		// ---- 炎 (FProceduralFireParams) ----
		XMFLOAT4 FireShape;				// BaseRadius / TipRadiusScale / TaperPower / FlameHeight
		XMFLOAT4 FireNoise;				// NoiseFrequency [1/m] / NoiseStrength / HeightErosion / Octaves
		XMFLOAT4 FireMotion;			// RiseSpeed [m/s] / EvolutionSpeed [1/s] / Turbulence / Time [s]
		XMFLOAT4 FireSmoke;				// FlameOpacity / SmokeDensity / SmokeStart / HeatGain
		XMFLOAT4 FireExtent;			// xyz = 半幅 [m], w = ノイズ空間オフセット
	};
	static_assert(sizeof(FHeterogeneousVolumeComputeParams) == 832,
		"FHeterogeneousVolumeComputeParams must mirror HLSL cbuffer FHeterogeneousVolumeComputeParams (b0)");

	// HLSL 側 (HeterogeneousVolumePS.hlsl) の cbuffer
	// HeterogeneousVolumeConstantBuffer (b2) と 1:1 ミラー必須 (176 bytes)
	struct FHeterogeneousVolumePassConstant
	{
		XMFLOAT4X4 WorldToBox;			// 転置済み
		XMFLOAT4   Albedo;				// rgb, w = Extinction [1/m]
		XMFLOAT4   Emissive;			// rgb [/m], w = AmbientIntensity
		XMFLOAT4   MarchParams;			// StepSize [m] / MaxStepCount / Jitter / ApplyFog
		XMFLOAT4   PhaseParams;			// g 前方 / g 後方 / 前方の重み / IndirectLighting
		XMFLOAT4   MultiScatter;		// Contribution / Occlusion / Eccentricity / Octaves
		XMFLOAT4   LightParams;			// DirectionalLightIntensityScale / AmbientOcclusionScale / GroundBounce / 未使用
		XMFLOAT4   Blackbody;			// TemperatureMin [K] / TemperatureMax [K] / Intensity [/m] / 未使用
	};
	static_assert(sizeof(FHeterogeneousVolumePassConstant) == 176,
		"FHeterogeneousVolumePassConstant must mirror HLSL cbuffer HeterogeneousVolumeConstantBuffer (b2)");

	RenderManager* m_Owner = nullptr;
	Params         m_Params;
	Stats          m_Stats;

	// コンピュートルートシグネチャ (ベイク / ライティングキャッシュ共通):
	//  [0] CBV b0 (ルート CBV)  [1] SRV t0 (密度)  [2] UAV u0 (出力)  static sampler s0 (線形クランプ)
	ComPtr<ID3D12RootSignature> m_RootSignature;
	ComPtr<ID3D12PipelineState> m_PSOCloudBake;
	ComPtr<ID3D12PipelineState> m_PSOFireBake;
	ComPtr<ID3D12PipelineState> m_PSOLightingCache;

	// b0 リング (1 Dispatch = 1 スロット。2 フレームインフライト)
	static constexpr unsigned int PARAM_SLOT_SIZE = 1024;	// 832 B を 256 B 境界へ
	static constexpr unsigned int PARAM_SLOT_COUNT = 32;	// 1 フレームの Dispatch 数の上限
	ComPtr<ID3D12Resource> m_ParamBuffer[2];
	void* m_ParamPtr[2] = {};
	unsigned int           m_ParamSlot = 0;

	ID3D12Device* Device();
	ID3D12GraphicsCommandList* CommandList();
	ComPtr<ID3D12PipelineState> CreateComputePipeline(const char* csoFile);

	// テクスチャ (Texture3D, UAV 状態で開始) を Size / Format で作り直す
	void CreateVolumeTexture(FHeterogeneousVolumeRenderResource::FVolumeTexture& Out,
		const XMUINT3& Size, DXGI_FORMAT Format, const wchar_t* Name);
	void Transition(FHeterogeneousVolumeRenderResource::FVolumeTexture& Texture, D3D12_RESOURCE_STATES NewState);

	// b0 リングへ書いて GPU アドレスを返す (溢れたら 0)
	D3D12_GPU_VIRTUAL_ADDRESS UploadComputeParams(const FHeterogeneousVolumeComputeParams& Params);

	// ボリューム 1 個の密度 / ライティングキャッシュを必要なら更新する。
	// Time [s] は炎 (アニメーションするソース) のベイクに使う
	void UpdateVolume(const FHeterogeneousVolumeData& Volume, const VIEW_CONSTANT& View, float Time);

	// ボリュームの半幅 [m] (LocalToWorld の各軸の長さ) と形状空間のスケール
	static XMFLOAT3 ComputeExtent(const XMFLOAT4X4& LocalToWorld);
	static XMFLOAT3 ComputeAspectScale(const XMFLOAT3& Extent);
	// レイマーチの基準になる最小ボクセル幅 [m]
	static float ComputeMinVoxelSize(const XMFLOAT3& Extent, const XMUINT3& Resolution);

public:
	explicit FHeterogeneousVolumeRenderer(RenderManager* owner);
	~FHeterogeneousVolumeRenderer();

	void Init();	// コンピュート RS / PSO / b0 リング

	// 可視ボリュームの密度ベイクとライティングキャッシュ更新 (コンピュート)。
	// グラフィックスのバインドには触れない。RenderLighting の冒頭
	// (SceneColor へ描く前) で呼ぶ
	void PrepareVolumes(const std::vector<const FHeterogeneousVolumeData*>& Volumes, const VIEW_CONSTANT& View, float Time);

	// SceneColor が RENDER_TARGET の状態で呼ぶ。後→前の順にレイマーチして合成する。
	// b0 (VIEW) / IBL (t6) / フォグ (b7, t33, t34) は呼び出し側がバインド済みであること
	void RenderHeterogeneousVolumes(const std::vector<const FHeterogeneousVolumeData*>& Volumes,
		const VIEW_CONSTANT& View, const VERTEX_BUFFER* ScreenQuad, unsigned int DepthSRVIndex);

	Params& GetParams() { return m_Params; }
	const Params& GetParams() const { return m_Params; }
	const Stats& GetStats() const { return m_Stats; }
};
