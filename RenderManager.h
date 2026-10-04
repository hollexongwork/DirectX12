#pragma once
#include <deque>
#include <climits>
#include <cstddef>
#include <cstdint>

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
//    1:1 対応 (b0..b7 / t0..t36)。順序変更・挿入は禁止。
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
//  系統別に分離:
//    VIEW_CONSTANT          = FViewUniformShaderParameters 相当 (b0)
//    FORWARD_LIGHT_CONSTANT = FForwardLightData 相当 (b3)
//    PP_SETTINGS            = パス毎パラメータ (b4, PostProcessSettings.h)
//    DIRECTIONAL_SHADOW_CONSTANT = CSM シャドウ定数 (b5, ShadowRendering.h)
//    LUMEN_CONSTANT         = Lumen 定数 (b6, LumenScene.h)
//    FOG_CONSTANT           = Exponential Height Fog / Volumetric Fog 定数 (b7, FogRendering.h)
//  ※ HLSL 側 (ConstantBuffers.hlsl) と 1:1 ミラー必須
// ============================================================

// b0 : View。ビュー行列群 + カメラ + 代表ディレクショナルライト + Temporal AA / TAAU。
// 太陽ライトは View ユニフォームに常駐する。448 B (定数リング 1 スロット 512 B に収まる)。
// 行列は転置して格納する (C++ 側は転置前で保持し、アップロード時に XMMatrixTranspose)。
// 書き手: FSceneRenderer::PrepareViewStateForVisibility (カメラ + テンポラル全フィールド) と
// ComputeLightGrid (光源 2 フィールド)。シャドウ / Lumen カード / Polygon2D は
// ゼロ初期化の VIEW_CONSTANT{} を使うので、追加フィールドはすべて 0 (ミップバイアス 0 / テンポラル情報無し)
struct VIEW_CONSTANT
{
	XMFLOAT4X4		View;				//   0  ワールド -> ビュー
	XMFLOAT4X4		Projection;			//  64  ジッタ込み (ViewToClip)
	XMFLOAT4X4		InvViewProjection;	// 128  ジッタ込み (深度 + UV からのワールド復元)
	XMFLOAT4		WorldCameraOrigin;	// 192  xyz = カメラワールド位置 [m], w = 1
	XMFLOAT4		NearFar;			// 208  x=Near, y=Far, zw=0

	// ディレクショナルライト (FScene のライトリストから毎フレーム解決)
	//   DirectionalLightDirection.xyz = 受光面からライトへ向かう方向 (発光方向の逆)
	//   DirectionalLightColor.rgb     = 線形色 x 強度 (lux)
	// ライト不在時は DirectionalLightColor = 0 (無光)
	XMFLOAT4		DirectionalLightDirection;	// 224
	XMFLOAT4		DirectionalLightColor;		// 240

	// ---- Temporal AA / TAAU (FViewUniformShaderParameters の同名メンバ) ----
	XMFLOAT4X4		PrevViewProjection;	// 256  前フレーム View*Projection (前フレームのジッタ込み)
	XMFLOAT4X4		ClipToPrevClip;		// 320  InvVP_NoAA(cur) * VP_NoAA(prev)
	XMFLOAT4		TemporalAAJitter;	// 384  xy = 今フレーム NDC ジッタ, zw = 前フレーム NDC ジッタ
	XMFLOAT4		TemporalAAParams;	// 400  x = SampleIndex, y = SampleCount, zw = TemporalJitterPixels (レンダー px)
	XMFLOAT4		ViewSizeAndInvSize;	// 416  (R.x, R.y, 1/R.x, 1/R.y) (exact-size なので BufferSize と同一)
	float			MaterialTextureMipBias;				// 432  マテリアルテクスチャの SampleBias (TemporalUpscale 時のみ非 0)
	float			MaterialTextureDerivativeMultiply;	// 436  = 2^MipBias (予約。SampleGrad 用で現状未使用)
	uint32_t		StateFrameIndexMod8;	// 440  TAA 有効時 FrameIndex & 7, それ以外 0
	uint32_t		StateFrameIndex;		// 444
};
static_assert(sizeof(VIEW_CONSTANT) == 448, "VIEW_CONSTANT must mirror HLSL ViewConstantBuffer (b0)");
static_assert(offsetof(VIEW_CONSTANT, PrevViewProjection) == 256, "VIEW_CONSTANT::PrevViewProjection offset");
static_assert(offsetof(VIEW_CONSTANT, ClipToPrevClip) == 320, "VIEW_CONSTANT::ClipToPrevClip offset");
static_assert(offsetof(VIEW_CONSTANT, TemporalAAJitter) == 384, "VIEW_CONSTANT::TemporalAAJitter offset");
static_assert(offsetof(VIEW_CONSTANT, MaterialTextureMipBias) == 432, "VIEW_CONSTANT::MaterialTextureMipBias offset");
static_assert(sizeof(VIEW_CONSTANT) <= 512, "VIEW_CONSTANT must fit one constant ring slot");


