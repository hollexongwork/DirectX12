#pragma once
#include "RenderManager.h"
#include "LightSceneProxy.h"

class FScene;
struct FSceneView;

// ============================================================
//  ShadowRendering
//  ShadowRendering.h / FProjectedShadowInfo /
//  FShadowSceneRenderer に相当するシャドウマップ描画系。
//
//  構成 (3 系統):
//    - ディレクショナル : Whole-Scene CSM (カスケードシャドウ)。
//        Texture2DArray (t14, MAX_SHADOW_CASCADES スライス)。
//        分割は CascadeDistributionExponent による指数分割、
//        各カスケードはサブフラスタの外接球 + テクセルスナップで安定化。
//    - スポット / レクト : 単一透視投影のシャドウ 1 スライス。
//        ローカルシャドウアトラス (t15, Texture2DArray) に確保。
//    - ポイント : キューブ 6 面 (world 軸整列, 90 度透視) を
//        ローカルアトラスの連続 6 スライスに描く。受光側は
//        支配軸から面を選び、デバイス深度を再構築して比較する。
//
//  データフロー:
//    FSceneRenderer::SetupLightConstants が集めたプロキシ列
//      -> InitDynamicShadows      (シャドウビュー構築 + GPU パラメータ)
//      -> RenderShadowDepthMaps   (深度パス: FScene のプロキシ列を巡回)
//      -> BindShadowResources     (b5 + t14/t15/t16 バインド)
//
//  ライトバッファ (t13) とローカルシャドウパラメータ (t16) は
//  「同じインデックス = 同じライト」で 1:1 対応する。
// ============================================================

// ---- シャドウ解像度 / 上限 ----
static const unsigned int MAX_SHADOW_CASCADES = 4;		// CSM 最大カスケード数
static const unsigned int CSM_RESOLUTION = 2048;		// カスケード 1 枚の解像度
static const unsigned int LOCAL_SHADOW_RESOLUTION = 1024;	// ローカルシャドウ 1 スライスの解像度
static const unsigned int MAX_LOCAL_SHADOW_SLICES = 16;		// ローカルアトラス総スライス数 (Point は 6 消費)

// ============================================================
//  DIRECTIONAL_SHADOW_CONSTANT (b5)
//  HLSL 側 DirectionalShadowData (ConstantBuffers.hlsl) と 1:1 ミラー必須。
// ============================================================
struct DIRECTIONAL_SHADOW_CONSTANT
{
	XMFLOAT4X4 WorldToShadowCascade[MAX_SHADOW_CASCADES];	// ワールド -> シャドウクリップ (転置済み)
	XMFLOAT4   CascadeSplits;			// 各カスケードのビュー深度遠端 (未使用は 1e9)
	XMFLOAT4   CascadeNormalOffset;		// 受光側法線オフセット [m] (テクセル世界サイズ x SlopeBias)
	XMFLOAT4   CascadeDepthBias;		// 受光側深度バイアス (NDC)
	XMFLOAT4   DirectionalShadowParams;	// x=NumCascades, y=ShadowDistance, z=FadeStart, w=1/CSM_RESOLUTION

	// ---- Distance Field Shadows (RayTraced Distance Field Shadows) ----
	XMFLOAT4   DFShadowParams0;			// x=NumDFObjects, y=DFShadowDistance [m], z=tan(LightSourceAngle/2), w=TraceDistance [m]
	XMFLOAT4   DFShadowParams1;			// x=ディレクショナル DF 有効 (0/1), y=レイ方向オフセット [m] (ShadowBias 由来), z=法線オフセット [m] (ShadowSlopeBias 由来), w=未使用
};
static_assert(sizeof(DIRECTIONAL_SHADOW_CONSTANT) == 352,
	"DIRECTIONAL_SHADOW_CONSTANT must mirror HLSL DirectionalShadowData (b5)");

// ============================================================
//  FLocalShadowParameters
//  ローカルライト 1 灯分のシャドウ投影パラメータ。
//  StructuredBuffer<FLocalShadowParameters> (t16, LocalShadowParams) として
//  ライトバッファ (t13) と同じインデックスでアップロードされる。
//  HLSL 側 (Structs.hlsl) と 1:1 ミラー必須 (逐次パック 96 bytes)。
// ============================================================
struct FLocalShadowParameters
{
	XMFLOAT4X4 WorldToShadow;		// Spot/Rect: ワールド -> シャドウクリップ (転置済み)。Point は未使用
	int        ShadowSliceIndex;	// -1 = 影なし。Point は 6 面の先頭スライス
	float      ShadowNearPlane;		// Point のデバイス深度再構築用
	float      ShadowFarPlane;		// Point のデバイス深度再構築用 (= 減衰半径)
	float      DepthBiasNDC;		// 受光側深度バイアス (NDC)
	float      InvShadowResolution;	// 1 / LOCAL_SHADOW_RESOLUTION (PCF オフセット)
	float      NormalOffsetWorld;	// 受光側法線オフセット [m] (SlopeBias * 0.05。シャドウマップ / DF 共通)
	float      DFShadow;			// 1 = シャドウマップの代わりにメッシュ SDF をレイマーチ (bUseRayTracedDistanceFieldShadows)
	float      DFSelfShadowBias;	// DF: レイ方向オフセット [m] (ShadowBias * 0.05。CSM 深度バイアスとワールド量一致)
};
static_assert(sizeof(FLocalShadowParameters) == 96,
	"FLocalShadowParameters must be 96 bytes (HLSL Structs.hlsl と 1:1 ミラー)");

