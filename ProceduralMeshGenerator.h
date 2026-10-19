#pragma once
#include "RenderManager.h"

#include <vector>
#include <functional>

// ============================================================
//  ProceduralMeshGenerator
//  CPU 側でプロシージャルにメッシュ (頂点 / インデックス列) を
//  生成するユーティリティ。UProceduralMeshComponent の
//  FProcMeshSection + KismetProceduralMeshLibrary
//  (CalculateTangentsForMesh 等) に相当する。
//
//  生成結果 (FProceduralMeshData) は FBXModel::BuildFromProceduralMesh
//  で GPU バッファ化され、FBX 由来のメッシュと同じ経路
//  (Mesh Distance Field / DXR BLAS / フラスタム境界) で描画される。
//  FAssetManager::CreateProceduralMesh が名前キーで共有キャッシュする。
//
//  規約:
//    - 座標系は左手 Y-up (FBX ロードと同じ aiProcess_ConvertToLeftHanded 後の空間)
//    - 表面は時計回り (RenderManager の FrontCounterClockwise = FALSE)。
//      幾何法線 = cross(v1 - v0, v2 - v0) が外向きになる巻き順
//    - 接線は +U 方向、従法線はシェーダ側で cross(N, T) として再構成される
//      (BasePassCommon.hlsl BuildTBN) ため、各ジェネレータは
//      dP/dV = cross(N, dP/dU) となる UV パラメータ化を採る (右手系ハンドネス)。
//      ComputeTangents は負ハンドネスの頂点数を返すので検証に使える
//    - 寸法の単位はメートル (シーンの既存アセットと同じ)
// ============================================================

// ------------------------------------------------------------
//  FProceduralMeshSection
//  FProcMeshSection 相当。1 サブセット (= 1 マテリアルスロット) 分の
//  三角形リスト。インデックスは 32bit、頂点は VERTEX_3D と同レイアウト。
// ------------------------------------------------------------
struct FProceduralMeshSection
{
	std::vector<VERTEX_3D>    Vertices;
	std::vector<unsigned int> Indices;
	unsigned int              MaterialIndex = 0;

	// Mesh Distance Field のベイクに三角形を含めるか。葉カードのような
	// 開いた面は (レイ交差の偶奇で符号を決める) SDF の内外判定を乱すので
	// false にする。描画 / シャドウ / BLAS には関係しない
	bool                      bAffectDistanceField = true;

	bool         IsEmpty() const { return Vertices.empty() || Indices.size() < 3; }
	unsigned int GetNumTriangles() const { return static_cast<unsigned int>(Indices.size() / 3); }
};

// ------------------------------------------------------------
//  FProceduralMeshData
//  複数セクションをまとめたメッシュ 1 体分。FBXModel の
//  サブセット列に 1:1 で対応する。
// ------------------------------------------------------------
struct FProceduralMeshData
{
	std::vector<FProceduralMeshSection> Sections;

	bool         IsEmpty() const;
	unsigned int GetNumVertices() const;
	unsigned int GetNumTriangles() const;

	// 単一セクションからの構築ヘルパ
	static FProceduralMeshData FromSection(FProceduralMeshSection Section);

	// 頂点位置 + インデックスの FNV-1a ハッシュ (Distance Field
	// キャッシュのキーなど、ジオメトリ同一性の判定に使う)
	unsigned long long ComputeGeometryHash() const;
};

// ------------------------------------------------------------
//  FProceduralNoise
//  シード付き Perlin ノイズ (改良版 Perlin, 2D / 3D) と fBm。
//  Terrain / Rock ジェネレータの変位ソース。
// ------------------------------------------------------------
class FProceduralNoise
{
private:
	unsigned char m_Perm[512];

	static float Fade(float t) { return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f); }
	static float Lerp(float a, float b, float t) { return a + (b - a) * t; }
	static float Grad(int hash, float x, float y, float z);

