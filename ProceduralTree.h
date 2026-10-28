#pragma once
#include "ProceduralMeshGenerator.h"

#include <string>

// ============================================================
//  ProceduralTree
//  パラメトリックな木のメッシュ生成 (Weber & Penn "Creation and
//  Rendering of Realistic Trees" の簡略版、v2)。
//
//  幹 (Level 0) から再帰的に枝 (Level 1..) を生やし、各枝は
//  セグメント列の曲がり (Curve / CurveBack / 低域通過した揺れ) +
//  屈光性 (Tropism) + 屈曲 (Level 1 / 2 と幹の分岐より上のジグザグ、
//  CurveVariance から) で形を決める。幹は Level[0].DownAngle だけ傾き、
//  幹と太い枝は非対称に二又に分岐でき (BaseSplits / SegSplits。主軸を継ぐ片と
//  細く開く脇の片)、Level 1 の枝の長さは樹冠の形 (CrownShape) と包絡 (Prune*)
//  で決まる (包絡の天井より上へ反った枝 / 包絡の外へ出た小枝の先は縮める)。
//  半径はパイプモデル (PipeExponent) で子枝の分だけ段階的に細くなり、
//  根元は張り出す (TrunkFlare)。
//
//  生成は 2 パス: (A) 骨格パス = 乱数を消費して骨格と葉の仕様を決める、
//  (B) 出力パス = 樹冠の統計 (中心 / 葉密度グリッド) を取ってから
//  ジオメトリを出す (乱数は引かない)。乱数は枝ごとのサブストリーム
//  (HashCombine(親シード, 子番号, 階層)) なので、ある枝のパラメータを
//  変えても他の枝は変わらない。
//
//  出力 (FProceduralMeshData) は常に 4 セクション (空も可、添字 = MaterialIndex):
//    0 : 樹皮 (幹 + 太い枝、閉じたチューブ)          Opaque、SDF に寄与
//    1 : 単葉カード (マスク付き四角形)                Masked / TwoSided / Substrate TwoSidedWrap、SDF 非寄与
//    2 : 葉の房カード (房アトラス、2x2 セル)          葉と同じ、SDF 非寄与
//    3 : 小枝 (根元半径 < TwigRadiusThreshold の枝)   Opaque、閉じたチューブだが SDF 非寄与 (64^3 より細い)
//  開いた面 (葉) はレイ交差の偶奇で符号を決める SDF ベイクを乱すので
//  SDF には入れない。DXR BLAS / 描画 / シャドウには全セクションが参加する。
//
//  同じ FProceduralTreeParams (ComputeHash が同値) からは常に同じ
//  メッシュが生成される (乱数は mt19937 + 自前の一様変換)。
//  形の定数 (屈曲の強さ、主枝の段 / 半径の下限、房カードの配置など) は
//  ProceduralTree.cpp 冒頭の constexpr、設計は Docs/Procedural/ProceduralTree_Design.md。
//
//  規約は ProceduralMeshGenerator.h と同じ (左手 Y-up、表面は時計回り、
//  接線 = +U、dP/dV = cross(N, dP/dU)、単位はメートル)。
//  樹皮の UV は U = 周方向 (根元の円周で整数タイル)、V = 枝に沿った
//  距離 (U と同じテクセル密度)。葉の UV は V = 0 が葉先、V = 1 が付け根。
// ============================================================

// 樹冠の形 (Weber & Penn ShapeRatio)。Level 1 の枝の長さ分布に使う
enum class ECrownShape : int
{
	Conical = 0,
	Spherical = 1,
	Hemispherical = 2,
	Cylindrical = 3,
	TaperedCylindrical = 4,
	Flame = 5,
	InverseConical = 6,
	TendFlame = 7,
	MAX,
};

// Details の Preset コンボ用。Oak5m = 既定値 (コンストラクタ)、Sapling = 旧 v1 の既定値
enum class ETreePreset : int
{
	Oak5m = 0,
	Sapling = 1,
	MAX,
};

