#pragma once
#include "RenderManager.h"

#include <vector>
#include <deque>
#include <memory>

class FScene;
class FPrimitiveSceneProxy;
class FLumenHardwareRayTracing;

// ============================================================
//  LumenScene
//  Lumen 相当のグローバルイルミネーションシステム。
//
//  ---- Surface Cache (エミッシブ光源化の本体) ----
//    1. MeshCards 生成 : SDF を持つ各プリミティブに 6 方向のカード
//    2. カードキャプチャ : Albedo / Normal / Emissive / Depth を
//         アトラスへ焼く (フレーム予算制)
//    3. 直接光 -> Radiosity (多バウンス) -> 合成:
//         FinalLighting = (Direct + Indirect) * Albedo / π
//                       + Emissive * EmissiveBoost
//         ★ Emissive がここで光源化される ★
//
//  ---- トレース基盤 ----
//    SWRT: 近距離 = メッシュ SDF / 遠距離 = Global Distance Field
//          (カメラ追従クリップマップ x2, 毎フレーム再構築)
//    HWRT: DXR 1.1 RayQuery (BLAS = FBXModel / TLAS = 毎フレーム。
//          ヒット法線はメッシュ SDF 勾配のハイブリッド構成)
//    全レイ共通: スクリーンスペーストレース (前フレーム SceneColor)
//          を先に試し、ミスで上記へフォールバック
//
//  ---- Final Gather (スクリーン GI) ----
//    GatherMode 2 (既定): Screen Probe Gather
//      16px 毎プローブ -> hemi-octahedral 8x8 トレース -> 3x3 空間
//      フィルタ -> SH L1 + テンポラル蓄積 -> フル解像度積分 (t28)
//    GatherMode 1: ピクセル毎コーントレース (フォールバック)
//
//  ---- Reflections ----
//    ラフネス連動の反射レイ (スクリーン -> SDF/HWRT) で Surface
//    Cache を採光し、IBL スペキュラの prefiltered を差し替える (t29)
//
//  ---- Radiance Cache / Translucency GI ----
//    カメラ周囲 16^3 ワールドプローブ (トロイダル, 予算制更新) を
//    octahedral アトラス + SH L1 ボリューム化し、半透明パスが
//    採光する (t30-t32)
// ============================================================

// ---- Lumen 定数 ----
static const unsigned int LUMEN_CARD_RESOLUTION = 64;	// カード 1 枚の解像度 (LumenTracingCommon.hlsl と 1:1)
static const unsigned int LUMEN_CARDS_PER_OBJECT = 6;	// ±X/±Y/±Z
static const unsigned int MAX_LUMEN_OBJECTS = 32;		// オブジェクトスロット数
static const unsigned int MAX_LUMEN_CARDS = MAX_LUMEN_OBJECTS * LUMEN_CARDS_PER_OBJECT; // 192
static const unsigned int LUMEN_ATLAS_TILES_X = 16;		// アトラスのタイル列数
static const unsigned int LUMEN_ATLAS_TILES_Y = 12;		// アトラスのタイル行数 (16*12 = 192)
static const unsigned int LUMEN_ATLAS_WIDTH = LUMEN_ATLAS_TILES_X * LUMEN_CARD_RESOLUTION;  // 1024
static const unsigned int LUMEN_ATLAS_HEIGHT = LUMEN_ATLAS_TILES_Y * LUMEN_CARD_RESOLUTION; // 768

// ---- Global Distance Field ----
static const unsigned int LUMEN_GLOBAL_SDF_RESOLUTION = 128;	// クリップマップ解像度 (^3)
static const unsigned int LUMEN_GLOBAL_SDF_CLIPMAPS = 2;		// クリップマップ数

// ---- Screen Probe Gather ----
static const unsigned int LUMEN_PROBE_DOWNSAMPLE = 16;	// プローブ間隔 [px]
static const unsigned int LUMEN_PROBE_OCTA_RES = 8;		// octahedral 解像度 (8x8 = 64 レイ)