// ============================================================
//  FDFObjectData
//  Distance Field オブジェクト 1 つ分の GPU データ (t18)。
//  FDistanceFieldObjectBuffers に相当し、可視かつ
//  影を落とすメッシュから毎フレーム詰め直される。
//  HLSL 側 (Structs.hlsl) と 1:1 ミラー必須 (96 bytes)。
// ============================================================
static const unsigned int MAX_DF_OBJECTS = 64;

// ポイントライトのキューブ面ガードバンド [テクセル]。
// 各面を 90 度よりわずかに広い FOV で描き、受光側 UV を同率で縮める。
// 面境界の PCF タップ (±1 テクセル) や法線オフセット由来のはみ出しが
// 隣面領域 (ボーダー = 白 = 影なし) を読んでシームに影の隙間が
// できるのを防ぐ。受光側 (ShadowFilteringCommon.hlsl) の同名定数と 1:1。
static const float POINT_SHADOW_GUARD_TEXELS = 6.0f;

// ライトのバイアス値 -> ワールドオフセット換算 [m / バイアス単位]。
// シャドウマップ経路の実効ワールド量と同一の換算を DF にも使う:
//   - CSM 深度バイアス   = (ShadowBias * 0.05) / depthRange [NDC]
//                          -> オルソのためワールド換算はちょうど ShadowBias * 0.05 [m]
//   - ローカル法線オフセット = SlopeBias * 0.05 [m]
// これにより DF の on/off でスライダーの効き方が一致する。
// DF 固有の自己交差回避 (SDF が受光面自身を指す問題) は、シェーダ側で
// SDF ボクセル幅ぶんの開始オフセットが常時・自動で適用される
// (DistanceFieldShadowing.hlsl)。ユーザーバイアスは純粋な調整量。
static const float SHADOW_BIAS_WORLD_SCALE = 0.05f;

struct FDFObjectData
{
	XMFLOAT4X4 WorldToVolume;				// ワールド -> ボリューム空間 [-1,1] (転置済み)
	XMFLOAT4   VolumeUVScaleAndDistance;	// xyz=アトラス UV スケール, w=距離値 -> ワールド距離 [m]
	XMFLOAT4   VolumeUVAdd;					// xyz=アトラス UV オフセット, w=未使用
};
static_assert(sizeof(FDFObjectData) == 96,
	"FDFObjectData must be 96 bytes (HLSL Structs.hlsl と 1:1 ミラー)");

// ============================================================
//  FProjectedShadowInfo
//  シャドウビュー 1 枚分 (FProjectedShadowInfo 相当)。
//  深度パスは各ビューごとに VIEW 定数 (b0) をライトの
//  View / Projection で詰め直して FScene のプリミティブを描く。
// ============================================================
struct FProjectedShadowInfo
{
	XMFLOAT4X4   ViewMatrixT;		// ライトビュー行列 (転置済み。VIEW 定数へ直接コピー)
	XMFLOAT4X4   ProjectionMatrixT;	// ライト射影行列 (転置済み)
	XMFLOAT4X4   ViewProjection;	// ワールド -> シャドウクリップ (転置前。キャスターカリング用)
	unsigned int SliceIndex = 0;	// 書き込み先アレイスライス
	bool         bDirectional = false;	// true = CSM アレイ / false = ローカルアトラス
};

// ============================================================
//  FShadowSceneRenderer
// ============================================================
class FShadowSceneRenderer
{
private:
	// シャドウ深度ターゲット (Texture2DArray, R32_TYPELESS -> DSV D32 / SRV R32)
	struct FShadowDepthTarget
	{
		ComPtr<ID3D12Resource>                   Resource;
		unsigned int                             SRVIndex = 0;
		std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> SliceDSV;	// スライスごとの DSV
	};

	RenderManager* m_RHI = nullptr;

	// DSV デスクリプタヒープ (シャドウ専有。CSM + ローカルの全スライス分)
	ComPtr<ID3D12DescriptorHeap> m_DSVHeap;
	unsigned int                 m_DSVHeapUsed = 0;

	FShadowDepthTarget m_CSMTarget;		// t14: CSM (MAX_SHADOW_CASCADES スライス)
	FShadowDepthTarget m_LocalTarget;	// t15: ローカルアトラス (MAX_LOCAL_SHADOW_SLICES スライス)

