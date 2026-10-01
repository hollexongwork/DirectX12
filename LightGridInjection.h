#pragma once
#include "RenderManager.h"
#include "LightSceneProxy.h"

// ============================================================
//  FLightGridInjection
//  FSceneRenderer::GatherLightsAndComputeLightGrid /
//  LightGridInjection.cpp (FLightGridInjectionCS + FLightGridCompactCS)
//  に相当するタイルドライトカリング (クラスタードライトグリッド)。
//
//  画面を XY = LIGHT_GRID_PIXEL_SIZE (64px) タイル、
//  Z = LIGHT_GRID_SIZE_Z (32) の指数スライスに分割した 3D セル
//  (froxel) ごとに、影響するローカルライトのインデックス列を
//  毎フレーム 2 つのコンピュートパスで構築する:
//
//    Pass 1 (LightGridInjection_CS):
//        セルごとに全ライトをビュー空間 AABB / コーン / 半空間で
//        判定し、グローバルリンクリスト
//        (StartOffsetGrid + CulledLightLinks) へ積む。
//    Pass 2 (LightGridCompact_CS):
//        リンクリストを連続領域へ圧縮し、
//        NumCulledLightsGrid (t19) + CulledLightDataGrid (t20)
//        として確定する。
//
//  受光側 (DeferredPS) は b3 (ForwardLightData) のグリッド
//  パラメータと LightGridCommon.hlsl のヘルパで自ピクセルの
//  セルを引き、そのセルのライトだけを評価する。グリッドが持つ
//  インデックスはライトバッファ (t13) / ローカルシャドウ
//  パラメータ (t16) と 1:1 のまま。
//
//  依存モデルは AutoExposure と同じ:
//    - 独立コンピュートルートシグネチャ
//    - RenderManager の公開アクセサ経由 (device / command list /
//      デスクリプタ確保)
//    - フレーム内 Dispatch (コマンドリストは flush しない)
// ============================================================

// ---- グリッド定数 (r.Forward.* の既定値と同一) ----
static const unsigned int LIGHT_GRID_PIXEL_SIZE = 64; // r.Forward.LightGridPixelSize
static const unsigned int LIGHT_GRID_PIXEL_SIZE_SHIFT = 6; // log2(LIGHT_GRID_PIXEL_SIZE)
static const unsigned int LIGHT_GRID_SIZE_Z = 32; // r.Forward.LightGridSizeZ
static const unsigned int MAX_CULLED_LIGHTS_PER_CELL = 32; // r.Forward.MaxCulledLightsPerCell

class FLightGridInjection
{
public:
	// ---- デバッグ / 制御 (ImGui: Light Grid ウィンドウから操作) ----
	struct Params
	{
		bool         bUseLightGrid = true; // false = 全灯ループへフォールバック
		unsigned int DebugMode = 0; // 0=off 1=複雑度ヒートマップ 2=Zスライス
	};

private:
	// HLSL 側 (LightGridInjectionCommon.hlsl。Pass 1/2 共通) の
	// cbuffer FLightGridParams (b0) と 1:1 ミラー必須。
	struct FLightGridParams
	{
		XMFLOAT4X4   ViewMatrix; // ワールド -> ビュー (転置済み)

		unsigned int CulledGridSizeX;
		unsigned int CulledGridSizeY;
		unsigned int CulledGridSizeZ;
		unsigned int NumLocalLights;

		XMFLOAT3     LightGridZParams; // (B, O, S)
		unsigned int LightGridPixelSize;

		float        InvProjScaleX; // 1 / Projection._11
		float        InvProjScaleY; // 1 / Projection._22
		float        ScreenWidth;
		float        ScreenHeight;

		unsigned int MaxCulledLightsPerCell;
		unsigned int MaxCulledLightLinks;
		unsigned int CulledLightDataCapacity;
		float        NearPlane;
	};
	static_assert(sizeof(FLightGridParams) == 128,
		"FLightGridParams must mirror HLSL cbuffer FLightGridParams (b0)");

	RenderManager* m_Owner = nullptr;
	Params         m_Params;

	// ---- 容量 (Init で確定。全バッファはこのセル数で一度だけ確保する) ----
	// レンダー解像度 R はスクリーンパーセンテージで変わるが、容量 (2 x 出力解像度) で
	// 確保しておけば SetViewSize で次元を変えるだけで済み、再確保の経路が要らない (§5.3)
	unsigned int m_CapacityGridX = 1;
	unsigned int m_CapacityGridY = 1;
	unsigned int m_CapacityCells = 0;

	// ---- 今フレームのグリッド次元 (SetViewSize で毎フレーム確定) ----
	// シェーダはセルを b0 / b3 の CulledGridSizeX/Y で引くので、容量確保のバッファの
	// 先頭 m_NumCells 分だけを使う
	unsigned int m_ViewWidth = 1;
	unsigned int m_ViewHeight = 1;
	unsigned int m_GridSizeX = 1;
	unsigned int m_GridSizeY = 1;
	unsigned int m_GridSizeZ = LIGHT_GRID_SIZE_Z;
	unsigned int m_NumCells = 0;

	// 独立コンピュートルートシグネチャ (両パス共通):
	//  [0] CBV  b0 (FLightGridParams)
	//  [1] SRV table t0 (ForwardLocalLights)
	//  [2] UAV table u0 (StartOffsetGrid)
	//  [3] UAV table u1 (CulledLightLinks)
	//  [4] UAV table u2 (Allocator)
	//  [5] UAV table u3 (NumCulledLightsGrid)
	//  [6] UAV table u4 (CulledLightDataGrid)
	ComPtr<ID3D12RootSignature> m_RootSignature;
	ComPtr<ID3D12PipelineState> m_PSOInjection;
	ComPtr<ID3D12PipelineState> m_PSOCompact;