// ---- Radiance Cache ----
static const unsigned int LUMEN_RC_PROBES_PER_AXIS = 16;	// 16^3 = 4096 プローブ
static const unsigned int LUMEN_RC_ATLAS_SIZE = 512;		// 64x64 タイル x 8x8 octa

// ============================================================
//  GPU ミラー構造体 (HLSL Structs.hlsl と 1:1 ミラー必須)
// ============================================================

// FLumenSceneObject (t24)。SDF フィールドは FDFObjectData と同じ意味。
// VolumeUVAdd.w = SDF 1 ボクセルのワールド幅 [m] (マーチ歩幅 / バイアス基準)。
struct FLumenSceneObjectData
{
	XMFLOAT4X4 WorldToVolume;				// ワールド -> SDF ボリューム [-1,1] (転置済み)
	XMFLOAT4   VolumeUVScaleAndDistance;	// xyz=アトラス UV スケール, w=距離値 -> ワールド [m]
	XMFLOAT4   VolumeUVAdd;					// xyz=アトラス UV オフセット, w=ボクセルワールド幅 [m]
	unsigned int CardOffset = 0;			// カードバッファ内の先頭カード
	unsigned int NumCards = 0;
	unsigned int bValid = 0;
	unsigned int Pad0 = 0;
};
static_assert(sizeof(FLumenSceneObjectData) == 112,
	"FLumenSceneObjectData must be 112 bytes (HLSL Structs.hlsl と 1:1 ミラー)");

// FLumenCardData (t25)。カード空間 = キャプチャビュー空間
// (原点 = カードカメラ, XY = カード平面, +Z = 面へ向かう奥行き,
//  深度レンジ [0, 2 * CardExtent.z])。
struct FLumenCardGPUData
{
	XMFLOAT4X4 WorldToCard;			// ワールド -> カード空間 (転置済み)
	XMFLOAT4X4 CardToWorld;			// カード空間 -> ワールド (転置済み)
	XMFLOAT4   CardExtentAndValid;	// xyz=カード半幅 (x,y=平面, z=半深度), w=有効
	XMFLOAT4   AtlasUVScaleBias;	// カード UV [0,1] -> アトラス UV
	XMFLOAT4   CardDirection;		// xyz=ワールド空間カード法線 (外向き)
};
static_assert(sizeof(FLumenCardGPUData) == 176,
	"FLumenCardGPUData must be 176 bytes (HLSL Structs.hlsl と 1:1 ミラー)");

// ============================================================
//  b6 : LUMEN_CONSTANT (HLSL LumenSceneParameters と 1:1 ミラー必須)
//  デファードライティング / トランスルーセンシーパスが参照する。
// ============================================================
struct LUMEN_CONSTANT
{
	unsigned int NumLumenObjects = 0;
	unsigned int bLumenScreenGI = 0;		// 1 = ピクセル毎コーントレース経路
	unsigned int LumenNumScreenCones = 4;
	unsigned int LumenDebugMode = 0;

	float LumenGIIntensity = 1.0f;
	float LumenMaxTraceDistance = 15.0f;
	float LumenConeTanAngle = 0.66f;
	float LumenSkyOcclusionStrength = 1.0f;

	float LumenSurfaceBias = 0.05f;
	unsigned int LumenGatherMode = 2;		// 0=off 1=ピクセル毎 2=Screen Probe Gather
	unsigned int bLumenReflections = 0;
	float LumenReflectionMaxRoughness = 0.4f;

	float LumenReflectionIntensity = 1.0f;
	unsigned int bLumenTranslucencyGI = 0;
	float LumenTranslucencyGIIntensity = 1.0f;
	float LumenPadA = 0.0f;

	XMFLOAT4 LumenRadianceCacheParams0 = { 0.0f, 0.0f, 0.0f, 1.0f };
	XMFLOAT4 LumenRadianceCacheParams1 = { 16.0f, 0.0f, 1.0f, 0.0f };
};
static_assert(sizeof(LUMEN_CONSTANT) == 96,
	"LUMEN_CONSTANT must mirror HLSL LumenSceneParameters (b6)");