// b1 : Primitive (FPrimitiveUniformShaderParameters 相当)。per-draw。128 B。
// 行列は転置して格納する。PreviousLocalToWorld は前フレームに描いた LocalToWorld で、
// ベロシティパス (DrawVelocity) だけが別の値を書く。それ以外のパスは LocalToWorld と
// 同値 (UploadPrimitiveConstant の既定)、ローカル空間描画 (Lumen カードキャプチャ /
// Polygon2D) は単位行列
struct PRIMITIVE_CONSTANT
{
	XMFLOAT4X4 LocalToWorld;			//  0 (転置)
	XMFLOAT4X4 PreviousLocalToWorld;	// 64 (転置。ベロシティパス以外は LocalToWorld と同値)
};
static_assert(sizeof(PRIMITIVE_CONSTANT) == 128, "PRIMITIVE_CONSTANT must mirror HLSL PrimitiveConstantBuffer (b1)");


// b3 : ForwardLightData (FForwardLightData 相当, 112 B)。
// ライトの数 + タイルドライトカリング (ライトグリッド) のパラメータ +
// フォワードシェーディング (半透明 / Volumetric Fog) が使う「選択されたディレクショナルライト」。
// ライト本体は StructuredBuffer<FLocalLightData> (t13, ForwardLightBuffer。
// C++ 側は FForwardLocalLightData, LightGridInjection.h):
//   [0, NumLocalLights)                                    : 視界内のローカルライト (Point/Spot/Rect)
//   [NumLocalLights, NumLocalLights + NumDirectionalLights) : ディレクショナルライト
// グリッド本体は NumCulledLightsGrid (t19) + CulledLightDataGrid (t20)。
// グリッドフィールドは FLightGridInjection::FillForwardLightData、
// ライトのフィールドは FSceneRenderer::ComputeLightGrid が毎フレーム解決する。
struct FORWARD_LIGHT_CONSTANT
{
	unsigned int	NumLocalLights = 0;			//   0 ローカルライト数
	unsigned int	NumDirectionalLights = 0;	//   4 ディレクショナルライト数
	unsigned int	NumGridCells = 0;			//   8 グリッド総セル数 (X*Y*Z)。現状どのシェーダーも読まない (将来用)
	unsigned int	HasDirectionalLight = 0;	//  12 選択されたフォワードディレクショナルライトがあるか

	unsigned int	CulledGridSizeX = 1;		//  16 画面タイル数 X (= ceil(W / LightGridPixelSize))
	unsigned int	CulledGridSizeY = 1;		//  20 画面タイル数 Y
	unsigned int	CulledGridSizeZ = 1;		//  24 Z スライス数 (LIGHT_GRID_SIZE_Z)
	unsigned int	LightGridPixelSizeShift = 6;//  28 log2(LightGridPixelSize)

	XMFLOAT3		LightGridZParams = { 1.0f, 0.0f, 1.0f };	// 32 (B, O, S): Slice = log2(Depth*B + O) * S
	unsigned int	MaxCulledLightsPerCell = 32;//  44 セルあたり保持するライト数上限

	unsigned int	LightGridDebugMode = 0;		//  48 0=off 1=複雑度ヒートマップ 2=Zスライス
	unsigned int	bUseLightGrid = 0;			//  52 0 = 全灯ループ (フォールバック)
	unsigned int	DirectionalLightBufferIndex = 0;	// 56 選択されたディレクショナルライトの t13 内の添字 (CSM / DF シャドウを持つライト)
	unsigned int	DirectionalLightFlags = 0;	//  60 選択されたディレクショナルライトの LIGHT_FLAG_*