// ------------------------------------------------------------
//  FProceduralTreeLevelParams
//  1 階層分のパラメータ。Level 0 = 幹、Level 1 = 幹から出る枝、...
//  Length / Radius は Level 0 では絶対値 [m]、Level 1 以降は親に
//  対する比率として解釈する。ここの初期化子は汎用の値で、Oak5m /
//  Sapling は全ての階層を FProceduralTreeParams のコンストラクタ /
//  MakePreset で設定する。
// ------------------------------------------------------------
struct FProceduralTreeLevelParams
{
	// ---- v1 から (意味は変えない。Rotate は bAlternate = true のとき面からの開き角として読む) ----
	int   Branches = 0;				// 親 1 本あたりの枝数 (Level 0 では未使用)。分岐したクローン片にはセグメント数比で配分する
	float Length = 1.0f;			// Level 0: 幹の長さ [m] / Level 1+: 親の長さに対する比率
	float LengthVariance = 0.0f;	// Length のランダム変動 (±比率)
	float LengthShape = 1.0f;		// 親の先端側に生える枝の長さ倍率 (Level 1 では CrownShape に掛かる倍率)
	float Radius = 0.5f;			// Level 0: 幹の根元半径 [m] / Level 1+: 子の半径の上限 (親の付け根半径比、0.9 まで)。Level 1 は下限 0.45 x 親もある
	float Taper = 0.7f;				// 先端半径 / 根元半径 (0 = 尖る, 1 = 細らない)。元の枝 (分岐前) の全長で測り、パイプモデルありでは凸形 1 - (1 - Taper) x^1.5。
									// 刈り込みで縮めた枝は Taper x (縮めた比) で先端を細らせる。先端は MinTipRadius を下回らない
	float DownAngle = 60.0f;		// 親の軸からの角度 [deg]。Level 0 では幹の傾き (鉛直から、30 まで。方位は LatticeHash、DownAngleVariance は使わない)
	float DownAngleVariance = 10.0f;
	float Rotate = 137.5f;			// 親の軸まわりの分岐角 [deg] (既定は黄金角)。bAlternate のときは面からの開き角
	float RotateVariance = 20.0f;
	float Curve = 0.0f;				// 枝全体の曲がり [deg] (+ で上方向 / - で下方向)。CurveBack != 0 なら前半のみ
	float CurveVariance = 20.0f;	// 枝全体でのランダムな曲がり [deg] (隣接セグメントで低域通過した滑らかな揺れ)。
									// Level 1 / 2 と幹の分岐より上の片では、セグメントごとに交互に折る屈曲 ±min(0.3 x CurveVariance, 20) deg の振幅も決める
	float BaseOffset = 0.3f;		// 子を生やし始める位置 (0..1, 親の長さ比)
	int   Segments = 6;				// 長さ方向の分割数
	int   RadialSegments = 8;		// 周方向の分割数

	// ---- v2 で追加 ----
	float Tropism = 0.0f;			// 屈光性 [deg]: 水平な枝が全長で上 (+) / 下 (-) へ曲がる角。TropismScale 倍、(1 - t.y) 倍 (直立で 0、水平で 1、真下で 2 =
									// 垂れた枝ほど強い)。1 セグメント ±12 deg まで、天頂 / 真下を越えては曲げない
	float CurveBack = 0.0f;			// 後半セグメントの曲がり [deg] (0 = Curve を全長に配る / 非 0 = 前半 Curve、後半 CurveBack の S 字)
	float DownAngleShape = 0.0f;	// 位置依存の DownAngle [deg]: 親の付け根側 +DownAngleShape (水平寄り) 〜 先端側 -0.6 DownAngleShape (立つ)
	float TipBias = 0.0f;			// 子を親の先端側へ寄せる (0 = 等間隔。位置 t = u^(1/(1+TipBias)))
	bool  bAlternate = false;		// true: 子を螺旋でなく左右交互 (平面的な小枝) に出す。phi = ±(180 - Rotate) (Weber & Penn の負の nRotate)
	float RatioPower = 1.0f;		// 子の半径 = 親の半径 x (刈り込み前の子の長さ / 親の名目長)^RatioPower、Radius x 親で上限 (Weber & Penn nRatioPower。0.5..1.5)
	float TipRound = 0.8f;			// 先端キャップを半球にする割合 (0 = 平らな扇, 1 = 先端半径分の半球)
	float SegSplits = 0.0f;			// 1 セグメントあたりの二又分岐の期待数 (0..1)。誤差拡散で決定的、誤差はクローンへ引き継ぐ。
									// 幹の最初の片は BaseSplits の分岐が先 (それまで数えない)
	float SplitAngle = 30.0f;		// 分岐で脇の片が元の接線から開く角 [deg] (- 0.5 x 鉛直からの傾き、8..90)。主軸を継ぐ片は 0.25 倍だけ逸れ、
									// 幹の脇の片は前半で 0.5 倍だけ鉛直へ戻る (壺形)
	float SplitAngleVariance = 8.0f;
	float BarkUVScale = 1.0f;		// この階層の樹皮 UV タイル密度の倍率 (細い枝ほど大きくして模様を細かくする)
};