// ============================================================
//  FLumenFrameInputs
//  FSceneRenderer が毎フレーム解決して渡すビュー / ライト /
//  シーンテクスチャ情報 (Lumen 側はゲーム側オブジェクトに触れない)。
// ============================================================
struct FLumenFrameInputs
{
	// ---- ライト ----
	XMFLOAT4 DirectionalLightDirection = { 0.0f, 1.0f, 0.0f, 0.0f };	// 受光面 -> ライト
	XMFLOAT4 DirectionalLightColor = { 0.0f, 0.0f, 0.0f, 0.0f };
	unsigned int LightBufferSRVIndex = 0;	// t13 と同一の StructuredBuffer
	unsigned int NumLocalLights = 0;

	// ---- IBL ----
	unsigned int IrradianceSRVIndex = 0;	// 拡散 irradiance キューブ
	unsigned int PrefilterSRVIndex = 0;		// prefilter キューブ (ミップ付き)

	// ---- ビュー ----
	XMFLOAT4   CameraOrigin = { 0.0f, 0.0f, 0.0f, 0.0f };
	XMFLOAT4X4 ViewProjectionT{};		// 転置済み
	XMFLOAT4X4 InvViewProjectionT{};	// 転置済み
	unsigned int ScreenWidth = 0;
	unsigned int ScreenHeight = 0;

	// ---- 履歴 (スクリーンスペーストレース / テンポラル用) ----
	XMFLOAT4X4 PrevViewProjectionT{};
	XMFLOAT4   PrevCameraOrigin = { 0.0f, 0.0f, 0.0f, 0.0f };
	bool       bHistoryValid = false;

	// ---- シーンテクスチャ SRV ----
	unsigned int SceneDepthSRVIndex = 0;		// 非線形深度 (R32F)
	unsigned int LinearDepthSRVIndex = 0;		// RG32F (R=ビュー距離)
	unsigned int GBufferNormalSRVIndex = 0;		// GBufferA
	unsigned int GBufferBSRVIndex = 0;			// GBufferB (ラフネス)
	unsigned int PrevSceneColorSRVIndex = 0;	// 前フレーム SceneColor
};

// ============================================================
//  FLumenSceneData
// ============================================================
class FLumenSceneData
{
public:
	// ---- 制御パラメータ (ImGui: Lumen ウィンドウから操作) ----
	struct Params
	{
		bool  bEnabled = true;				// Lumen シーン全体 (キャプチャ + ライティング)
		int   GatherMode = 2;				// 0=off 1=ピクセル毎トレース 2=Screen Probe Gather
		int   NumScreenCones = 4;			// ピクセル毎経路のコーン数 (1..8)
		float GIIntensity = 1.0f;			// 拡散 GI の強度
		float EmissiveBoost = 1.0f;			// Surface Cache のエミッシブ倍率
		float MaxTraceDistance = 15.0f;		// トレース最大距離 [m]
		float SurfaceBias = 0.05f;			// レイ開始の法線オフセット [m]
		float SkyOcclusionStrength = 1.0f;	// IBL のスカイ可視率減衰 (0..1)
		int   NumRadiosityRays = 4;			// Radiosity のテクセルあたりレイ数
		int   RadiosityCardsPerFrame = 32;	// Radiosity のフレームあたり更新カード数
		int   CaptureBudgetPerFrame = 12;	// キャプチャのフレームあたりカード数
		unsigned int DebugMode = 0;			// 0=off 1=GIのみ 2=スカイ可視率 3=GI拡散寄与

		// ---- Global Distance Field ----
		bool  bGlobalSDF = true;			// 遠距離トレースにクリップマップを使う
		float GlobalSDFExtent0 = 12.8f;		// クリップマップ0 半径 [m] (1 は 4 倍)
		float DetailTraceDistance = 2.5f;	// メッシュ SDF で追う近距離 [m]