	// ---- 選択されたフォワードディレクショナルライト ----
	XMFLOAT3		DirectionalLightColor = { 0.0f, 0.0f, 0.0f };	// 64 線形色 x 強度 (lux)
	float			DirectionalLightVolumetricScatteringIntensity = 0.0f;	// 76
	XMFLOAT3		DirectionalLightDirection = { 0.0f, 1.0f, 0.0f };	// 80 受光点 -> ライト方向
	float			DirectionalLightSourceRadius = 0.0f;	//  92 sin(見かけの半角)
	float			DirectionalLightSoftSourceRadius = 0.0f;	// 96
	float			DirectionalLightSpecularScale = 1.0f;	// 100
	float			DirectionalLightDiffuseScale = 1.0f;	// 104
	float			Pad = 0.0f;					// 108
};
static_assert(sizeof(FORWARD_LIGHT_CONSTANT) == 112, "FORWARD_LIGHT_CONSTANT must mirror HLSL ForwardLightData (b3)");




struct TEXTURE
{
	ComPtr<ID3D12Resource>	Resource;
	unsigned int			SRVIndex;
	~TEXTURE();
};


struct RENDER_TARGET
{
	ComPtr<ID3D12Resource>	Resource;
	unsigned int			SRVIndex;
	unsigned int			RTVIndex;
	D3D12_GPU_DESCRIPTOR_HANDLE SRVHandle;
	D3D12_CPU_DESCRIPTOR_HANDLE RTVHandle;

	// UAV (CreateRenderTarget の bAllowUnorderedAccess = true の時のみ。
	// TAA 履歴などコンピュートが書くターゲット用)。UINT_MAX = UAV 無し
	unsigned int				UAVIndex = UINT_MAX;
	D3D12_GPU_DESCRIPTOR_HANDLE UAVHandle{};

	// 生成時のサイズ / フォーマット (全ターゲットで記録。後段の実寸参照用)
	unsigned int			Width = 0;
	unsigned int			Height = 0;
	DXGI_FORMAT				Format = DXGI_FORMAT_UNKNOWN;

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
	HeightFog,		// One / SrcAlpha, RGB のみ (フォグパス: Dst * 透過率 + インスキャッタ)
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
	None,			// 深度無効 (DepthEnable = FALSE, DSVFormat = UNKNOWN)。
	// DSV をバインドしないフルスクリーンパス用。DSV 無しで
	// DSVFormat = D32 の PSO を使うとデバッグレイヤー
	// EXECUTION ERROR #615 (DEPTH_STENCIL_FORMAT_MISMATCH_PIPELINE_STATE) になる
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
	unsigned int                CreateRenderTargetView(ID3D12Resource* Resource, unsigned int MipLevel = 0);
	D3D12_CPU_DESCRIPTOR_HANDLE GetRenderTargetViewHandle(unsigned int RTVIndex);

	// DepthBias / SlopeScaledDepthBias はシャドウ深度 PSO 用 (既定値は従来通り 0)
	// Blend / Cull / Depth プリセットでマテリアルの Blend Mode / Two Sided
	// に対応する PSO バリアントを生成する (既定は従来の不透明上書き)
	// bOptional = true: .cso が欠落 / 空なら 1 行ログを出して nullptr を返す (assert せず、
	// PS 無しの「何も描かない PSO」も作らない)。呼び出し側は null を登録しないこと
	ComPtr<ID3D12PipelineState> CreatePipeline(const char* VertexShaderFile, const char* PixelShaderFile, const DXGI_FORMAT* RTVFormats, unsigned int NumRenderTargets, int DepthBias = 0, float SlopeScaledDepthBias = 0.0f,
		EBlendStatePreset BlendPreset = EBlendStatePreset::Opaque,
		ECullModePreset CullPreset = ECullModePreset::Back,
		EDepthStatePreset DepthPreset = EDepthStatePreset::DepthWrite,
		bool bOptional = false);

	// シェーダ可視ヒープ / ルートシグネチャ / ビューポート / シザーを設定する
	// (BeginFrame と FlushAndResetCommandList の Reset 後の復帰で共通)
	void SetDefaultGraphicsState();


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

	// ---- DXR (Lumen HWRT 用) ----
	// InitDevice が ID3D12Device5 + RaytracingTier 1.1 (RayQuery) を判定。
	// 非対応環境では null / false のままで SWRT のみが使われる。
	ComPtr<ID3D12Device5>              m_Device5;
	ComPtr<ID3D12GraphicsCommandList4> m_GraphicsCommandList4;
	bool                               m_bRayTracingSupported = false;
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
	// DSV ヒープは 1 枠固定。解像度変更時はリソースだけを作り直し、
	// 同じ CPU 枠 (m_DepthBufferHandle) へ DSV を再作成する。
	ComPtr<ID3D12Resource>       m_DepthBuffer;
	ComPtr<ID3D12DescriptorHeap> m_DepthBufferDescriptorHeap;
	D3D12_CPU_DESCRIPTOR_HANDLE  m_DepthBufferHandle{};
	unsigned int                 m_DepthBufferWidth = 0;
	unsigned int                 m_DepthBufferHeight = 0;

