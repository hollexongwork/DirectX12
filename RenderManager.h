#pragma once
#include "PostProcessSettings.h"

// ============================================================
//  RenderManager
//  FD3D12DynamicRHI / FRHICommandList に相当する RHI 層。
//  Phase 2 でフレームオーケストレーション (パス列 / G-Buffer /
//  ポストプロセス) を FSceneRenderer / FSceneTextures に分離し、
//  以下の低レベル責務のみを持つ:
//    - デバイス / スワップチェーン / コマンドキュー / フェンス
//    - デスクリプタヒープ (SRV/RTV) + フリーリスト
//    - リング定数バッファ
//    - ルートシグネチャ + PSO キャッシュ
//    - リソース生成 (RT / テクスチャ / VB / IB) とバインド API
//  ※ CONSTANT_TYPE / TEXTURE_TYPE の enum 値は HLSL レジスタと
//    1:1 対応 (b0..b5 / t0..t18)。順序変更・挿入は禁止。
//    新規リソースは COUNT の直前に追加すること。
// ============================================================


struct VERTEX_3D
{
	XMFLOAT3 Position;
	XMFLOAT3 Normal;
	XMFLOAT3 Tangent;
	XMFLOAT2 TexCoord;
	XMFLOAT4 Color;
};




// ============================================================
//  定数バッファ構造体
//  3 系統に分離:
//    VIEW_CONSTANT          = FViewUniformShaderParameters 相当 (b0)
//    FORWARD_LIGHT_CONSTANT = FForwardLightData 相当 (b3)
//    PP_SETTINGS            = パス毎パラメータ (b4, PostProcessSettings.h)
//    DIRECTIONAL_SHADOW_CONSTANT = CSM シャドウ定数 (b5, ShadowRendering.h)
//  ※ HLSL 側 (ConstantBuffers.hlsl) と 1:1 ミラー必須
// ============================================================

// b0 : View。ビュー行列群 + カメラ + 代表ディレクショナルライト。
// 太陽ライトは View ユニフォームに常駐する。
struct VIEW_CONSTANT
{
	XMFLOAT4X4		View;
	XMFLOAT4X4		Projection;
	XMFLOAT4X4		InvViewProjection;
	XMFLOAT4		WorldCameraOrigin;	// xyz = カメラワールド位置 [m]
	XMFLOAT4		NearFar;			// x=Near, y=Far

	// ディレクショナルライト (FScene のライトリストから毎フレーム解決)
	//   DirectionalLightDirection.xyz = 受光面からライトへ向かう方向 (発光方向の逆)
	//   DirectionalLightColor.rgb     = 線形色 x 強度 (lux)
	// ライト不在時は DirectionalLightColor = 0 (無光)
	XMFLOAT4		DirectionalLightDirection;
	XMFLOAT4		DirectionalLightColor;
};
static_assert(sizeof(VIEW_CONSTANT) == 256, "VIEW_CONSTANT must mirror HLSL ViewConstantBuffer (b0)");


// b1 : Primitive (FPrimitiveUniformShaderParameters 相当)。per-draw。
struct PRIMITIVE_CONSTANT
{
	XMFLOAT4X4 LocalToWorld;
};
static_assert(sizeof(PRIMITIVE_CONSTANT) == 64, "PRIMITIVE_CONSTANT must mirror HLSL PrimitiveConstantBuffer (b1)");


// b3 : ForwardLightData (FForwardLightData 相当)。
// ローカルライト (Point/Spot/Rect) の有効数 + タイルドライトカリング
// (ライトグリッド) のパラメータ。ライト本体は
// StructuredBuffer<FLightShaderParameters> (t13, ForwardLocalLights)、
// グリッド本体は NumCulledLightsGrid (t19) + CulledLightDataGrid (t20)。
// グリッドフィールドは FLightGridInjection::FillForwardLightData が
// 毎フレーム解決する (LightGridInjection.h)。
struct FORWARD_LIGHT_CONSTANT
{
	unsigned int	NumLocalLights = 0;			// ローカルライト有効数
	unsigned int	NumGridCells = 0;			// グリッド総セル数 (X*Y*Z)
	unsigned int	CulledGridSizeX = 1;		// 画面タイル数 X (= ceil(W / LightGridPixelSize))
	unsigned int	CulledGridSizeY = 1;		// 画面タイル数 Y