		// ---- Screen Probe Gather ----
		bool  bScreenSpaceTrace = true;		// 前フレーム SceneColor のスクリーントレース
		float ScreenTraceThickness = 0.3f;	// スクリーントレースの厚み判定 [m]
		float TemporalAlpha = 0.1f;			// プローブ SH のテンポラルブレンド率
		float SkySampleMip = 1.5f;			// スカイ採光の prefilter ミップ

		// ---- Reflections ----
		bool  bReflections = true;
		float ReflectionMaxRoughness = 0.4f;	// これ以上は IBL のみ
		float ReflectionFadeStart = 0.25f;		// フェード開始ラフネス
		float ReflectionIntensity = 1.0f;

		// ---- Radiance Cache / Translucency GI ----
		bool  bRadianceCache = true;
		bool  bTranslucencyGI = true;
		float TranslucencyGIIntensity = 1.0f;
		float RadianceCacheSpacing = 1.0f;			// プローブ間隔 [m]
		int   RadianceCacheProbesPerFrame = 128;	// フレームあたり更新プローブ数

		// ---- HWRT (DXR) ----
		bool  bUseHardwareRayTracing = true;	// 対応環境でのみ有効化される
	};

	// ---- 統計 (ImGui 表示用) ----
	struct Stats
	{
		unsigned int NumObjects = 0;		// 有効オブジェクト数
		unsigned int NumValidCards = 0;		// 有効カード数
		unsigned int NumPendingCaptures = 0;// キャプチャ待ちカード数
		unsigned int NumCapturedThisFrame = 0;
		unsigned int NumProbesX = 0;		// スクリーンプローブ数
		unsigned int NumProbesY = 0;
		unsigned int NumTLASInstances = 0;	// HWRT インスタンス数
		bool bHardwareRayTracingActive = false;	// 今フレーム HWRT でトレースしたか
	};

private:
	// HLSL 側 (LumenSceneLightingCommon.hlsl) の cbuffer LumenPassParams
	// (b0) と 1:1 ミラー必須。
	struct FLumenPassParams
	{
		unsigned int CardStartIndex;		// GDF ビルドではクリップマップ番号
		unsigned int NumCardsToProcess;
		unsigned int PassNumLumenObjects;
		unsigned int PassNumLocalLights;

		XMFLOAT4 PassDirectionalLightDirection;	// xyz=受光面->ライト, w=有効
		XMFLOAT4 PassDirectionalLightColor;		// rgb=線形色 x 強度 (lux)
		XMFLOAT4 PassAtlasParams;				// xy=1/アトラスサイズ, z=カード解像度, w=フレーム番号
		XMFLOAT4 PassTraceParams;				// x=最大距離, y=面バイアス, z=Radiosityレイ数, w=Emissiveブースト
		XMFLOAT4 PassGlobalSDF0;				// xyz=クリップマップ0中心, w=半径 (0=無効)
		XMFLOAT4 PassGlobalSDF1;
		XMFLOAT4 PassProbeParams0;				// x=プローブ数X, y=プローブ数Y, z=ダウンサンプル, w=octa解像度
		XMFLOAT4 PassProbeParams1;				// x=画面幅, y=画面高, z=テンポラルα, w=履歴有効
		XMFLOAT4 PassCameraOrigin;				// xyz=カメラ, w=ディテールトレース距離
		XMFLOAT4 PassPrevCameraOrigin;			// xyz=前カメラ, w=スクリーントレース厚み
		XMFLOAT4 PassRCParams0;					// xyz=RC最小コーナー, w=間隔
		XMFLOAT4 PassRCParams1;					// x=プローブ数/軸, y=更新開始, z=更新数, w=スカイミップ
		XMFLOAT4 PassReflectionParams;			// x=最大ラフネス, y=フェード開始, z=強度, w=予約