	// ---- 中間バッファ (常時 UNORDERED_ACCESS のまま) ----
	ComPtr<ID3D12Resource> m_StartOffsetGrid; // NumCells * uint
	unsigned int           m_StartOffsetUAVIndex = 0;
	ComPtr<ID3D12Resource> m_CulledLightLinks; // NumCells * MaxPerCell * uint2
	unsigned int           m_LinksUAVIndex = 0;
	ComPtr<ID3D12Resource> m_Allocator; // 2 * uint ([0]=NextLink, [1]=NextData)
	unsigned int           m_AllocatorUAVIndex = 0;

	// ---- 出力バッファ (UAV <-> PIXEL_SHADER_RESOURCE を毎フレーム往復) ----
	ComPtr<ID3D12Resource> m_NumCulledLightsGrid; // NumCells * 2 * uint
	unsigned int           m_NumCulledUAVIndex = 0;
	unsigned int           m_NumCulledSRVIndex = 0;
	ComPtr<ID3D12Resource> m_CulledLightDataGrid; // NumCells * MaxPerCell * uint
	unsigned int           m_DataGridUAVIndex = 0;
	unsigned int           m_DataGridSRVIndex = 0;

	// アロケータのゼロクリア用 CPU 専用ヒープ
	// (ClearUnorderedAccessViewUint は shader-visible と CPU-only の
	//  両ハンドルが必要 -- AutoExposure と同じパターン)
	ComPtr<ID3D12DescriptorHeap> m_ClearHeap;

	// b0 アップロードバッファ。2 フレームインフライト (Present の
	// 待ち方) に合わせ、ライトバッファと同じ理由でダブルバッファ化。
	ComPtr<ID3D12Resource> m_ParamBuffer[2];
	void*                  m_ParamPtr[2] = {};
	unsigned int           m_ParamFrame = 0;

	// 出力バッファが SRV ステートに居るか (フレーム先頭で UAV へ戻す)
	bool m_bOutputsInSRVState = false;

	ID3D12Device* Device();
	ID3D12GraphicsCommandList* CommandList();
	ComPtr<ID3D12PipelineState> CreateComputePipeline(const char* csoFile);

public:
	explicit FLightGridInjection(RenderManager* owner);
	~FLightGridInjection();

	// 容量 (CapacityWidth x CapacityHeight のスクリーン) / RS / PSO / バッファ / UAV / SRV。
	// FSceneRenderer は (2 x 出力解像度) で呼ぶ (スクリーンパーセンテージ上限 200 %)。
	// 次元は SetViewSize で別途設定すること
	void Init(unsigned int CapacityWidth, unsigned int CapacityHeight);

	// 今フレームのレンダー解像度 (R) からグリッド次元 / ScreenWidth/Height /
	// MaxCulledLightLinks を決める。毎フレーム BeginFrame 先頭 (FillForwardLightData /
	// Dispatch より前) に呼ぶ。容量を超える場合は容量へ丸める (assert)
	void SetViewSize(unsigned int Width, unsigned int Height);

	// b3 (FORWARD_LIGHT_CONSTANT) のグリッドフィールドを埋める。
	// NumLocalLights は呼び出し側 (SetupLightConstants) が設定する。
	// Z スライスパラメータ (GetLightGridZParams 相当) もここで解決。
	void FillForwardLightData(FORWARD_LIGHT_CONSTANT& Out, float NearPlane, float FarPlane) const;

	// 今フレームのライトグリッド構築 (Injection -> Compact) を記録する。
	//  ViewConstant        : カメラの VIEW 定数 (View/Projection/NearFar)
	//  lightBufferSRVIndex : 今フレームのライトバッファ SRV (t13 と同じもの)
	//  numLocalLights      : 有効ローカルライト数
	void Dispatch(const VIEW_CONSTANT& ViewConstant,
	              unsigned int lightBufferSRVIndex,
	              unsigned int numLocalLights);

	// デファードライティングにバインドする SRV (t19 / t20)
	unsigned int GetNumCulledLightsGridSRVIndex() const { return m_NumCulledSRVIndex; }
	unsigned int GetCulledLightDataGridSRVIndex() const { return m_DataGridSRVIndex; }

	// ---- Volumetric Fog (FVolumetricFog, VolumetricFog.h) 用アクセサ ----
	// ライト散乱コンピュートパスが froxel ごとのセルを引くためのもの。
	// Dispatch 後の出力バッファは PIXEL_SHADER_RESOURCE で常在するので、
	// コンピュートから読む側が NON_PIXEL へ一時遷移して戻すこと
	// (m_bOutputsInSRVState の前提を崩さない)。
	ID3D12Resource* GetNumCulledLightsGridResource() const { return m_NumCulledLightsGrid.Get(); }
	ID3D12Resource* GetCulledLightDataGridResource() const { return m_CulledLightDataGrid.Get(); }
	bool            AreOutputsInSRVState() const { return m_bOutputsInSRVState; }

	// ---- ImGui 用アクセサ ----
	Params&       GetParams()       { return m_Params; }
	const Params& GetParams() const { return m_Params; }

	unsigned int GetGridSizeX() const { return m_GridSizeX; }
	unsigned int GetGridSizeY() const { return m_GridSizeY; }
	unsigned int GetGridSizeZ() const { return m_GridSizeZ; }
	unsigned int GetNumCells()  const { return m_NumCells; }
};
