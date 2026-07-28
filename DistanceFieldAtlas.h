#pragma once
#include "RenderManager.h"

#include <string>
#include <vector>

// ============================================================
//  DistanceFieldAtlas
//  Mesh Distance Field (MDF) 生成 + Volume Texture Atlas に
//  相当する。FBXModel::Load 時にメッシュ単位で SDF をベイクし、
//  Texture3D アトラス (t17) の固定スロットへ格納する。
//
//    - ベイク : DistanceFieldBake_CS (cs_5_0)。ボクセルごとに
//        全三角形への最短距離 + 3 軸レイ交差の多数決で符号を決定
//        (GenerateDistanceFieldVolumeData の簡易版)
//    - キャッシュ : <fbxパス>.mdf にベイク結果を保存し、
//        次回起動時は GPU ベイクをスキップ ( DDC )
//    - 格納 : R16_FLOAT の Texture3D。スロットは X 方向に
//        DF_VOLUME_RES ずつ並ぶ (64 x MAX_DF_MESHES, 64, 64)
//
//  距離値は「パディング済みローカル境界の最大半幅」で正規化
//  ([-1,1] クランプ)。ワールド距離への復元は
//    world = value * DistanceScaleLocal * maxAxisScale(LocalToWorld)
//  で行う (FShadowSceneRenderer::UpdateDistanceFieldObjects)。
// ============================================================

// ---- 定数 ----
static const unsigned int DF_VOLUME_RES = 64;		// 1 メッシュあたりの SDF 解像度 (64^3)
static const unsigned int MAX_DF_MESHES = 16;		// アトラススロット数
static const float        DF_BORDER_FRACTION = 0.2f;	// 境界パディング比率 (DistanceField spread)

// ============================================================
//  FDistanceFieldMeshInfo
//  メッシュ 1 つ分の SDF 情報 (FBXModel が保持し、
//  FStaticMeshSceneProxy 経由でシャドウ描画系へ渡る)
// ============================================================
struct FDistanceFieldMeshInfo
{
	bool     bValid = false;
	int      SlotIndex = -1;

	// アトラス UV 変換: uv = uvw(0..1) * UVScale + UVAdd
	// (ボクセル中心整列の恒等マッピング。スロット境界のブリード防止は
	//  シェーダ側の uvw クランプが担う)
	XMFLOAT3 UVScale = { 0.0f, 0.0f, 0.0f };
	XMFLOAT3 UVAdd = { 0.0f, 0.0f, 0.0f };

	// パディング済みローカル境界 (ボリューム空間 [-1,1] との対応)
	XMFLOAT3 LocalBoundsCenter = { 0.0f, 0.0f, 0.0f };
	XMFLOAT3 LocalBoundsExtent = { 1.0f, 1.0f, 1.0f };	// 半幅

	// 距離の正規化係数 (= LocalBoundsExtent の最大成分)
	float    DistanceScaleLocal = 1.0f;
};

// ============================================================
//  FDistanceFieldAtlas
//  GDistanceFieldVolumeTextureAtlas 相当のグローバルアトラス
// ============================================================
class FDistanceFieldAtlas
{
private:
	RenderManager* m_RHI = nullptr;

	// ---- ベイク用コンピュート (独立ルートシグネチャ) ----
	// 0: b0 CBV (ベイクパラメータ) / 1: t0 SRV (三角形) / 2: u0 UAV (出力)
	ComPtr<ID3D12RootSignature> m_BakeRootSignature;
	ComPtr<ID3D12PipelineState> m_BakePSO;
	ComPtr<ID3D12Resource>      m_BakeParamBuffer;	// 小さな UPLOAD CB

	// ---- アトラス本体 (Texture3D R16_FLOAT, t17) ----
	ComPtr<ID3D12Resource> m_AtlasTexture;
	unsigned int           m_AtlasSRVIndex = 0;
	unsigned int           m_NumAllocatedSlots = 0;

	// ---- アトラスアップロード用スクラッチ (RowPitch 256 対応) ----
	ComPtr<ID3D12Resource> m_UploadScratch;

	// ---- 内部ヘルパー ----
	bool TryLoadCache(const std::string& CachePath, std::vector<float>& OutVoxels) const;
	void SaveCache(const std::string& CachePath, const std::vector<float>& Voxels) const;

	// GPU ベイク (Dispatch -> Flush -> リードバック)
	void BakeOnGPU(const std::vector<XMFLOAT3>& TriangleVertices,
		const XMFLOAT3& VolumeMin, const XMFLOAT3& VoxelSize,
		float InvNormalizeExtent, std::vector<float>& OutVoxels);

	// float ボクセル列を half へ変換してアトラスのスロットへコピー
	void UploadSlot(unsigned int SlotIndex, const std::vector<float>& Voxels);

	FDistanceFieldAtlas(RenderManager* RHI);

public:
	~FDistanceFieldAtlas();

	// グローバルアトラス (GDistanceFieldVolumeTextureAtlas)。
	// 初回呼び出しで生成する。RenderManager 初期化後にのみ呼ぶこと。
	static FDistanceFieldAtlas& Get();

	// メッシュを登録して SDF をベイク (キャッシュがあればロード) し、
	// アトラススロットへ格納する。FBXModel::Load から呼ばれる。
	//   FilePath         : キャッシュファイル名の元 (<FilePath>.mdf)
	//   TriangleVertices : ローカル空間の三角形頂点列 (3 頂点 x 三角形数)
	//   BoundsMin/Max    : ローカル境界 (パディング前)
	void AddMesh(const char* FilePath,
		const std::vector<XMFLOAT3>& TriangleVertices,
		const XMFLOAT3& BoundsMin, const XMFLOAT3& BoundsMax,
		FDistanceFieldMeshInfo& OutInfo);

	unsigned int GetAtlasSRVIndex() const { return m_AtlasSRVIndex; }
	unsigned int GetNumAllocatedSlots() const { return m_NumAllocatedSlots; }
};