// ------------------------------------------------------------
//  FProceduralTreeParams
//  木 1 本分のパラメータ。既定値 (コンストラクタ) = Oak5m プリセット
//  (約 5 m の開けた場所のオーク、4 階層)。メンバ初期化子は名目値で、
//  コンストラクタが Oak5m 用に一部を上書きする (コメントの「Oak」が
//  実際の既定値。書いていないものは初期化子のまま)。
// ------------------------------------------------------------
struct FProceduralTreeParams
{
	static constexpr int MaxLevels = 4;

	unsigned int Seed = 1;
	int   Levels = 4;					// 階層数 (1 = 幹のみ .. MaxLevels)
	float TropismScale = 1.0f;			// 各階層の Tropism に掛ける倍率。旧キー Tree.Tropism (単位なし) は各階層の Tropism [deg] へ読み替える
	float BarkUVTilesPerMeter = 2.0f;	// 樹皮テクスチャのタイル数 [1/m] (根元の円周で整数に丸め、V も同じ密度にしてテクセルを正方にする)
	int   BaseSplits = 1;				// 幹が BaseOffset を越えた最初のセグメントで分岐する本数 (0 = 一本幹, 1 = 二又の主幹)

	// ---- 樹冠 (Crown) ----
	ECrownShape CrownShape = ECrownShape::Hemispherical;	// Level 1 の枝の長さ分布 (ShapeRatio)
	float PruneRatio = 0.6f;			// 樹冠包絡を越える枝を縮める割合 (0 = 刈らない .. 1 = 包絡に合わせる)。枝は消さず短くするだけ。Oak 0.5。
										// 0 より大きいと、包絡の天井より上へ反った枝と葉の間引きの範囲の外へ出た小枝 / 側枝の先も縮める
	float PruneWidth = 0.5f;			// 包絡の最大半径 (樹冠の高さに対する比)。Oak 0.8
	float PruneWidthPeak = 0.45f;		// 包絡が最大半径になる高さ (樹冠の高さ比 0..1)。Oak 0.62 (頂を平たく)
	float PrunePowerLow = 0.6f;			// 最大高さより上側の包絡の形 (べき)。Oak 0.7
	float PrunePowerHigh = 0.4f;		// 最大高さより下側の包絡の形 (べき)。Oak 0.3

	// ---- 幹 / 樹皮 (Trunk / Bark) ----
	float TrunkFlare = 0.5f;			// 根元の張り出し (幹の根元半径 x (1 + TrunkFlare)、TrunkFlareHeight で消える)。Oak 0.6
	float TrunkFlareHeight = 0.15f;		// 張り出しが消える高さ (幹の長さ比。BaseOffset より小さくすること)。Oak 0.10
	float GroundOffset = 0.0f;			// 幹の根元を原点より下げる量 [m] (地面に埋めて接地させる)
	float PipeExponent = 2.0f;			// パイプモデルの指数 (親^n = 先端^n + Σ子^n。2 = ダ・ヴィンチ則, 0 = 無効 = 線形テーパ)
	float JunctionSink = 0.6f;			// 子枝の根元を親の表面から内側へ沈める量 (子の半径比, 0 = 親の軸上)
	float JunctionCollar = 0.35f;		// 子枝の根元リングの膨らみ (半径倍率 - 1)
	float TwigRadiusThreshold = 0.012f;	// この根元半径 [m] (確定値) 未満の枝は小枝セクション (MaterialIndex 3, SDF 非寄与) へ。Oak 0.022 (2.2 cm 以上の主枝は溝のある樹皮)
	float BarkAOInner = 0.6f;			// 樹冠の内側にある枝の暗さ (頂点カラー倍率, 1 = 暗くしない。幹は樹冠の根元より下では暗くしない)。Oak 0.85
	float BarkMoss = 0.35f;				// 苔 (頂点カラーの黄緑化) の強さ (0..1): 上向きの面 + 幹の根元 (地面から ~1.1 m、木ごとの方位の側)。Oak 0.55

