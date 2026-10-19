#pragma once
#include "ProceduralMeshGenerator.h"

#include <string>

// ============================================================
//  ProceduralTree
//  パラメトリックな木のメッシュ生成 (Weber & Penn "Creation and
//  Rendering of Realistic Trees" の簡略版)。
//
//  幹 (Level 0) から再帰的に枝 (Level 1..) を生やし、各枝は
//  セグメント列の曲がり + 屈光性 (Tropism) で形を決める。枝は
//  先細りのチューブ (回転最小フレームで周方向の捻れを抑える)、
//  葉は最終階層の枝に貼るマスク付きカード (四角形) として出力する。
//
//  出力 (FProceduralMeshData) は 2 セクション:
//    Section 0 : 樹皮 (幹 + 全階層の枝 + 先端 / 根元キャップ) MaterialIndex 0
//    Section 1 : 葉カード                                      MaterialIndex 1
//  葉カードは開いた面なので Mesh Distance Field には寄与させない
//  (bAffectDistanceField = false。閉じていない面はレイ交差の偶奇で
//  符号を決める SDF ベイクを乱す)。DXR BLAS / 描画 / シャドウには参加する。
//
//  同じ FProceduralTreeParams (ComputeHash が同値) からは常に同じ
//  メッシュが生成される (乱数はシード付き mt19937 で、分布は自前の
//  一様変換なので処理系に依らず決定的)。
//
//  規約は ProceduralMeshGenerator.h と同じ (左手 Y-up、表面は時計回り、
//  接線 = +U、dP/dV = cross(N, dP/dU)、単位はメートル)。
//  樹皮の UV は U = 周方向 (根元の円周で整数タイル)、V = 枝に沿った
//  距離 x BarkUVTilesPerMeter。葉の UV は V = 0 が葉先、V = 1 が付け根。
// ============================================================

// ------------------------------------------------------------
//  FProceduralTreeLevelParams
//  1 階層分のパラメータ。Level 0 = 幹、Level 1 = 幹から出る枝、...
//  Length / Radius は Level 0 では絶対値 [m]、Level 1 以降は親に
//  対する比率として解釈する。
// ------------------------------------------------------------
struct FProceduralTreeLevelParams
{
	int   Branches = 0;				// 親 1 本あたりの枝数 (Level 0 では未使用)
	float Length = 1.0f;			// Level 0: 幹の長さ [m] / Level 1+: 親の長さに対する比率
	float LengthVariance = 0.0f;	// Length のランダム変動 (±比率)
	float LengthShape = 1.0f;		// 親の先端側に生える枝の長さ倍率 (付け根側 1 -> 先端側 LengthShape)
	float Radius = 0.5f;			// Level 0: 幹の根元半径 [m] / Level 1+: 親の付け根半径に対する比率
	float Taper = 0.7f;				// 先端半径 / 根元半径 (0 = 尖る, 1 = 細らない)
	float DownAngle = 60.0f;		// 親の軸からの角度 [deg]
	float DownAngleVariance = 10.0f;
	float Rotate = 137.5f;			// 親の軸まわりの分岐角 [deg] (既定は黄金角)
	float RotateVariance = 20.0f;
	float Curve = 0.0f;				// 枝全体の曲がり [deg] (+ で上方向 / - で下方向)
	float CurveVariance = 20.0f;	// 枝全体でのランダムな曲がり [deg]
	float BaseOffset = 0.3f;		// 子を生やし始める位置 (0..1, 親の長さ比)
	int   Segments = 6;				// 長さ方向の分割数
	int   RadialSegments = 8;		// 周方向の分割数
};

// ------------------------------------------------------------
//  FProceduralTreeParams
//  木 1 本分のパラメータ。既定値は落葉樹 (広葉樹) 風の 3 階層。
// ------------------------------------------------------------
struct FProceduralTreeParams
{
	static constexpr int MaxLevels = 4;

	unsigned int Seed = 1;
	int   Levels = 3;					// 階層数 (1 = 幹のみ .. MaxLevels)
	float Tropism = 0.2f;				// 枝全体で上方向 (+Y) へ引く強さ (負で垂れ下がる)
	float BarkUVTilesPerMeter = 1.0f;	// 樹皮テクスチャの V 方向タイル数 [1/m]

	// ---- 葉 ----
	bool  bGenerateLeaves = true;
	int   LeavesPerBranch = 12;			// 最終階層の枝 1 本あたりの葉の数
	float LeafSize = 0.22f;				// 葉の長さ [m] (幅は LeafAspect 倍)
	float LeafSizeVariance = 0.3f;		// LeafSize のランダム変動 (±比率)
	float LeafAspect = 0.6f;			// 幅 / 長さ
	float LeafAngle = 50.0f;			// 枝の軸からの角度 [deg]
	float LeafAngleVariance = 20.0f;
	float LeafTintVariance = 0.25f;		// 頂点カラーによる葉ごとの明度ばらつき (0..1)

	FProceduralTreeLevelParams Level[MaxLevels];

	FProceduralTreeParams();

	// パラメータの同一性判定 / メッシュキャッシュのキー (FNV-1a 64bit)。
	// 生成アルゴリズムを変えたときは GeneratorVersion を上げて
	// 古い SDF キャッシュ (Saved/ProceduralMesh) と衝突しないようにする
	static constexpr unsigned int GeneratorVersion = 1;
	unsigned long long ComputeHash() const;

	// 範囲外の値を生成可能な範囲へ丸める (Details / INI からの入力用)
	void Sanitize();
};

// 生成結果の統計 (Details 表示用)
struct FProceduralTreeStats
{
	unsigned int NumStems = 0;
	unsigned int NumLeaves = 0;
	unsigned int NumVertices = 0;
	unsigned int NumTriangles = 0;
};

// ------------------------------------------------------------
//  FProceduralTreeBuilder
// ------------------------------------------------------------
class FProceduralTreeBuilder
{
public:
	static constexpr unsigned int BarkMaterialIndex = 0;
	static constexpr unsigned int LeafMaterialIndex = 1;

	// Params から木のメッシュを生成する。Sections[0] = 樹皮、
	// Sections[1] = 葉 (bGenerateLeaves = false でも空セクションとして存在する)
	static FProceduralMeshData Build(const FProceduralTreeParams& Params,
		FProceduralTreeStats* OutStats = nullptr);
};

// ------------------------------------------------------------
//  FProceduralTreeTextures
//  木用のテクスチャ (樹皮 BaseColor / Normal / ARM、葉 BaseColor / ARM、
//  平坦法線) を CPU で生成して Saved/ProceduralTexture/*.dds に書き出す。
//  既存のマテリアル経路 (FAssetManager::LoadTexture -> DDS ローダ) を
//  そのまま使うため、ファイルとして用意する (ミップ付き RGBA8)。
//  ファイルが揃っていれば何もしない。
// ------------------------------------------------------------
class FProceduralTreeTextures
{
public:
	static constexpr unsigned int Version = 1;	// 絵柄を変えたら上げる (ファイル名に入る)

	static std::string GetDirectory();
	static std::string GetBarkBaseColorPath();
	static std::string GetBarkNormalPath();
	static std::string GetBarkARMPath();
	static std::string GetLeafBaseColorPath();
	static std::string GetLeafARMPath();
	static std::string GetFlatNormalPath();

	// 無いファイルだけ生成する。全て揃っていれば (生成済みなら) true
	static bool EnsureGenerated();
};