	unsigned int	CulledGridSizeZ = 1;		// Z スライス数 (LIGHT_GRID_SIZE_Z)
	unsigned int	LightGridPixelSizeShift = 6;// log2(LightGridPixelSize)
	unsigned int	MaxCulledLightsPerCell = 32;// セルあたり保持するライト数上限
	unsigned int	LightGridDebugMode = 0;		// 0=off 1=複雑度ヒートマップ 2=Zスライス

	XMFLOAT3		LightGridZParams = { 1.0f, 0.0f, 1.0f };	// (B, O, S): Slice = log2(Depth*B + O) * S
	unsigned int	bUseLightGrid = 0;			// 0 = 全灯ループ (フォールバック)
};
static_assert(sizeof(FORWARD_LIGHT_CONSTANT) == 48, "FORWARD_LIGHT_CONSTANT must mirror HLSL ForwardLightData (b3)");




struct TEXTURE
{
	ComPtr<ID3D12Resource>	Resource;
	unsigned int			SRVIndex;
	~TEXTURE();
};


struct CONSTANT_BUFFER
{
	ComPtr<ID3D12Resource>	Resource;
	unsigned int			SRVIndex;
	~CONSTANT_BUFFER();
};


struct RENDER_TARGET
{
	ComPtr<ID3D12Resource>	Resource;
	unsigned int			SRVIndex;
	unsigned int			RTVIndex;
	D3D12_GPU_DESCRIPTOR_HANDLE SRVHandle;
	D3D12_CPU_DESCRIPTOR_HANDLE RTVHandle;
	~RENDER_TARGET();
};



struct VERTEX_BUFFER
{
	ComPtr<ID3D12Resource>		Resource;
	unsigned int				Stride;
	unsigned int				Size;
};

struct INDEX_BUFFER
{
	ComPtr<ID3D12Resource>		Resource;
	unsigned int				Size;
};


// ============================================================
//  PSO ステートプリセット
//  TStaticBlendState / TStaticRasterizerState /
//  TStaticDepthStencilState の代表形。CreatePipeline に渡して
//  マテリアルの Blend Mode / Two Sided に対応する PSO 列を作る。
// ============================================================

// EBlendMode -> ブレンドステート (SetBlendModeBlendState 相当)
enum class EBlendStatePreset
{
	Opaque,			// One / Zero 上書き (BLEND_Opaque / BLEND_Masked)
	Translucent,	// SrcAlpha / InvSrcAlpha (BLEND_Translucent)
	Additive,		// SrcAlpha / One (BLEND_Additive)
	NoColorWrite,	// カラー書き込み無効 (半透明深度プリパス用)
};

// bTwoSided -> ラスタライザカリング
enum class ECullModePreset
{
	Back,	// 背面カリング (既定)
	None,	// Two Sided (カリング無効)
};

// 深度書き込み (トランスルーセンシーはテストのみ)
enum class EDepthStatePreset
{
	DepthWrite,		// 深度テスト + 書き込み (既定)
	DepthRead,		// 深度テストのみ (Translucent / Additive)
	DepthReadEqual,	// 深度テストのみ + EQUAL 比較
					// (半透明深度プリパスの着色パス: プリパスが書いた
					//  最前面深度に一致するフラグメントだけ着色する)
};



class RenderManager
{

private:
	// ------------------------------------------------------------
	//  Init helpers (called from the constructor)
	// ------------------------------------------------------------
	void Init();
	void InitViewport();
	void InitDevice();
	void InitCommandObjects();
	void InitSwapChain();
	void InitBackBufferRTV();
	void InitDepthBuffer();
	void InitDescriptorHeaps();
	void InitImGui();
	void InitConstantBuffers();
	void InitRootSignature();
	void InitPipelines();

	// ------------------------------------------------------------
	//  Descriptor helpers
	// ------------------------------------------------------------
	unsigned int AllocateSRVSlot();
	unsigned int AllocateRTVSlot();

	D3D12_CPU_DESCRIPTOR_HANDLE OffsetCPUHandle(D3D12_CPU_DESCRIPTOR_HANDLE base, unsigned int index, D3D12_DESCRIPTOR_HEAP_TYPE type) const;
	D3D12_GPU_DESCRIPTOR_HANDLE OffsetGPUHandle(D3D12_GPU_DESCRIPTOR_HANDLE base, unsigned int index, D3D12_DESCRIPTOR_HEAP_TYPE type) const;