	// ---- 葉 (Leaves) ----
	bool  bGenerateLeaves = true;
	int   LeavesPerBranch = 8;			// 最終階層の枝 1 本あたりの葉の数 (LeafTerminalCount を含む)。Oak 6
	int   LeavesOnParentLevel = 4;		// 最終階層の 1 つ上の枝 (側枝の胴) にも付ける葉の数 (t >= 0.35 の範囲。ClustersPerTwig > 0 なら房 2 枚も t >= 0.55 に)
	float LeafSize = 0.11f;				// 葉の長さ [m] (幅は LeafAspect 倍)。Oak 0.10
	float LeafSizeVariance = 0.3f;		// LeafSize のランダム変動 (±比率、房カードの ClusterSize にも掛かる)。Oak 0.25
	float LeafAspect = 0.65f;			// 幅 / 長さ
	float LeafAngle = 40.0f;			// 枝の軸からの角度 [deg] (房カードは 0.8 倍)
	float LeafAngleVariance = 20.0f;
	float LeafTipBias = 1.5f;			// 葉を枝先へ寄せる (位置 t = u^(1/(1+LeafTipBias)))。Oak 1.0
	int   LeafTerminalCount = 2;		// 枝先 (t = 1) に接線方向へ付ける葉の数。最終階層の 2 つ上より上の枝 (主軸 / 主枝) の先端にも付ける。Oak 1
	float LeafBareTwigFraction = 0.08f;	// 葉 / 房を付けない小枝 (最終階層) の割合 (乱数は消費する)
	float LeafPetioleLength = 0.015f;	// 葉柄の長さ [m] (枝の表面から葉身の付け根まで。min(LeafPetioleLength, max(1 cm, 2 x 枝の半径))、側枝の胴の葉は一律 2 cm)
	float LeafDroop = 20.0f;			// 葉先側の法線を下へ倒す角 [deg] (ジオメトリは平面のまま、法線のみ)
	float LeafCup = 12.0f;				// 葉の左右の縁の法線を内側へ倒す角 [deg] (皿状の陰影)

	// ---- 葉の房カード (Clusters, MaterialIndex 2) ----
	int   ClustersPerTwig = 2;			// 最終階層の枝 1 本あたり、t >= 0.12 に置く房カード数 (0 = 房を全て使わない: 枝先 / 側枝の胴の房も)。Oak 5
	int   TerminalClusters = 1;			// 枝先に接線方向へ付ける房カード数 (k 枚目は接線まわりに k x 90 deg 回す)。小枝は 6 割だけ (LatticeHash)、
										// 主軸 / 主枝の先端には常に付ける
	float ClusterSize = 0.35f;			// 房カードの長さ [m]。運ぶ枝の長さで 0.4 倍 (4 cm 以下) .. 1 倍 (25 cm 以上)、Level 1 の枝ごと ±30 % / 小枝ごと ±20 %。Oak 0.30
	float ClusterAspect = 1.1f;			// 房カードの幅 / 長さ。Oak 1.25