	// ---- ローカルシャドウパラメータ (StructuredBuffer, t16) ----
	// ライトバッファ (t13) と同じくダブルバッファのアップロードヒープ。
	ComPtr<ID3D12Resource>  m_ShadowParamBuffer[2];
	FLocalShadowParameters* m_ShadowParamPointer[2] = {};
	unsigned int            m_ShadowParamSRVIndex[2] = {};
	unsigned int            m_ShadowParamFrame = 0;

	// ---- Distance Field オブジェクトバッファ (StructuredBuffer, t18) ----
	// t16 と同じダブルバッファのアップロードヒープ。
	ComPtr<ID3D12Resource> m_DFObjectBuffer[2];
	FDFObjectData* m_DFObjectPointer[2] = {};
	unsigned int           m_DFObjectSRVIndex[2] = {};
	unsigned int           m_DFObjectFrame = 0;
	unsigned int           m_NumDFObjects = 0;

	// ---- 今フレームのシャドウビュー列 ----
	std::vector<FProjectedShadowInfo> m_ShadowViews;
	bool m_bUsedCSM = false;
	bool m_bUsedLocal = false;

	// ---- シャドウキャスターカリング ----
	// 各シャドウビューの ViewProjection から FConvexVolume を構築し、
	// フラスタム外のキャスターを深度パスから除外する
	// (FProjectedShadowInfo の SubjectPrimitives 収集に相当)。
	// FSceneRenderer::RenderShadowDepths が毎フレーム有効フラグを伝搬する。
	bool m_bFrustumCullingEnabled = true;

	// ---- ディレクショナルシャドウ定数 (b5) ----
	DIRECTIONAL_SHADOW_CONSTANT m_DirectionalConstant{};

	// ---- 内部ヘルパー ----
	void InitShadowDepthTarget(FShadowDepthTarget& Target,
		unsigned int Resolution, unsigned int ArraySize, const wchar_t* Name);
	void InitShadowParamBuffers();
	void InitDistanceFieldBuffers();

	// CSM カスケード構築 (サブフラスタ外接球 + テクセルスナップ)。
	// カメラ情報は FSceneView (ゲーム側スナップショット) から読む。
	void SetupDirectionalShadows(const FLightSceneProxy* Directional,
		const FSceneView& View);

	// スポット / レクト / ポイントのシャドウビュー + t16 パラメータ構築
	void SetupLocalShadows(const std::vector<const FLightSceneProxy*>& LocalLights);

public:
	FShadowSceneRenderer(RenderManager* RHI);
	~FShadowSceneRenderer();

	// 毎フレーム: プロキシ列からシャドウビューと GPU パラメータを構築する
	// (FSceneRenderer::InitDynamicShadows)。
	// LocalLights はライトバッファ (t13) と同順であること (t16 と 1:1 対応)。
	// View はゲーム側で構築済みの FSceneView (カメラスナップショット)。
	// View.bValid = false のフレームは CSM をスキップする。
	void InitDynamicShadows(
		const FLightSceneProxy* Directional,
		const std::vector<const FLightSceneProxy*>& LocalLights,
		const FSceneView& View);

	// Distance Field オブジェクトバッファ (t18) を今フレームの
	// プロキシ列から詰め直す (DistanceFieldObjectBuffers 更新相当)。
	// InitDynamicShadows の後 / BindShadowResources の前に呼ぶこと。
	void UpdateDistanceFieldObjects(FScene* Scene);

	// シャドウ深度パス (FSceneRenderer::RenderShadowDepthMaps)。
	// FScene のプリミティブプロキシ列を各シャドウビューで巡回する。
	void RenderShadowDepthMaps(FScene* Scene);

	// デファードライティング直前に呼ぶ: b5 + t14/t15/t16/t17/t18 をバインド
	void BindShadowResources();

	// ---- シャドウキャスターカリング (ImGui デバッグ用) ----
	// 統計は毎フレーム InitDynamicShadows でリセットされ、
	// RenderShadowDepthMaps が全シャドウビュー分を累積する。
	struct FShadowCullingStats
	{
		int NumViews = 0;		// 今フレームのシャドウビュー数
		int NumProcessed = 0;	// 判定対象キャスター数 (全ビュー累積)
		int NumDrawn = 0;		// 深度パスで描画されたキャスター数
		int NumCulled = 0;		// フラスタムで棄却されたキャスター数
	};

	void SetFrustumCullingEnabled(bool bEnabled) { m_bFrustumCullingEnabled = bEnabled; }
	const FShadowCullingStats& GetCullingStats() const { return m_CullingStats; }

	// ImGui 表示用
	unsigned int GetNumShadowViews() const { return (unsigned int)m_ShadowViews.size(); }
	unsigned int GetNumDistanceFieldObjects() const { return m_NumDFObjects; }

private:
	FShadowCullingStats m_CullingStats;
};
