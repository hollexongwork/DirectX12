#pragma once
#include "RenderManager.h"
#include "ShadowRendering.h"
#include "Scene.h"

class FLightGridInjection;
class FShadowSceneRenderer;

// ============================================================
//  FVolumetricFog
//  VolumetricFog.cpp (FDeferredShadingSceneRenderer::ComputeVolumetricFog)
//  に相当する Volumetric Fog の froxel ボリューム構築。
//
//  視錐台を XY = VOLUMETRIC_FOG_GRID_PIXEL_SIZE (8px) タイル、
//  Z = VOLUMETRIC_FOG_GRID_SIZE_Z (64) の指数スライス
//  (r.VolumetricFog.DepthDistributionScale = 32) に分割した froxel
//  ごとに、毎フレーム 3 つのコンピュートパスで
//  「カメラから各 froxel までの累積インスキャッタ + 透過率」を作る:
//
//    Pass 1 (VolumetricFogAttributes_CS):
//        指数高さフォグの密度 -> 散乱 / 吸収 (VBufferA) + 発光 (VBufferB)
//    Pass 2 (VolumetricFogLightScattering_CS):
//        ディレクショナル (CSM 影) + ローカルライト (ライトグリッド +
//        シャドウアトラス) + スカイ (IBL irradiance) を HG 位相関数で
//        散乱 -> LightScattering。前フレームをセル中心の再投影で
//        採光しテンポラル蓄積 (r.VolumetricFog.HistoryWeight = 0.9)
//    Pass 3 (VolumetricFogIntegration_CS):
//        Z 方向に手前から積分 -> IntegratedLightScattering (t34)
//
//  受光側 (HeightFogPS / TranslucentPS) は b7 (FogUniformParameters) の
//  GridZParams / SVPosToVolumeUV でボリューム UV を求め、
//  HeightFogCommon.hlsl の CombineVolumetricFog がサンプルする。
//
//  依存モデルは FLightGridInjection と同じ:
//    - 独立コンピュートルートシグネチャ
//    - RenderManager の公開アクセサ経由 (device / command list /
//      デスクリプタ確保)
//    - フレーム内 Dispatch (コマンドリストは flush しない)
//  シャドウマップ / ライトグリッドはそれぞれの所有者 (FShadowSceneRenderer /
//  FLightGridInjection) の常在状態 (PIXEL_SHADER_RESOURCE) から
//  NON_PIXEL へ一時遷移して読み、Dispatch 後に戻す。
// ============================================================

// ---- グリッド定数 (r.VolumetricFog.* の既定値と同一) ----
static const unsigned int VOLUMETRIC_FOG_GRID_PIXEL_SIZE = 8;        // r.VolumetricFog.GridPixelSize
static const unsigned int VOLUMETRIC_FOG_GRID_SIZE_Z = 64;           // r.VolumetricFog.GridSizeZ
static const float        VOLUMETRIC_FOG_DEPTH_DISTRIBUTION_SCALE = 32.0f; // r.VolumetricFog.DepthDistributionScale

// ------------------------------------------------------------
//  FVolumetricFogInputs
//  今フレームの解決済み状態 (FSceneRenderer が詰める)。
//  Volumetric Fog はゲーム側オブジェクトに触れない。
// ------------------------------------------------------------
struct FVolumetricFogInputs
{
	const VIEW_CONSTANT* View = nullptr;				// カメラの VIEW 定数 (転置済み行列)
	const FORWARD_LIGHT_CONSTANT* ForwardLightData = nullptr;	// b3 (ライトグリッドパラメータ)
	XMFLOAT4X4                    PrevViewProjectionT{};		// 前フレームの View x Projection (転置済み)
	bool                          bHistoryValid = false;		// 前フレームの行列 / 履歴が有効か

	unsigned int                  LightBufferSRVIndex = 0;		// t13 と同じライトバッファ
	FLightGridInjection* LightGrid = nullptr;			// t19 / t20 (null = 全灯ループ)
	FShadowSceneRenderer* ShadowRenderer = nullptr;		// CSM / ローカルアトラス / b5 定数
	unsigned int                  SkyIrradianceSRVIndex = 0;	// IBL irradiance キューブ (スカイ項)

	// ディレクショナルライトは ForwardLightData (b3) の「選択されたフォワードディレクショナルライト」を使う

	const FExponentialHeightFogSceneInfo* FogInfo = nullptr;	// 解決済みフォグ (null = 無効)
};

class FVolumetricFog
{
public:
	// ---- 制御 (r.VolumetricFog.* 相当。ImGui: Fog コンポーネントの Details から操作) ----
	struct Params
	{
		bool  bTemporalReprojection = true;		// r.VolumetricFog.TemporalReprojection
		bool  bJitter = true;					// r.VolumetricFog.Jitter (セル内サンプル位置の Halton ジッタ)
		float HistoryWeight = 0.9f;				// r.VolumetricFog.HistoryWeight
		float InverseSquaredLightDistanceBiasScale = 1.0f; // r.VolumetricFog.InverseSquaredLightDistanceBiasScale
	};