	// ---- 葉のシェーディング (頂点カラー / 頂点法線で表現。照明側の葉の補正は Shader/SubstrateEvaluation.hlsl 冒頭) ----
	float LeafNormalBlend = 0.7f;		// 葉の法線を樹冠中心からの方向へ寄せる割合 (0 = カードの面法線。巻き順を守るため上限 0.85)
	float LeafNormalUpBias = 0.15f;		// 葉の法線に足す上向き成分
	float LeafAOInner = 0.4f;			// 樹冠の内側 (葉密度の高い所) の葉の暗さ (1 = 暗くしない。下側の暗さとの積は 0.45 を下回らない)。Oak 0.65
	float LeafAOBottom = 0.65f;			// 樹冠の下側の葉の暗さ。Oak 0.8
	float LeafAOFalloff = 0.6f;			// 葉密度グリッドによる遮蔽の強さ (セル断面 1 m² あたりの葉面積に対する光学的厚さ)
	float LeafHueVariance = 0.5f;		// 日向 = 黄緑 / 日陰 = 青緑 の色相ばらつき (0..1)。Oak 0.4
	float LeafBranchVariance = 0.15f;	// Level 1 の枝ごとの色相オフセット (同じ枝の葉は似た色になる)。Oak 0.3
	float LeafDeadFraction = 0.02f;		// 枯れ葉 (茶色く小さい、単葉のみ) の割合
	float LeafTintVariance = 0.3f;		// 葉ごとの明度ばらつき (±、頂点カラー。カードごとの値と 0.6 m 周期の空間ノイズを半々に混ぜる)。Oak 0.15
	float LeafErosion = 0.25f;			// 頂点 α による葉ごとの縁の削れ量 (0..0.3。α = 1 - LeafErosion x hash)
	XMFLOAT3 LeafTint = { 1.0f, 1.0f, 1.0f };	// 全ての葉に掛ける色 (季節 / 樹種の調整。テクスチャを作り直さずに済む)

	FProceduralTreeLevelParams Level[MaxLevels];

	FProceduralTreeParams();						// = Oak5m プリセット
	static FProceduralTreeParams MakePreset(ETreePreset Preset);	// Sapling = v1 の既定値 (Levels 3, 14 枝, 葉 0.22 m ...)

	// 生成アルゴリズム / 乱数の消費順の版 (SDF キャッシュ Saved/ProceduralMesh と衝突しないように)。v2 で固定し、
	// その後の改訂は ProceduralTree.cpp の GeneratorRevision (現在 3 = polish3) を上げてハッシュに混ぜる
	static constexpr unsigned int GeneratorVersion = 2;
	unsigned long long ComputeHash() const;		// GeneratorVersion / GeneratorRevision と上の全フィールド (使っている階層分) を混ぜる。LeafTint も含む

	// 範囲を丸め、枝数 (<= MaxTotalStems、分岐の片を正確に数える)、葉カード数 (<= MaxLeafCards、上界)、
	// 三角形数 (上界 <= 500k、ProceduralTree.cpp の MaxTreeTriangles。細い枝の分割から減らす) を収める
	void Sanitize();

	// Sanitize と同じ見積り (Details 表示 / テスト用)
	unsigned long long EstimateStems() const;
	unsigned long long EstimateCards() const;
};

// 生成結果の統計 (Details 表示用)
struct FProceduralTreeStats
{
	unsigned int NumStems = 0;			// 分岐したクローン片も 1 本と数える (Oak 既定 532)
	unsigned int NumTwigStems = 0;		// うち小枝セクション (MaterialIndex 3) へ出した本数 (Oak 既定 501)
	unsigned int NumLeaves = 0;			// 単葉カード数 (実際に出力した枚数。LeafBareTwigFraction 等で名目値より少ない)
	unsigned int NumClusters = 0;		// 房カード数
	unsigned int NumVertices = 0;
	unsigned int NumTriangles = 0;
	unsigned int NumTrianglesPerSection[4] = {};	// 樹皮 / 葉 / 房 / 小枝
	unsigned int NumDistanceFieldTriangles = 0;		// SDF ベイクに入る三角形数 (樹皮セクションのみ)
	float        GenerateMs = 0.0f;		// FProceduralTreeBuilder::Build の所要時間 [ms]
	float        RebuildMs = 0.0f;		// ATree::RebuildTree 全体 (生成 + GPU バッファ + SDF ベイク + BLAS) [ms]
};

// ------------------------------------------------------------
//  FProceduralTreeBuilder
// ------------------------------------------------------------
class FProceduralTreeBuilder
{
public:
	// セクション / マテリアルスロットの割り当て (ATree と ImGui も同じ定数を使う)
	static constexpr unsigned int BarkMaterialIndex = 0;		// 幹 / 太い枝: Opaque、閉じたチューブ、SDF に寄与
	static constexpr unsigned int LeafMaterialIndex = 1;		// 単葉カード: Masked / TwoSided / Substrate TwoSidedWrap、SDF 非寄与
	static constexpr unsigned int ClusterMaterialIndex = 2;		// 葉の房カード: 葉と同じマテリアル設定 + 房アトラス、SDF 非寄与
	static constexpr unsigned int TwigMaterialIndex = 3;		// 小枝 (根元半径 < TwigRadiusThreshold): Opaque、閉じたチューブだが
																//  64^3 のボクセルより細いので SDF 非寄与 (ベイク時間の節約)
	static constexpr unsigned int NumSections = 4;			// Sections は常に 4 本 (空も可) で MaterialIndex = 添字