		XMFLOAT4X4 PassViewProjection;			// 転置済み
		XMFLOAT4X4 PassInvViewProjection;		// 転置済み
		XMFLOAT4X4 PassPrevViewProjection;		// 転置済み
	};
	static_assert(sizeof(FLumenPassParams) == 416,
		"FLumenPassParams must mirror HLSL cbuffer LumenPassParams (b0)");

	// ---- カードのローカル空間定義 (キャプチャ / 行列構築用 CPU データ) ----
	struct FLumenCardLocal
	{
		XMFLOAT4X4 LocalViewMatrix;		// ローカル空間カードビュー (転置前)
		XMFLOAT4X4 ProjectionMatrix;	// オルソ射影 (転置前)
		XMFLOAT4X4 CardToLocal;			// カード空間 -> ローカル (= LocalViewMatrix の逆)
		XMFLOAT3   Extent;				// カード半幅 (x,y=平面, z=半深度)
	};

	// ---- オブジェクトスロット (プロキシ 1 つ分の常駐データ) ----
	// (コンポーネント, プロキシ) のペアをキーに永続割当する。
	struct FLumenObjectSlot
	{
		const class UPrimitiveComponent* Component = nullptr;	// キー 1
		const FPrimitiveSceneProxy* Proxy = nullptr;	// キー 2。null = 空きスロット
		FLumenCardLocal Cards[LUMEN_CARDS_PER_OBJECT];
		bool bCaptured[LUMEN_CARDS_PER_OBJECT] = {};	// キャプチャ済みか
	};

	// ---- キャプチャ要求 (フレーム予算制のキュー) ----
	struct FCaptureRequest
	{
		unsigned int SlotIndex = 0;
		unsigned int CardIndex = 0;		// スロット内のカード番号 (0..5)
	};

	// ---- コンピュート用テクスチャ (UAV + SRV, 2D / 3D 共用) ----
	struct FLumenComputeTexture
	{
		ComPtr<ID3D12Resource> Resource;
		unsigned int SRVIndex = 0;
		unsigned int UAVIndex = 0;
		D3D12_GPU_DESCRIPTOR_HANDLE SRVHandle{};	// ImGui プレビュー用
		bool bInReadState = false;					// true = (PIXEL|NON_PIXEL) SRV 状態
	};

	// ---- スクリーンプローブ SH セット (テンポラルのピンポン) ----
	struct FLumenProbeSHSet
	{
		FLumenComputeTexture SHR;
		FLumenComputeTexture SHG;
		FLumenComputeTexture SHB;
		FLumenComputeTexture Aux;	// x=スカイ可視率, y=カメラ距離 (リプロジェクション検証)
	};

	RenderManager* m_RHI = nullptr;
	Params m_Params;
	Stats  m_Stats;

	// ---- オブジェクトスロット + キャプチャキュー ----
	FLumenObjectSlot m_Slots[MAX_LUMEN_OBJECTS];
	std::deque<FCaptureRequest> m_CaptureQueue;

	// ---- GPU バッファ CPU ミラー (毎フレーム詰め直し) ----
	FLumenSceneObjectData m_ObjectData[MAX_LUMEN_OBJECTS];
	FLumenCardGPUData     m_CardData[MAX_LUMEN_CARDS];
	unsigned int          m_NumObjects = 0;

	// ---- オブジェクト / カードバッファ (StructuredBuffer, t24/t25) ----
	ComPtr<ID3D12Resource> m_ObjectBuffer[2];
	FLumenSceneObjectData* m_ObjectBufferPointer[2] = {};
	unsigned int           m_ObjectBufferSRVIndex[2] = {};
	ComPtr<ID3D12Resource> m_CardBuffer[2];
	FLumenCardGPUData* m_CardBufferPointer[2] = {};
	unsigned int           m_CardBufferSRVIndex[2] = {};
	unsigned int           m_BufferFrame = 0;