	unsigned int                CreateShaderResourceView(ID3D12Resource* Resource);
	D3D12_GPU_DESCRIPTOR_HANDLE GetShaderResourceViewHandle(unsigned int SRVIndex);
	unsigned int                CreateRenderTargetView(ID3D12Resource* Resource, unsigned int MipLevel = 0);
	D3D12_CPU_DESCRIPTOR_HANDLE GetRenderTargetViewHandle(unsigned int RTVIndex);

	// DepthBias / SlopeScaledDepthBias はシャドウ深度 PSO 用 (既定値は従来通り 0)
	// Blend / Cull / Depth プリセットでマテリアルの Blend Mode / Two Sided
	// に対応する PSO バリアントを生成する (既定は従来の不透明上書き)
	ComPtr<ID3D12PipelineState> CreatePipeline(const char* VertexShaderFile, const char* PixelShaderFile, const DXGI_FORMAT* RTVFormats, unsigned int NumRenderTargets, int DepthBias = 0, float SlopeScaledDepthBias = 0.0f,
		EBlendStatePreset BlendPreset = EBlendStatePreset::Opaque,
		ECullModePreset CullPreset = ECullModePreset::Back,
		EDepthStatePreset DepthPreset = EDepthStatePreset::DepthWrite);


	// ------------------------------------------------------------
	//  Members
	// ------------------------------------------------------------
	static RenderManager* m_Instance;

	HWND m_WindowHandle = nullptr;
	bool m_WindowMode = true;
	int  m_BackBufferWidth = 0;
	int  m_BackBufferHeight = 0;

	// Device / queue / sync
	UINT64 m_Frame[2] = {};
	UINT   m_RTIndex = 0;

	ComPtr<IDXGIFactory4>             m_Factory;
	ComPtr<IDXGIAdapter3>             m_Adapter;
	ComPtr<ID3D12Device>              m_Device;
	ComPtr<ID3D12CommandQueue>        m_CommandQueue;
	ComPtr<ID3D12Fence>               m_Fence;
	ComPtr<IDXGISwapChain3>           m_SwapChain;
	ComPtr<ID3D12GraphicsCommandList> m_GraphicsCommandList;
	ComPtr<ID3D12CommandAllocator>    m_GraphicsCommandAllocator[2];
	HANDLE                            m_FenceEvent = nullptr;

	// Back buffers
	ComPtr<ID3D12Resource>       m_RenderTarget[2];
	ComPtr<ID3D12DescriptorHeap> m_RenderTargetDescriptorHeap;
	D3D12_CPU_DESCRIPTOR_HANDLE  m_RenderTargetHandle[2]{};

	// Depth buffer (DSV は RHI 所有。SRV は FSceneTextures 側で生成)
	ComPtr<ID3D12Resource>       m_DepthBuffer;
	ComPtr<ID3D12DescriptorHeap> m_DepthBufferDescriptorHeap;
	D3D12_CPU_DESCRIPTOR_HANDLE  m_DepthBufferHandle{};

	D3D12_RECT     m_ScissorRect{};
	D3D12_VIEWPORT m_Viewport{};

	// Descriptor heaps + free-lists
	ComPtr<ID3D12DescriptorHeap> m_SRVDescriptorHeap;
	std::list<unsigned int>      m_SRVDescriptorPool;
	static const unsigned int    SRV_DESCRIPTOR_MAX = 10000;

	ComPtr<ID3D12DescriptorHeap> m_RTVDescriptorHeap;
	std::list<unsigned int>      m_RTVDescriptorPool;
	static const unsigned int    RTV_DESCRIPTOR_MAX = 1000;

	// Ring constant buffer (one per frame-in-flight)
	static const unsigned int CONSTANT_BUFFER_SIZE = 512;
	// シャドウ深度パス (シャドウビュー数 x プリミティブ数) の分も
	// リングから消費するため余裕を確保 (旧 1000)
	static const unsigned int CONSTANT_BUFFER_MAX = 3000;
	ComPtr<ID3D12Resource>    m_ConstantBuffer[2];
	byte* m_ConstantBufferPointer[2] = {};
	unsigned int              m_ConstantBufferView[2][CONSTANT_BUFFER_MAX] = {};
	unsigned int              m_ConstantBufferIndex[2] = {};