	static constexpr unsigned int MaxLeafCards = 8000;		// 単葉 + 房カードの合計上限 (Sanitize が LeavesPerBranch (2 まで) -> ClustersPerTwig -> LeavesOnParentLevel
															//  -> 最深 Branches の順に減らす。EstimateCards は上界)
	static constexpr unsigned int MaxTotalStems = 20000;	// 枝の片 (分岐のクローン込み) の総数の上限。EstimateStems は分岐の予定を正確に数える
															//  (Sanitize が 最深の SegSplits -> BaseSplits -> 最深 Branches の順に減らす)

	// Params から木のメッシュを生成する。Sections は常に NumSections 本
	// (空セクションも MaterialIndex 付きで存在する)
	static FProceduralMeshData Build(const FProceduralTreeParams& Params,
		FProceduralTreeStats* OutStats = nullptr);
};

// ------------------------------------------------------------
//  FProceduralTreeTextures (ProceduralTreeTextures.cpp)
//  木用のテクスチャ (樹皮 / 葉 / 葉の房アトラス / 小枝の各
//  BaseColor / Normal / ARM、平坦法線) を CPU で生成して
//  Saved/ProceduralTexture/*.dds に書き出す。既存のマテリアル経路
//  (FAssetManager::LoadTexture -> DDS ローダ) をそのまま使うため、
//  ファイルとして用意する (ミップ付き RGBA8)。ファイルが揃っていれば
//  何もしない。
// ------------------------------------------------------------
class FProceduralTreeTextures
{
public:
	static constexpr unsigned int Version = 2;	// ファイル名に入る版 (v2: 葉 512² + 法線 / ARM、房アトラス 1024²、樹皮 1024²、小枝 256²)。v2 で固定したので、
												// その後に絵柄を変えた (樹皮 v6 = 溝の網、樫の葉) ときは古い Saved/ProceduralTexture/Tree*_v2.dds を消して作り直させる
	static constexpr float LeafCoverageClip = 0.3333f;	// 葉 / 房の α ミップのカバレッジ保存に使う clip 値 (Material 既定の OpacityMaskClipValue と一致させる)
	static constexpr float AtlasCellInset = 1.0f / 64.0f;	// 房アトラスのセル内 UV インセット (ジェネレータとベイカで共有)

	static std::string GetDirectory();
	static std::string GetBarkBaseColorPath();		// TreeBark_diff_v2 (1024², sRGB、縦の溝の網)
	static std::string GetBarkNormalPath();			// TreeBark_nor_dx_v2
	static std::string GetBarkARMPath();			// TreeBark_arm_v2
	static std::string GetLeafBaseColorPath();		// TreeLeaf_diff_v2 (512², 樫の葉、α = SDF ランプ、カバレッジ保存ミップ)
	static std::string GetLeafNormalPath();			// TreeLeaf_nor_dx_v2 (主脈 / 側脈のハイトフィールドから)
	static std::string GetLeafARMPath();			// TreeLeaf_arm_v2 (512²: AO / ラフネス / 0)
	static std::string GetClusterBaseColorPath();	// TreeCluster_diff_v2 (1024², 2x2 セル)
	static std::string GetClusterNormalPath();		// TreeCluster_nor_dx_v2
	static std::string GetClusterARMPath();			// TreeCluster_arm_v2
	static std::string GetTwigBaseColorPath();		// TreeTwig_diff_v2 (256², 皮目のある若い樹皮)
	static std::string GetTwigNormalPath();			// TreeTwig_nor_dx_v2
	static std::string GetTwigARMPath();			// TreeTwig_arm_v2
	static std::string GetFlatNormalPath();			// TreeFlat_nor_dx_v2 (4², 生成失敗時のフォールバック)

	// 無いファイルだけ生成する (初回のみ)。全て揃っていれば (生成済みなら) true
	static bool EnsureGenerated();
};