	// 既定ビューポート / シザー (SetDefaultGraphicsState と
	// RestoreDefaultViewport が適用する。SetDefaultViewportSize で変更)
	D3D12_RECT     m_ScissorRect{};
	D3D12_VIEWPORT m_Viewport{};
	unsigned int   m_DefaultViewportWidth = 0;
	unsigned int   m_DefaultViewportHeight = 0;

	// Descriptor heaps + free-lists
	ComPtr<ID3D12DescriptorHeap> m_SRVDescriptorHeap;
	std::list<unsigned int>      m_SRVDescriptorPool;
	static const unsigned int    SRV_DESCRIPTOR_MAX = 10000;

	ComPtr<ID3D12DescriptorHeap> m_RTVDescriptorHeap;
	std::list<unsigned int>      m_RTVDescriptorPool;
	static const unsigned int    RTV_DESCRIPTOR_MAX = 1000;

	// ---- 遅延削除キュー (Deferred Deletion) ----
	// 2フレーム・イン・フライトのため、破棄要求されたリソース /
	// デスクリプタ枠は「破棄時点で記録中のフレームの Signal 値」を
	// 添えて保留し、GPU がそのフェンス値へ到達してから実解放する。
	// (FRHIResource 遅延削除に相当。即時解放すると in-flight の
	//  コマンドリストが解放済みリソース / 上書きされたデスクリプタを
	//  参照して DEVICE_REMOVED になる)
	struct DEFERRED_RELEASE_ENTRY
	{
		UINT64                 FenceValue;     // この値まで GPU 完了で解放可
		ComPtr<ID3D12Resource> Resource;       // null = デスクリプタ枠のみ返却
		int                    SRVIndex = -1;  // -1 = なし
		int                    RTVIndex = -1;  // -1 = なし
	};
	std::deque<DEFERRED_RELEASE_ENTRY> m_DeferredReleaseQueue;