	// Root signature + PSOs
	ComPtr<ID3D12RootSignature> m_RootSignature;
	std::unordered_map<std::string, ComPtr<ID3D12PipelineState>> m_PipelineState;


public:
	// ---- Constant-buffer / texture slots (root-parameter order) ----
	// HLSL レジスタと 1:1 (ConstantBuffers.hlsl / Resources.hlsl)。
	enum class CONSTANT_TYPE
	{
		VIEW,			// b0  FViewUniformShaderParameters 相当
		PRIMITIVE,		// b1  FPrimitiveUniformShaderParameters 相当 (per-draw)
		MATERIAL,		// b2  per-material PBR パラメータ
		FORWARD_LIGHT,	// b3  FForwardLightData 相当 (NumLocalLights)
		POST_PROCESS,	// b4  パス毎ポストプロセスパラメータ (PP_SETTINGS)
		SHADOW,			// b5  ディレクショナルシャドウ (CSM) 定数 (DIRECTIONAL_SHADOW_CONSTANT)
	};

	enum class TEXTURE_TYPE
	{
		// ---- G-Buffer / マテリアル共用 (ベースパス=マテリアル, ライティング=G-Buffer) ----
		BASE_COLOR = (int)CONSTANT_TYPE::SHADOW + 1, // t0 GBufferC / SceneColor 入力
		NORMAL,           // t1  GBufferA (World Normal)
		MSRA,             // t2  GBufferB (Metallic/Specular/Roughness/AO) / ARM
		DEPTH,            // t3  非線形深度 SRV
		LINEAR_DEPTH,     // t4  線形深度
		ENVIRONMENT,      // t5  環境マップ (equirect)
		// ---- IBL precomputed ----
		IRRADIANCE,       // t6
		PREFILTER,        // t7
		BRDF_LUT,         // t8
		// ---- PostProcess ----
		BLOOM,            // t9   (bloom result / second bloom input)
		COLOR_GRADING_LUT,// t10  (3D grading LUT)
		AUTO_EXPOSURE,    // t11  (eye-adaptation exposure scale buffer)
		DOF,              // t12  (half-res DOF blur, premultiplied + CoC)
		// ---- Lights ----
		LIGHTS,           // t13  (StructuredBuffer<FLightShaderParameters> ForwardLocalLights)
		// ---- Shadows ----
		DIRECTIONAL_SHADOW, // t14 (Texture2DArray: CSM カスケード深度)
		LOCAL_SHADOW,     // t15  (Texture2DArray: ローカルライトシャドウアトラス)
		LOCAL_SHADOW_DATA,// t16  (StructuredBuffer<FLocalShadowParameters> LocalShadowParams)

		// ---- Distance Field Shadows ----
		DF_ATLAS,         // t17  (Texture3D<float>: メッシュ SDF アトラス, DistanceFieldAtlas.h)
		DF_OBJECTS,       // t18  (StructuredBuffer<FDFObjectData> DFObjects)

		// ---- Light Grid (タイルドライトカリング, LightGridInjection.h) ----
		NUM_CULLED_LIGHTS_GRID, // t19 (StructuredBuffer<uint> NumCulledLightsGrid: セルごとの [ライト数, データ開始])
		CULLED_LIGHT_DATA_GRID, // t20 (StructuredBuffer<uint> CulledLightDataGrid: ライトインデックス列)

		// ---- Refraction (RefractionCommon.hlsl) ----
		SCENE_COLOR_COPY, // t21 (屈折用シーンカラーコピー。半透明パス直前に SceneColor から CopyResource)

		// ---- Substrate (Substrate.hlsl) ----
		SUBSTRATE_MATERIAL0, // t22 (Texture2D<uint4>: Slab パック 0。x = ヘッダ, 0 = 非 Substrate)
		SUBSTRATE_MATERIAL1, // t23 (Texture2D<uint4>: Slab パック 1)

		// ---- Count ----
		COUNT,
	};

	// ------------------------------------------------------------
	//  Lifetime / singleton
	// ------------------------------------------------------------
	RenderManager();
	~RenderManager();

	static RenderManager* GetInstance() { return m_Instance; }