	// ---- キャプチャアトラス (グラフィックスパスの MRT) ----
	// 常在状態は (PIXEL | NON_PIXEL) の読み取り。キャプチャパスの間だけ
	// RENDER_TARGET / DEPTH_WRITE へ遷移して戻す (RenderCardCaptures)。
	std::unique_ptr<RENDER_TARGET> m_AlbedoAtlas;	// RGBA8 (a=有効マーカー)
	std::unique_ptr<RENDER_TARGET> m_NormalAtlas;	// RGBA8 (カード空間法線)
	std::unique_ptr<RENDER_TARGET> m_EmissiveAtlas;	// R11G11B10F (エミッシブラディアンス)

	// ---- 深度アトラス (R32_TYPELESS: DSV D32 / SRV R32) ----
	ComPtr<ID3D12Resource>       m_DepthAtlas;
	ComPtr<ID3D12DescriptorHeap> m_DepthAtlasDSVHeap;
	D3D12_CPU_DESCRIPTOR_HANDLE  m_DepthAtlasDSV{};
	unsigned int                 m_DepthAtlasSRVIndex = 0;

	// ---- ライティングアトラス (RGBA16F, UAV <-> SRV) ----
	FLumenComputeTexture m_DirectLightingAtlas;
	FLumenComputeTexture m_IndirectLightingAtlas;
	FLumenComputeTexture m_FinalLightingAtlas;

	// ---- Global Distance Field (128^3 R16F クリップマップ x2) ----
	FLumenComputeTexture m_GlobalSDF[LUMEN_GLOBAL_SDF_CLIPMAPS];
	XMFLOAT4 m_GlobalSDFParams[LUMEN_GLOBAL_SDF_CLIPMAPS] = {};	// xyz=中心, w=半径

	// ---- Screen Probe Gather ----
	unsigned int m_NumProbesX = 0;
	unsigned int m_NumProbesY = 0;
	FLumenComputeTexture m_ProbeGeo;			// xyz=法線, w=ビュー距離 (0=無効)
	FLumenComputeTexture m_ProbeTraceRadiance;	// (PW*8) x (PH*8)
	FLumenComputeTexture m_ProbeFilteredRadiance;
	FLumenProbeSHSet     m_ProbeSH[2];			// テンポラルのピンポン
	unsigned int         m_ProbeSHFrame = 0;
	FLumenComputeTexture m_DiffuseIndirect;		// フル解像度 (t28)

	// ---- Reflections ----
	FLumenComputeTexture m_ReflectionTexture;	// フル解像度 (t29)

	// ---- Radiance Cache ----
	FLumenComputeTexture m_RCAtlas;				// 512x512 octahedral
	FLumenComputeTexture m_RCSH[3];				// 16^3 SH ボリューム R/G/B (t30-t32)
	unsigned int m_RCCursor = 0;				// 更新ラウンドロビン
	XMFLOAT4 m_RCVolumeParams0 = { 0.0f, 0.0f, 0.0f, 1.0f };	// xyz=最小コーナー, w=間隔

	// ---- HWRT (DXR TLAS) ----
	std::unique_ptr<FLumenHardwareRayTracing> m_HardwareRayTracing;
	bool m_bHWRTActiveThisFrame = false;

	// ---- コンピュート (独立ルートシグネチャ + PSO 群) ----
	ComPtr<ID3D12RootSignature> m_ComputeRootSignature;
	ComPtr<ID3D12PipelineState> m_PSODirectLighting;
	ComPtr<ID3D12PipelineState> m_PSORadiosity;
	ComPtr<ID3D12PipelineState> m_PSOCombine;
	ComPtr<ID3D12PipelineState> m_PSOGlobalSDF;
	ComPtr<ID3D12PipelineState> m_PSOProbeSetup;
	ComPtr<ID3D12PipelineState> m_PSOProbeTrace;
	ComPtr<ID3D12PipelineState> m_PSOProbeFilter;
	ComPtr<ID3D12PipelineState> m_PSOProbeSH;
	ComPtr<ID3D12PipelineState> m_PSOProbeIntegrate;
	ComPtr<ID3D12PipelineState> m_PSOReflections;
	ComPtr<ID3D12PipelineState> m_PSORCTrace;
	ComPtr<ID3D12PipelineState> m_PSORCSH;