	// ---- 統計 (ImGui 表示用) ----
	struct Stats
	{
		unsigned int GridSizeX = 0;
		unsigned int GridSizeY = 0;
		unsigned int GridSizeZ = 0;
		unsigned int NumFroxels = 0;
		bool         bDispatchedThisFrame = false;
	};

private:
	// HLSL 側 (VolumetricFogCommon.hlsl) の cbuffer FVolumetricFogParams (b0)
	// と 1:1 ミラー必須 (720 bytes)。
	struct FVolumetricFogParams
	{
		XMFLOAT4X4 ViewToWorld;						// ビュー -> ワールド (転置済み)
		XMFLOAT4X4 PrevWorldToClip;					// 前フレーム View x Projection (転置済み)
		XMFLOAT4X4 WorldToShadowCascade[MAX_SHADOW_CASCADES];	// CSM (転置済み)

		XMFLOAT4   CascadeSplits;
		XMFLOAT4   CascadeDepthBias;
		XMFLOAT4   DirectionalShadowParams;			// x=NumCascades, y=ShadowDistance, z=FadeStart, w=1/解像度

		XMFLOAT4   GridSize;						// xyz = froxel 数, w = GridPixelSize
		XMFLOAT4   GridZParams;						// xyz = (B, O, S), w = 1/GridSizeZ
		XMFLOAT4   ScreenSize;						// xy = レンダー解像度 R (m_ViewWidth/Height), zw = 1/xy
		XMFLOAT4   ProjectionParams;				// x = 1/P._11, y = 1/P._22, z = Near, w = MaxDistance
		XMFLOAT4   CameraOrigin;
		XMFLOAT4   FrameJitter;						// xyz = セルオフセット, w = HistoryWeight
		XMFLOAT4   TemporalParams;					// x = テンポラル, y = 履歴有効, z = 逆二乗バイアススケール

		XMFLOAT4   FogDensityParams0;				// x = Density0, y = Falloff0, z = Height0, w = ExtinctionScale
		XMFLOAT4   FogDensityParams1;				// x = Density1, y = Falloff1, z = Height1, w = PhaseG
		XMFLOAT4   FogAlbedo;						// rgb, w = StartDistance
		XMFLOAT4   FogEmissive;						// rgb [/m], w = NearFadeInDistance
		XMFLOAT4   FogInscatteringColor;			// rgb, w = bOverrideLightColors
		XMFLOAT4   DirectionalInscatteringColor;	// rgb, w = StaticLightingScatteringIntensity

		XMFLOAT4   DirectionalLightDirection;		// xyz, w = enabled (b3 の選択されたフォワードディレクショナルライト)
		XMFLOAT4   DirectionalLightColor;			// rgb x VolumetricScatteringIntensity

		unsigned int NumLocalLights;
		unsigned int CulledGridSizeX;
		unsigned int CulledGridSizeY;
		unsigned int CulledGridSizeZ;

		unsigned int LightGridPixelSizeShift;
		unsigned int MaxCulledLightsPerCell;
		unsigned int bUseLightGrid;
		unsigned int PadA;

		XMFLOAT3     LightGridZParams;
		unsigned int PadB;
	};
	static_assert(sizeof(FVolumetricFogParams) == 720,
		"FVolumetricFogParams must mirror HLSL cbuffer FVolumetricFogParams (b0)");

	RenderManager* m_Owner = nullptr;
	Params         m_Params;
	Stats          m_Stats;

	// ---- グリッド次元 (CreateVolumes でレンダー解像度 R から確定) ----
	unsigned int m_GridSizeX = 1;
	unsigned int m_GridSizeY = 1;
	unsigned int m_GridSizeZ = VOLUMETRIC_FOG_GRID_SIZE_Z;

	// ボリュームを作ったビュー (レンダー) 解像度。b0 の ScreenSize に使う (バックバッファではない)
	unsigned int m_ViewWidth = 1;
	unsigned int m_ViewHeight = 1;

	// 独立コンピュートルートシグネチャ (3 パス共通):
	//  [0]  CBV  b0  (FVolumetricFogParams)
	//  [1]  SRV  t0  ForwardLightBuffer        [2]  SRV t1  LocalShadowParams
	//  [3]  SRV  t2  NumCulledLightsGrid       [4]  SRV t3  CulledLightDataGrid
	//  [5]  SRV  t4  DirectionalShadowCascades [6]  SRV t5  LocalLightShadows
	//  [7]  SRV  t6  VBufferA                  [8]  SRV t7  VBufferB
	//  [9]  SRV  t8  LightScatteringHistory    [10] SRV t9  LightScatteringTexture
	//  [11] SRV  t10 SkyIrradiance
	//  [12] UAV  u0  RWVBufferA                [13] UAV u1  RWVBufferB
	//  [14] UAV  u2  RWLightScattering         [15] UAV u3  RWIntegratedLightScattering
	//  static sampler s0 (線形クランプ) / s1 (シャドウ比較)
	static constexpr unsigned int NUM_SRV_SLOTS = 11;	// t0..t10 (ルートパラメータ [1] から)
	static constexpr unsigned int NUM_UAV_SLOTS = 4;	// u0..u3 (ルートパラメータ [1 + NUM_SRV_SLOTS] から)
	ComPtr<ID3D12RootSignature> m_RootSignature;
	ComPtr<ID3D12PipelineState> m_PSOAttributes;
	ComPtr<ID3D12PipelineState> m_PSOLightScattering;
	ComPtr<ID3D12PipelineState> m_PSOIntegration;