public:
	explicit FProceduralNoise(unsigned int Seed = 0);

	// [-1, 1] の Perlin ノイズ
	float Noise2D(float x, float y) const;
	float Noise3D(float x, float y, float z) const;

	// fBm (fractal Brownian motion)。各オクターブの振幅合計で正規化し
	// おおむね [-1, 1] に収める
	float FBM2D(float x, float y, unsigned int Octaves, float Lacunarity, float Persistence) const;
	float FBM3D(float x, float y, float z, unsigned int Octaves, float Lacunarity, float Persistence) const;
};

// ------------------------------------------------------------
//  ジェネレータのパラメータ
// ------------------------------------------------------------

// 地形 (ハイトフィールド)。XZ 平面のグリッドを fBm で Y 方向に変位する
struct FProceduralTerrainParams
{
	float        Width = 20.0f;			// X 方向の全長
	float        Depth = 20.0f;			// Z 方向の全長
	unsigned int SubdivisionsX = 64;	// X 方向の分割数 (クアッド数)
	unsigned int SubdivisionsZ = 64;	// Z 方向の分割数
	float        HeightScale = 2.0f;	// fBm [-1,1] -> 高さ (m)
	float        Frequency = 0.15f;		// ノイズ周波数 (1/m)
	unsigned int Octaves = 5;
	float        Lacunarity = 2.0f;
	float        Persistence = 0.5f;
	unsigned int Seed = 1;
	XMFLOAT2     UVTiling = { 4.0f, 4.0f };	// テクスチャ繰り返し回数

	// 高さに応じて頂点カラーを LowColor -> HighColor で補間する
	// (bColorByHeight = false なら白)
	bool         bColorByHeight = false;
	XMFLOAT4     LowColor = { 0.25f, 0.35f, 0.15f, 1.0f };
	XMFLOAT4     HighColor = { 0.8f, 0.8f, 0.8f, 1.0f };
};

// 岩。イコスフィアを fBm で法線方向に変位する
struct FProceduralRockParams
{
	float        Radius = 1.0f;
	unsigned int Subdivisions = 3;		// イコスフィアの分割回数 (3 = 1280 三角形)
	float        Displacement = 0.35f;	// 変位振幅 (Radius に対する比率)
	float        Frequency = 1.2f;		// ノイズ周波数 (1/m)
	unsigned int Octaves = 4;
	float        Lacunarity = 2.0f;
	float        Persistence = 0.5f;
	unsigned int Seed = 7;
	XMFLOAT3     Stretch = { 1.0f, 0.8f, 1.0f };	// 軸ごとの非等方スケール
	bool         bFlatShading = true;	// 面ごとに頂点を分離してフラット法線にする
};

// ------------------------------------------------------------
//  FProceduralMeshBuilder
//  基本形状ジェネレータ + メッシュ後処理 (法線 / 接線 / 変換 / 結合)
// ------------------------------------------------------------
class FProceduralMeshBuilder
{
public:
	// ---- パラメトリック曲面の汎用グリッド ----
	// Surface(u, v, OutPosition, OutNormal) を (ColumnsU + 1) x (RowsV + 1) の
	// 格子点で評価して三角形化する。u / v はどちらも [0, 1]。
	// UV = (u * UVScale.x, v * UVScale.y)。
	using FSurfaceFunc = std::function<void(float u, float v, XMFLOAT3& OutPosition, XMFLOAT3& OutNormal)>;
	static FProceduralMeshSection CreateParametricSurface(
		const FSurfaceFunc& Surface, unsigned int ColumnsU, unsigned int RowsV,
		const XMFLOAT2& UVScale = { 1.0f, 1.0f });

	// ---- 基本形状 ----
	// 直方体。HalfExtent は各軸の半幅。面ごとに頂点を分離 (24 頂点)
	static FProceduralMeshSection CreateBox(const XMFLOAT3& HalfExtent,
		const XMFLOAT2& UVScale = { 1.0f, 1.0f });