	// HWRT (RayQuery, SM 6.5) バリアント。cso 不在 / 生成失敗時は null
	// のまま SWRT へフォールバックする。
	ComPtr<ID3D12PipelineState> m_PSODirectLightingRT;
	ComPtr<ID3D12PipelineState> m_PSORadiosityRT;
	ComPtr<ID3D12PipelineState> m_PSOProbeTraceRT;
	ComPtr<ID3D12PipelineState> m_PSOReflectionsRT;
	ComPtr<ID3D12PipelineState> m_PSORCTraceRT;

	// b0 アップロードバッファ (16 スロット x 512B x 2 フレーム)
	static const unsigned int PASS_PARAM_SLOTS = 16;
	static const unsigned int PASS_PARAM_STRIDE = 512;
	ComPtr<ID3D12Resource> m_PassParamBuffer[2];
	unsigned char* m_PassParamPointer[2] = {};

	// ---- Radiosity ラウンドロビン ----
	unsigned int m_RadiosityCardCursor = 0;
	unsigned int m_FrameNumber = 0;

	// ---- 内部ヘルパー ----
	ID3D12Device* Device();
	ID3D12GraphicsCommandList* CommandList();

	void InitAtlases();
	void InitScreenTextures();	// GDF / プローブ / 反射 / RC
	void InitBuffers();
	void InitComputePipelines();
	ComPtr<ID3D12PipelineState> CreateComputePipeline(const char* csoFile);
	// 失敗を許容するローダ (HWRT バリアント用。不在 / 失敗で null)
	ComPtr<ID3D12PipelineState> TryCreateComputePipeline(const char* csoFile);

	// コンピュートテクスチャ生成 (2D: Depth=1 / 3D: Depth>1)
	void CreateComputeTexture(FLumenComputeTexture& Texture, const wchar_t* Name,
		unsigned int Width, unsigned int Height, unsigned int Depth,
		DXGI_FORMAT Format, bool bStartInReadState);

	// メッシュのローカル境界から 6 カードを構築する (MeshCards 生成)
	void BuildMeshCards(const FPrimitiveSceneProxy* Proxy, FLumenObjectSlot& Slot) const;

	// スロット割当 / 解放 (プロキシ再生成で自動的に再割当される)
	int  AllocateSlot(const class UPrimitiveComponent* Component,
		const FPrimitiveSceneProxy* Proxy);
	void FreeSlot(unsigned int SlotIndex);

	// カードスロット -> アトラスタイルの UV スケール / バイアス
	static XMFLOAT4 GetAtlasUVScaleBias(unsigned int GlobalCardIndex);

	// パスパラメータを書き込み、その GPU アドレスを返す
	D3D12_GPU_VIRTUAL_ADDRESS WritePassParams(unsigned int SlotIndex,
		const FLumenPassParams& Params);

	// 共通パスパラメータの解決 (フレーム入力 + Lumen 状態)
	FLumenPassParams MakeBasePassParams(const FLumenFrameInputs& Inputs) const;

	// コンピュートテクスチャの状態遷移 (READ = PIXEL|NON_PIXEL)
	void TransitionComputeTexture(FLumenComputeTexture& Texture, bool bToRead);

	// 共通 SRV/UAV テーブルのバインド (t0..t14 / u0..u3) + TLAS
	void BindCommonComputeState(const FLumenFrameInputs& Inputs);

	// TLAS 構築 (HWRT。UpdateLumenScene 内から呼ばれる)
	void UpdateTLAS();

	// Global Distance Field クリップマップ再構築
	void UpdateGlobalDistanceField(const FLumenFrameInputs& Inputs);

	// Radiance Cache 更新 (トレース + SH 化)
	void UpdateRadianceCache(const FLumenFrameInputs& Inputs);