	// ---- ボリュームテクスチャ (Texture3D, R16G16B16A16_FLOAT) ----
	struct FVolumeTexture
	{
		ComPtr<ID3D12Resource> Resource;
		unsigned int           SRVIndex = 0;
		unsigned int           UAVIndex = 0;
		D3D12_RESOURCE_STATES  State = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
	};

	FVolumeTexture m_VBufferA;				// 散乱 rgb + 吸収
	FVolumeTexture m_VBufferB;				// 発光 rgb
	FVolumeTexture m_LightScattering[2];	// ピンポン (今フレーム / 履歴)
	FVolumeTexture m_IntegratedLightScattering;	// t34
	unsigned int   m_LightScatteringFrame = 0;	// 今フレームの書き込み先

	// b0 アップロードバッファ x2 (2 フレームインフライト)
	ComPtr<ID3D12Resource> m_ParamBuffer[2];
	void* m_ParamPtr[2] = {};
	unsigned int           m_ParamFrame = 0;

	// テンポラルジッタのフレーム番号 / 履歴の有効性
	unsigned int m_FrameNumber = 0;
	bool         m_bHistoryValid = false;

	ID3D12Device* Device();
	ID3D12GraphicsCommandList* CommandList();
	ComPtr<ID3D12PipelineState> CreateComputePipeline(const char* csoFile);
	void CreateVolumeTexture(FVolumeTexture& Out, const wchar_t* Name);
	void Transition(FVolumeTexture& Volume, D3D12_RESOURCE_STATES NewState);

public:
	explicit FVolumetricFog(RenderManager* owner);
	~FVolumetricFog();

	void Init();	// RS / PSO / b0 バッファ + CreateVolumes(バックバッファ解像度)

	// レンダー解像度 Width x Height からグリッド次元 (ceil(W/8), ceil(H/8), 64) を決め、
	// 5 枚のボリューム (UAV 状態) と SRV / UAV を作る。m_bHistoryValid = false にする
	// (履歴ボリュームが未定義のため)。テンポラルジッタの位相 (m_FrameNumber) と
	// ピンポン (m_LightScatteringFrame) は触らない = 再確保しても再確保無しの実行とフレーム整合を保つ。
	// 先に ReleaseVolumes を呼んでおくこと
	void CreateVolumes(unsigned int Width, unsigned int Height);

	// 5 枚のボリュームとその SRV / UAV 枠を遅延削除キューへ返す (デストラクタも使う)
	void ReleaseVolumes();

	// froxel Z 分布パラメータ (GetVolumetricFogGridZParams 相当)。
	// FOG 定数 (b7) の VolumetricFogGridZParams にも使う。
	static XMFLOAT3 ComputeGridZParams(float NearPlane, float FarPlane);

	// Volumetric Fog の最大距離 (froxel 最終スライスの開始境界)。near + 1m 未満にはしない。
	// FOG 定数 (b7) とコンピュート b0 の両方がこれを使うこと (不一致だと受光側のスライス参照がずれる)。
	static float ComputeMaxDistance(float NearPlane, float VolumetricFogDistance)
	{
		return (VolumetricFogDistance > NearPlane + 1.0f) ? VolumetricFogDistance : (NearPlane + 1.0f);
	}

	// 今フレームの 3 パスを記録する。FogInfo が無効 / bEnableVolumetricFog
	// = false のフレームは何もしない (呼び出し側が ApplyVolumetricFog = 0 にする)。
	void Dispatch(const FVolumetricFogInputs& Inputs);

	// フォグパス / トランスルーセンシーにバインドする SRV (t34)
	unsigned int GetIntegratedLightScatteringSRVIndex() const { return m_IntegratedLightScattering.SRVIndex; }

	// ---- アクセサ ----
	unsigned int GetGridSizeX() const { return m_GridSizeX; }
	unsigned int GetGridSizeY() const { return m_GridSizeY; }
	unsigned int GetGridSizeZ() const { return m_GridSizeZ; }

	Params& GetParams() { return m_Params; }
	const Params& GetParams() const { return m_Params; }
	const Stats& GetStats() const { return m_Stats; }
};