	// ------------------------------------------------------------
	//  Frame (FSceneRenderer から呼ばれる)
	// ------------------------------------------------------------
	void WaitGPU();
	// フレーム先頭: ヒープ / ルートシグネチャ / 定数リング / ビューポート
	void BeginFrame();
	// フレーム末尾: Close -> Execute -> Present -> 前フレーム待ち -> Reset
	void Present();

	// ------------------------------------------------------------
	//  Resource creation
	// ------------------------------------------------------------
	std::unique_ptr<RENDER_TARGET> CreateRenderTarget(unsigned int Width, unsigned int Height, DXGI_FORMAT Format, unsigned int MipLevels = 1);
	std::unique_ptr<TEXTURE>       LoadTexture(const char* FileName, bool sRGB = false);
	//std::unique_ptr<TEXTURE>       CreateSolidColorTexture(UINT8 r, UINT8 g, UINT8 b, UINT8 a);
	std::unique_ptr<VERTEX_BUFFER> CreateVertexBuffer(unsigned int Stride, unsigned int Size);
	std::unique_ptr<INDEX_BUFFER>  CreateIndexBuffer(unsigned int Size);

	// ------------------------------------------------------------
	//  Binding
	// ------------------------------------------------------------
	void SetConstant(CONSTANT_TYPE Type, const void* Constant, unsigned int Size);
	void SetTexture(TEXTURE_TYPE Type, const TEXTURE* Texture);
	void SetTexture(TEXTURE_TYPE Type, const RENDER_TARGET* Texture);
	void SetVertexBuffer(const VERTEX_BUFFER* VertexBuffer);
	void SetIndexBuffer(const INDEX_BUFFER* IndexBuffer);
	void SetPipelineState(const char* PipelineName);

	// Bind a descriptor-table root parameter directly from an SRV-heap index.
	void BindRootTableBySRVIndex(unsigned int RootParameter, unsigned int SRVIndex);

	// ------------------------------------------------------------
	//  Descriptor release (called by resource destructors)
	// ------------------------------------------------------------
	void ReleaseShaderResourceView(unsigned int SRVIndex);
	void ReleaseRenderTargetView(unsigned int RTVIndex);

	// ------------------------------------------------------------
	//  Simple accessors
	// ------------------------------------------------------------
	ID3D12Device* GetDevice() { return m_Device.Get(); }
	ID3D12GraphicsCommandList* GetGraphicsCommandList() { return m_GraphicsCommandList.Get(); }
	int                        GetBackBufferWidth() { return m_BackBufferWidth; }
	int                        GetBackBufferHeight() { return m_BackBufferHeight; }

	// 深度バッファ (DSV は RHI 所有 / SRV は FSceneTextures が生成)
	ID3D12Resource*             GetDepthBufferResource() { return m_DepthBuffer.Get(); }
	D3D12_CPU_DESCRIPTOR_HANDLE GetDepthStencilViewHandle() { return m_DepthBufferHandle; }

	// 現在のバックバッファ (Tonemap / ImGui の描画先)
	ID3D12Resource*             GetCurrentBackBufferResource() { return m_RenderTarget[m_RTIndex].Get(); }
	D3D12_CPU_DESCRIPTOR_HANDLE GetCurrentBackBufferRTV() { return m_RenderTargetHandle[m_RTIndex]; }

	// ------------------------------------------------------------
	//  Accessors used by IBLBaker / baker classes
	// ------------------------------------------------------------
	ID3D12CommandQueue* GetCommandQueue() { return m_CommandQueue.Get(); }
	ID3D12DescriptorHeap* GetSRVDescriptorHeap() { return m_SRVDescriptorHeap.Get(); }

	unsigned int                CreateShaderResourceViewPublic(ID3D12Resource* Resource) { return CreateShaderResourceView(Resource); }
	D3D12_GPU_DESCRIPTOR_HANDLE GetShaderResourceViewHandlePublic(unsigned int SRVIndex) { return GetShaderResourceViewHandle(SRVIndex); }

	unsigned int                AllocateDescriptor();              // Reserve one slot from the SRV/UAV/CBV heap.
	D3D12_CPU_DESCRIPTOR_HANDLE GetCPUDescriptorHandle(unsigned int Index);
	D3D12_GPU_DESCRIPTOR_HANDLE GetGPUDescriptorHandle(unsigned int Index);
	void                        FlushAndResetCommandList();        // Close / Execute / Wait / Reset for immediate completion.

};