	// Radiosity をカード範囲 [Start, Start+Count) に対して記録する
	void DispatchRadiosityRange(unsigned int StartCard, unsigned int NumCards,
		unsigned int ParamSlot, const FLumenPassParams& BaseParams);

	// HWRT が有効ならそのフレームのトレース PSO を返す
	ID3D12PipelineState* SelectTracePSO(
		ID3D12PipelineState* SWRT, ID3D12PipelineState* HWRT) const;

public:
	explicit FLumenSceneData(RenderManager* RHI);
	~FLumenSceneData();

	void Init();

	// 毎フレーム: FScene のプロキシ列からオブジェクト / カードバッファを
	// 詰め直し、新規プロキシのキャプチャを予約する。HWRT 有効時は
	// TLAS も再構築する (BeginUpdateLumenSceneTasks 相当)。
	void UpdateLumenScene(FScene* Scene);

	// カードキャプチャパス (フレーム予算制)。b0/b1 を上書きするため
	// シャドウ深度パスの後 / RenderLighting の前に呼ぶこと。
	void RenderCardCaptures();

	// Surface Cache ライティング + Global SDF + Radiance Cache:
	//   GDF 再構築 -> Direct -> Radiosity -> Combine -> RC 更新
	void RenderLumenSceneLighting(const FLumenFrameInputs& Inputs);

	// スクリーン GI (Screen Probe Gather + Reflections)。
	// RenderLighting 内の LinearDepth パス後 / デファードドロー前に
	// 呼ぶこと (G-Buffer / 深度 / LinearDepth が読み取り状態で、
	// Surface Cache の FinalLighting が確定済みであること)。
	void RenderLumenScreenGI(const FLumenFrameInputs& Inputs);

	// デファードライティング直前に呼ぶ: t24-t32 をバインド
	void BindLumenResources();

	// トランスルーセンシーパス直前に呼ぶ: t30-t32 (Radiance Cache)
	void BindTranslucencyResources();

	// b6 (LUMEN_CONSTANT) をパラメータから解決する
	void FillLumenConstant(LUMEN_CONSTANT& Out) const;

	// ---- アクセサ ----
	Params& GetParams() { return m_Params; }
	const Params& GetParams() const { return m_Params; }
	const Stats& GetStats() const { return m_Stats; }

	bool IsEnabled() const { return m_Params.bEnabled; }
	unsigned int GetNumObjects() const { return m_NumObjects; }
	bool IsHardwareRayTracingSupported() const;

	// ---- ImGui プレビュー用 SRV ハンドル ----
	D3D12_GPU_DESCRIPTOR_HANDLE GetAlbedoAtlasSRVHandle() const { return m_AlbedoAtlas ? m_AlbedoAtlas->SRVHandle : D3D12_GPU_DESCRIPTOR_HANDLE{}; }
	D3D12_GPU_DESCRIPTOR_HANDLE GetNormalAtlasSRVHandle() const { return m_NormalAtlas ? m_NormalAtlas->SRVHandle : D3D12_GPU_DESCRIPTOR_HANDLE{}; }
	D3D12_GPU_DESCRIPTOR_HANDLE GetEmissiveAtlasSRVHandle() const { return m_EmissiveAtlas ? m_EmissiveAtlas->SRVHandle : D3D12_GPU_DESCRIPTOR_HANDLE{}; }
	D3D12_GPU_DESCRIPTOR_HANDLE GetFinalLightingSRVHandle() const { return m_FinalLightingAtlas.SRVHandle; }
	D3D12_GPU_DESCRIPTOR_HANDLE GetProbeRadianceSRVHandle() const { return m_ProbeFilteredRadiance.SRVHandle; }
	D3D12_GPU_DESCRIPTOR_HANDLE GetDiffuseIndirectSRVHandle() const { return m_DiffuseIndirect.SRVHandle; }
	D3D12_GPU_DESCRIPTOR_HANDLE GetReflectionSRVHandle() const { return m_ReflectionTexture.SRVHandle; }
};