	// XZ 平面 (法線 +Y)、原点中心。Width = X 方向、Depth = Z 方向の全長
	static FProceduralMeshSection CreatePlane(float Width, float Depth,
		unsigned int SubdivisionsX = 1, unsigned int SubdivisionsZ = 1,
		const XMFLOAT2& UVScale = { 1.0f, 1.0f });

	// UV 球 (経度 Slices x 緯度 Stacks)、原点中心
	static FProceduralMeshSection CreateSphere(float Radius,
		unsigned int Slices = 32, unsigned int Stacks = 16);

	// イコスフィア (正二十面体の再帰分割)。頂点は共有 (スムーズ法線)
	static FProceduralMeshSection CreateIcoSphere(float Radius, unsigned int Subdivisions = 2);

	// Y 軸方向の円柱 / 円錐台。原点中心、高さ Height。
	// TopRadius = 0 で円錐。bCaps = false で側面のみ
	static FProceduralMeshSection CreateCylinder(float BottomRadius, float TopRadius, float Height,
		unsigned int Slices = 32, unsigned int Stacks = 1, bool bCaps = true);

	// 円錐 (CreateCylinder の TopRadius = 0 ラッパ)
	static FProceduralMeshSection CreateCone(float Radius, float Height,
		unsigned int Slices = 32, bool bCap = true);

	// トーラス (XZ 平面のリング)。MajorRadius = リング半径、MinorRadius = 管の半径
	static FProceduralMeshSection CreateTorus(float MajorRadius, float MinorRadius,
		unsigned int MajorSegments = 32, unsigned int MinorSegments = 16);

	// Y 軸方向のカプセル。全高 = 2 * (HalfHeight + Radius)
	static FProceduralMeshSection CreateCapsule(float Radius, float HalfHeight,
		unsigned int Slices = 32, unsigned int HemisphereStacks = 8);

	// ---- ノイズベース ----
	static FProceduralMeshSection CreateTerrain(const FProceduralTerrainParams& Params);
	static FProceduralMeshSection CreateRock(const FProceduralRockParams& Params);

	// ---- 後処理 ----
	// 面法線の面積加重平均でスムーズ法線を再計算する
	static void ComputeSmoothNormals(FProceduralMeshSection& Section);

	// 三角形ごとに頂点を複製してフラット法線にする (頂点数 = 三角形数 x 3)
	static void ComputeFlatNormals(FProceduralMeshSection& Section);

	// UV 微分から接線を計算する (Lengyel 法 + Gram-Schmidt 直交化)。
	// 戻り値は負ハンドネス (cross(N, T) が dP/dV と逆向き) の頂点数。
	// 0 でなければそのメッシュの法線マップは一部で鏡像になる
	static unsigned int ComputeTangents(FProceduralMeshSection& Section);

	// 位置 / 法線 / 接線を行列で変換する (法線は逆転置ではなく
	// 3x3 回転部をそのまま適用した後に正規化。非等方スケール時は
	// ComputeSmoothNormals を再実行すること)
	static void Transform(FProceduralMeshSection& Section, const XMMATRIX& Matrix);

	// 全三角形の巻き順を反転する (法線は反転しない)
	static void FlipWinding(FProceduralMeshSection& Section);

	// 全頂点カラーを設定する
	static void SetVertexColor(FProceduralMeshSection& Section, const XMFLOAT4& Color);

	// Source を Destination の末尾へ結合する (インデックスはオフセットされる)
	static void Append(FProceduralMeshSection& Destination, const FProceduralMeshSection& Source);

	// ---- 検証 ----
	// 各三角形の幾何法線が頂点法線と同じ向き (= 表面が時計回り) になって
	// いるかを確認する。戻り値は逆向きの三角形数。縮退三角形は数えない
	static unsigned int CountInvertedTriangles(const FProceduralMeshSection& Section);

private:
	// 頂点法線を基準に巻き順を正して三角形を追加する
	static void AddTriangleOriented(FProceduralMeshSection& Section,
		unsigned int I0, unsigned int I1, unsigned int I2);
};