	// キュー先頭から CompletedFenceValue 以下のエントリを実解放する。
	// (Present / WaitGPU のフェンス待ち直後に呼ぶ)
	void FlushDeferredReleases(UINT64 CompletedFenceValue);

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
		LUMEN,			// b6  Lumen Surface Cache / スクリーン GI 定数 (LUMEN_CONSTANT, LumenScene.h)
		FOG,			// b7  Exponential Height Fog / Volumetric Fog 定数 (FOG_CONSTANT, FogRendering.h)
	};

	enum class TEXTURE_TYPE
	{
		// ---- G-Buffer / マテリアル共用 (ベースパス=マテリアル, ライティング=G-Buffer) ----
		BASE_COLOR = (int)CONSTANT_TYPE::FOG + 1, // t0 GBufferC / SceneColor 入力
		NORMAL,           // t1  GBufferA (World Normal)
		MSRA,             // t2  GBufferB (Metallic/Specular/Roughness/AO) / ARM
		DEPTH,            // t3  非線形深度 SRV
		LINEAR_DEPTH,     // t4  線形深度
		ENVIRONMENT,      // t5  予約・未使用 (equirect 環境マップは IBL ベイク入力のみ。レジスタ順維持)
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
		LIGHTS,           // t13  (StructuredBuffer<FLocalLightData> ForwardLightBuffer: ローカル + ディレクショナル)
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

		// ---- Lumen Surface Cache (LumenScene.h / LumenTracingCommon.hlsl) ----
		LUMEN_SCENE_OBJECTS,   // t24 (StructuredBuffer<FLumenSceneObject> LumenSceneObjects)
		LUMEN_CARDS,           // t25 (StructuredBuffer<FLumenCardData> LumenCardBuffer)
		LUMEN_FINAL_LIGHTING,  // t26 (Texture2D<float4>: Surface Cache FinalLighting)
		LUMEN_DEPTH_ATLAS,     // t27 (Texture2D<float>: カードキャプチャ深度)

		// ---- Lumen Final Gather / Reflections / Radiance Cache ----
		LUMEN_DIFFUSE_INDIRECT,// t28 (Texture2D<float4>: Screen Probe Gather 積分結果。rgb=平均入射ラディアンス, a=スカイ可視率)
		LUMEN_REFLECTIONS,     // t29 (Texture2D<float4>: 反射ラディアンス。a=適用ウェイト)
		LUMEN_RC_SH_R,         // t30 (Texture3D<float4>: Radiance Cache SH L1 (R チャンネル係数))
		LUMEN_RC_SH_G,         // t31 (Texture3D<float4>: 同 G)
		LUMEN_RC_SH_B,         // t32 (Texture3D<float4>: 同 B)

		// ---- Exponential Height Fog / Volumetric Fog (FogRendering.h / HeightFogCommon.hlsl) ----
		FOG_INSCATTERING_CUBEMAP,  // t33 (TextureCube<float4>: Inscattering Color Cubemap = IBL prefilter キューブ)
		VOLUMETRIC_FOG_INTEGRATED, // t34 (Texture3D<float4>: Volumetric Fog 積分結果 IntegratedLightScattering)

		// ---- Temporal AA (VelocityRendering.h / TemporalAA.h) ----
		VELOCITY,          // t35 (Texture2D<float2>: SceneVelocity R16G16_UNORM エンコード済み, 0 = 未書き込み)
		TEMPORAL_AA_DEBUG, // t36 (Texture2D<float4>: TAA DebugOutput / TAA 出力 (TemporalUpscalerIO))

		// ---- システムテクスチャ (SystemTextures.h) ----
		LTC_MAT,           // t37 (Texture2D<float4>: LTC 逆行列テーブル。レクトライトのスペキュラ)
		LTC_AMP,           // t38 (Texture2D<float2>: LTC 振幅 / フレネルテーブル)

		// ---- Count ----
		COUNT,             // = 47 (b0..b7 + t0..t38。ルートシグネチャ 47 DWORD)
	};

	// ------------------------------------------------------------
	//  Lifetime / singleton
	// ------------------------------------------------------------
	RenderManager();
	~RenderManager();

	static RenderManager* GetInstance() { return m_Instance; }

	// ------------------------------------------------------------
	//  Frame (BeginFrame / Present は FSceneRenderer、WaitGPU は終了時 / FlushAndResetCommandList から呼ばれる)
	// ------------------------------------------------------------
	void WaitGPU();
	// フレーム先頭: ヒープ / ルートシグネチャ / 定数リング / ビューポート
	void BeginFrame();
	// フレーム末尾: Close -> Execute -> Present -> 前フレーム待ち -> Reset
	void Present();

	// ------------------------------------------------------------
	//  Resource creation
	// ------------------------------------------------------------
	// bAllowUnorderedAccess = true で ALLOW_UNORDERED_ACCESS フラグと
	// ミップ 0 の UAV (UAVIndex / UAVHandle) を追加生成する (コンピュートの書き込み先用)。
	// 初期状態は従来どおり PIXEL_SHADER_RESOURCE。
	std::unique_ptr<RENDER_TARGET> CreateRenderTarget(unsigned int Width, unsigned int Height, DXGI_FORMAT Format, unsigned int MipLevels = 1, bool bAllowUnorderedAccess = false);
	std::unique_ptr<TEXTURE>       LoadTexture(const char* FileName, bool sRGB = false);
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
	// PipelineName が非 null の PSO として登録済みか。オプション PSO (PostProcessUpscale0..5 等) は
	// .cso 欠落時に登録されないので、SetPipelineState の前にこれで確かめる (§3.8)
	bool HasPipelineState(const char* PipelineName) const;

	// Bind a descriptor-table root parameter directly from an SRV-heap index.
	void BindRootTableBySRVIndex(unsigned int RootParameter, unsigned int SRVIndex);

	// ------------------------------------------------------------
	//  Descriptor release (called by resource destructors)
	//  ※ どちらも即時返却ではなく遅延削除キュー経由 (GPU 完了後に返却)
	// ------------------------------------------------------------
	void ReleaseShaderResourceView(unsigned int SRVIndex);
	void ReleaseRenderTargetView(unsigned int RTVIndex);

	// リソース本体とデスクリプタ枠をまとめて遅延解放する。
	// Resource の所有権を引き取り、GPU が現在記録中のフレームを
	// 完了するまで生存させる (TEXTURE / RENDER_TARGET デストラクタ、
	// および実行時のアセット差し替え時に使用)。
	void DeferredRelease(ComPtr<ID3D12Resource> Resource,
		int SRVIndex = -1, int RTVIndex = -1);

	// ------------------------------------------------------------
	//  Simple accessors
	// ------------------------------------------------------------
	ID3D12Device* GetDevice() { return m_Device.Get(); }
	ID3D12GraphicsCommandList* GetGraphicsCommandList() { return m_GraphicsCommandList.Get(); }

	// ---- DXR (Lumen HWRT 用) ----
	ID3D12Device5* GetDevice5() { return m_Device5.Get(); }
	ID3D12GraphicsCommandList4* GetGraphicsCommandList4() { return m_GraphicsCommandList4.Get(); }
	bool IsRayTracingSupported() const { return m_bRayTracingSupported; }
	int                        GetBackBufferWidth() { return m_BackBufferWidth; }
	int                        GetBackBufferHeight() { return m_BackBufferHeight; }

	// 現在のフレームインデックス (0/1)。CPU が毎フレーム書き換える
	// アップロードリソースのダブルバッファリング用 (in-flight フレームとの
	// 書き込み競合防止。AutoExposure / ColorGradingLUTBaker / Polygon2D)。
	unsigned int               GetCurrentFrameIndex() const { return m_RTIndex; }

	// 深度バッファ (DSV は RHI 所有 / SRV は FSceneTextures が生成)
	ID3D12Resource* GetDepthBufferResource() { return m_DepthBuffer.Get(); }
	D3D12_CPU_DESCRIPTOR_HANDLE GetDepthStencilViewHandle() { return m_DepthBufferHandle; }

	// 深度バッファの再確保 (レンダー解像度変更用)。
	//  ReleaseDepthBuffer : リソースを遅延削除キューへ (DSV 枠は保持)
	//  CreateDepthBuffer  : R32_TYPELESS / DEPTH_WRITE で生成し、同一 DSV 枠へ DSV を再作成
	void         ReleaseDepthBuffer();
	void         CreateDepthBuffer(unsigned int Width, unsigned int Height);
	unsigned int GetDepthBufferWidth() const { return m_DepthBufferWidth; }
	unsigned int GetDepthBufferHeight() const { return m_DepthBufferHeight; }

	// ---- 既定ビューポート / シザー ----
	// SetDefaultViewportSize : m_Viewport / m_ScissorRect を書き換える
	//                          (次の BeginFrame / FlushAndResetCommandList から適用)
	// RestoreDefaultViewport : 既定値を現在のコマンドリストへ即時記録する
	//                          (シャドウ / Lumen カードキャプチャ後の復帰用)
	void         SetDefaultViewportSize(unsigned int Width, unsigned int Height);
	void         RestoreDefaultViewport();
	unsigned int GetDefaultViewportWidth() const { return m_DefaultViewportWidth; }
	unsigned int GetDefaultViewportHeight() const { return m_DefaultViewportHeight; }

	// ---- リソース / デスクリプタ計数 (リーク検査・統計表示用) ----
	size_t GetNumFreeSRVDescriptors() const { return m_SRVDescriptorPool.size(); }
	size_t GetNumFreeRTVDescriptors() const { return m_RTVDescriptorPool.size(); }
	size_t GetDeferredReleaseQueueLength() const { return m_DeferredReleaseQueue.size(); }
	// ローカル (ビデオ) メモリの現在使用量 [byte] (IDXGIAdapter3::QueryVideoMemoryInfo)。取得失敗時は 0
	UINT64 QueryLocalVideoMemoryUsage();

	// 現在のバックバッファ (Tonemap / ImGui の描画先)
	ID3D12Resource* GetCurrentBackBufferResource() { return m_RenderTarget[m_RTIndex].Get(); }
	D3D12_CPU_DESCRIPTOR_HANDLE GetCurrentBackBufferRTV() { return m_RenderTargetHandle[m_RTIndex]; }

	// ------------------------------------------------------------
	//  Accessors used by IBLBaker / baker classes
	// ------------------------------------------------------------
	ID3D12DescriptorHeap* GetSRVDescriptorHeap() { return m_SRVDescriptorHeap.Get(); }

	unsigned int                AllocateDescriptor();              // Reserve one slot from the SRV/UAV/CBV heap.
	D3D12_CPU_DESCRIPTOR_HANDLE GetCPUDescriptorHandle(unsigned int Index);
	D3D12_GPU_DESCRIPTOR_HANDLE GetGPUDescriptorHandle(unsigned int Index);
	void                        FlushAndResetCommandList();        // Close / Execute / Wait / Reset for immediate completion.

};
