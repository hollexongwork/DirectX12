#include "Main.h"
#include "ProceduralTree.h"

#include <random>
#include <cmath>
#include <cfloat>
#include <cstring>
#include <chrono>
#include <algorithm>

// ============================================================
//  内部ヘルパ
// ============================================================
namespace
{
	constexpr float PT_PI = 3.14159265358979323846f;
	constexpr float PT_TWO_PI = 6.28318530717958647692f;
	constexpr float PT_DEG2RAD = PT_PI / 180.0f;

	// 枝の最小半径 [m] (これ以下には細らせない) と先端半径の下限 (針のように光る先端を作らない)
	constexpr float MinStemRadius = 0.0015f;
	constexpr float MinTipRadius = 0.002f;
	// 小枝セクションでこの根元半径 [m] 未満の枝は周 4 分割以下 / 先細り補正なし / 襟なし
	constexpr float ThinTwigRadius = 0.004f;
	// 1 セグメントあたりの曲がりの上限 [deg] (曲がり + 揺れ + 屈光性の合計。隣接リングのフレームが捻れて帯が潰れないように) と屈光性だけの上限
	constexpr float MaxBendPerSegmentDeg = 40.0f;
	constexpr float MaxTropismPerSegmentDeg = 12.0f;
	// 樹冠包絡の下限 (最大半径に対する比。上端 / 根元で半径 0 に絞らない: 幹の先端から出る枝が切り株にならない) と、
	// 包絡の高さ比 h を 0..1 の外へ許す量 (幹の先端 (h = 1) から上へ伸びる枝も下限半径の中に収める)
	constexpr float PruneEnvelopeFloor = 0.25f;
	constexpr float PruneEnvelopeOvershoot = 0.12f;
	// 房カードを置き始める枝の長さ比 (枝先側 (1 - ClusterTMin) に配る)
	constexpr float ClusterTMin = 0.12f;
	// 最終階層の 1 つ上の枝 (側枝) の葉を置き始める長さ比 (側枝の胴の 65 % に葉を付ける: 主枝の途中が裸の棒にならない)
	constexpr float ParentLeafTMin = 0.35f;
	// 側枝 (最終階層の 1 つ上) の胴に付ける房カードの数と置き始める長さ比 (ClustersPerTwig > 0 のときだけ。1 枚 5 draw の規約は同じ)
	constexpr int   ClustersOnParentLevel = 2;
	constexpr float ParentClusterTMin = 0.55f;
	// 枝先の房を付ける小枝の割合 (LatticeHash(枝, 31) > 1 - 割合。draw は引くので回数は変わらない): 全ての小枝の先端が同じ「葉 + 房」の型にならない
	constexpr float TerminalClusterKeep = 0.6f;
	// 房カードの大きさを運ぶ枝 (片) の長さで縮める: ClusterMinLength 以下で ClusterMinScale 倍、ClusterFullLength 以上で 1 倍
	// (主軸の先の細い枝に付く数 cm の小枝が 30 cm の房を運ばない。小枝はどれも半径 1〜3 mm なので半径では区別できない。
	//  間引きは名目の大きさで判定するので枚数は変わらない)
	constexpr float ClusterMinScale = 0.4f;
	constexpr float ClusterMinLength = 0.04f;
	constexpr float ClusterFullLength = 0.25f;
	// 二又分岐の非対称: 主軸を継ぐ片は分岐角 x MainSplitDeviation だけ逸れ、脇の片は分岐角いっぱいに開く。
	// 脇の片の断面の取り分は LatticeHash で SideShareMin..SideShareMax (主軸は残り)、長さは SideLengthMin..1 倍
	constexpr float MainSplitDeviation = 0.25f;
	constexpr float SideShareMin = 0.30f;
	constexpr float SideShareMax = 0.45f;
	constexpr float SideLengthMin = 0.72f;
	// 幹の傾きの上限 [deg] (傾き = Level[0].DownAngle: 幹の「親の軸」は鉛直。方位は LatticeHash で乱数は引かない)
	constexpr float MaxTrunkLeanDeg = 30.0f;
	// 分岐で切られた子の無い片 (幹の根元の片など) の半径: 線形テーパと一定 Base の配合 (0 = 線形テーパ, 1 = 一定)。幹が分岐の手前で ~15 % 細る
	constexpr float CutPieceHold = 0.4f;
	// パイプモデルありのテーパの形の指数 (半径 = 1 - (1 - Taper) x^p、x = 元の枝の長さ比): 根元側は太いまま、先の方で細る
	constexpr float PipeTaperPower = 1.5f;
	// Level 1 の子 (主枝) の付け根の位置を揺らす量 (枠の間隔比 ±、LatticeHash): 主枝が等間隔の傘の骨にならず、群れと隙間ができる
	constexpr float LimbSlotJitter = 0.4f;
	// 樹冠包絡の外の葉 / 房を間引く余裕 [m] (カードの中心が 1.1 R(h) + LeafCullMargin / PruneRatio より外なら出さない。刈り込みが残す
	// はみ出し (1 - PruneRatio) 倍の先の小枝 + カード 1 枚分。以前の 1.1 R / PruneRatio は上端 (R が小さい) で枝先の葉を全て落とし、裸の小枝が突き出ていた)
	constexpr float LeafCullMargin = 0.5f;
	// 房カードと混ぜるときに残す単葉の割合 (枝先でない単葉の名目枚数に掛ける)
	constexpr float LeafMixFactor = 0.85f;

	// ---- polish3: 骨格 / 輪郭 ----
	// Level 2 以下の子の付け根の位置の揺れ (枠の間隔比 ±、LatticeHash): 小枝が等間隔の櫛 / 羊歯の葉にならない
	constexpr float ChildSlotJitter = 0.4f;
	// Level 1 の付け根を段 (LimbTiers 段) に寄せる強さ (0 = 寄せない .. 1 = 段の中心に揃う): 主枝が段ごとに群れ、段の間に空が抜ける
	constexpr int   LimbTiers = 3;
	constexpr float LimbTierPull = 0.45f;
	// Level 1 の子 (主枝) の根元半径の下限 (付け根での親の半径比): 樹冠の中に太い骨格の枝が見える (細い棒の網にしない)
	constexpr float LimbMinRadiusRatio = 0.45f;
	// 屈曲 (仮軸分枝のジグザグ): Level 1 / 2 と幹の分岐より上の片で、セグメントごとに ±LimbKinkFactor x CurveVariance [deg] を交互に折る
	// (振幅 / 面は LatticeHash。交互なので枝全体の向き (Curve の予定) は保たれる)
	constexpr float LimbKinkFactor = 0.30f;
	// 幹の脇の片 (二又の脇の主幹) を前半で鉛直へ戻す量 (分岐角比): 外へ開いてから立ち上がる壺形
	constexpr float SideCloneUpright = 0.5f;
	// 二又の分岐点のまわりに Level 1 の子を置かない幅 [m] と、分岐点から ForkDownRange [m] 以内の子の下向き角の上限 [deg]
	constexpr float ForkClearance = 0.25f;
	constexpr float ForkDownRange = 0.5f;
	constexpr float ForkMaxDownDeg = 70.0f;
	// 主軸 (幹の分岐した片) の先端を樹冠の上端 yT から下げる量 (ClusterSize 比): 主軸の先が葉の外へ裸で突き出ない
	constexpr float LeaderTopMargin = 0.5f;
	// 房カードの大きさのばらつき: Level 1 の枝ごと ±LimbClusterVariance、小枝ごとの型 (モチーフ) ±TwigClusterVariance (どちらも LatticeHash)
	constexpr float LimbClusterVariance = 0.30f;
	constexpr float TwigClusterVariance = 0.20f;
	// 枝の途中の房カードを面法線まわりに回す最大角 [deg] (付け根を中心に。LatticeHash): 同じ房の扇が同じ向きで並ばない
	constexpr float ClusterRollDeg = 25.0f;
	// 最終階層とその 1 つ上の枝 (小枝 / 側枝) の先端は葉の間引きの範囲から TwigTipCardReach [m] 内側で切る (枝先のカードの中心が範囲に残り、
	// 葉の無い鞭が樹冠の外へ突き出ない)。根元から範囲の外の枝は長さ比 BareTwigKeep まで縮める
	constexpr float TwigTipCardReach = 0.2f;	// TwigTrimScale: 範囲の R(h) の倍率 (葉の間引きの 1.1 より少し内側)
	constexpr float TwigTrimScale = 1.05f;
	constexpr float BareTwigKeep = 0.15f;
	// 樹冠の天井 / 小枝の先の切り詰めで縮めた枝の 1 セグメントの下限 [m]
	constexpr float MinTrimSegment = 0.012f;

	// ---- polish3: 樹皮 ----
	// 根元の苔: 地面から BasalMossHeight [m] まで、種ごとの方位 (LatticeHash) の側ほど濃く、まだら (Patch の SmoothStep)
	constexpr float BasalMossHeight = 1.1f;
	// 生成規則の改訂番号 (ComputeHash に混ぜる。ヘッダの GeneratorVersion を変えずに Saved/ProceduralMesh の SDF キャッシュを無効にする)。
	// 3 = polish3 (屈曲 / 天井 / 小枝の切り詰め / 枠の揺れ / 樹皮タイルの継承)
	constexpr int GeneratorRevision = 3;
	// Sanitize が三角形数をこの値以下に収める (周方向の分割 -> 長さ方向の分割の順に最深階層から減らす)
	constexpr double MaxTreeTriangles = 500000.0;

	inline XMVECTOR Load3(const XMFLOAT3& V) { return XMLoadFloat3(&V); }

	inline XMFLOAT3 Store3(FXMVECTOR V)
	{
		XMFLOAT3 out;
		XMStoreFloat3(&out, V);
		return out;
	}

	inline float Clamp(float v, float lo, float hi) { return (v < lo) ? lo : (v > hi) ? hi : v; }
	inline int   ClampInt(int v, int lo, int hi) { return (v < lo) ? lo : (v > hi) ? hi : v; }
	inline float Lerp(float a, float b, float t) { return a + (b - a) * t; }
	inline float SmoothStep(float e0, float e1, float x)
	{
		const float t = Clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
		return t * t * (3.0f - 2.0f * t);
	}

	inline float LengthSq3(FXMVECTOR V) { return XMVectorGetX(XMVector3LengthSq(V)); }
	inline float Dot3(FXMVECTOR A, FXMVECTOR B) { return XMVectorGetX(XMVector3Dot(A, B)); }

	// 長さ 0 に近いベクトルは Fallback を返す正規化
	inline XMVECTOR SafeNormalize(FXMVECTOR V, FXMVECTOR Fallback)
	{
		return (LengthSq3(V) < 1.0e-16f) ? Fallback : XMVector3Normalize(V);
	}

	// D に直交する任意の単位ベクトル
	inline XMVECTOR AnyPerpendicular(FXMVECTOR D)
	{
		const XMVECTOR up = (fabsf(XMVectorGetY(D)) < 0.99f)
			? XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f)
			: XMVectorSet(1.0f, 0.0f, 0.0f, 0.0f);
		return SafeNormalize(XMVector3Cross(up, D), XMVectorSet(1.0f, 0.0f, 0.0f, 0.0f));
	}

	// V から Axis 成分を除いて正規化する (Gram-Schmidt)。退化時は Axis に直交する任意ベクトル
	inline XMVECTOR Orthonormalize(FXMVECTOR V, FXMVECTOR Axis)
	{
		const XMVECTOR projected = XMVectorSubtract(V, XMVectorScale(Axis, Dot3(V, Axis)));
		return (LengthSq3(projected) < 1.0e-12f) ? AnyPerpendicular(Axis) : XMVector3Normalize(projected);
	}

	// 単位軸 Axis まわりに Radians 回転する
	inline XMVECTOR RotateAxis(FXMVECTOR V, FXMVECTOR Axis, float Radians)
	{
		if (fabsf(Radians) < 1.0e-7f) return V;
		return XMVector3Rotate(V, XMQuaternionRotationAxis(Axis, Radians));
	}

	// 単位ベクトル N を、Ref との内積が MinDot 以上になるまで Ref 側へ寄せる
	// (N + a Ref の Ref 方向余弦が MinDot になる a の閉形式)。曲げた葉の法線が
	// 面から 90 deg 近くまで離れて巻き順判定 (頂点法線の和) を乱すのを防ぐ
	inline XMVECTOR ClampToHemisphere(FXMVECTOR N, FXMVECTOR Ref, float MinDot)
	{
		const float d = Dot3(N, Ref);
		if (d >= MinDot) return N;
		const float a = -d + MinDot * sqrtf(fmaxf(1.0f - d * d, 0.0f) / (1.0f - MinDot * MinDot));
		return SafeNormalize(XMVectorAdd(N, XMVectorScale(Ref, a)), Ref);
	}

	// From を To へ運ぶ最小回転で N を平行移動する (回転最小フレーム)。
	// 戻り値は To に直交するよう再直交化済み
	inline XMVECTOR TransportNormal(FXMVECTOR N, FXMVECTOR From, FXMVECTOR To)
	{
		const XMVECTOR axis = XMVector3Cross(From, To);
		const float s = sqrtf(LengthSq3(axis));
		const float c = Dot3(From, To);
		if (s < 1.0e-6f)
		{
			return Orthonormalize(N, To);
		}

		const XMVECTOR unitAxis = XMVectorScale(axis, 1.0f / s);
		const float angle = atan2f(s, c);

		// 回転の向きは From を To へ運ぶ方を選ぶ (四元数の符号規約に依存しない)
		const XMVECTOR a = RotateAxis(From, unitAxis, angle);
		const XMVECTOR b = RotateAxis(From, unitAxis, -angle);
		const float signedAngle = (Dot3(a, To) >= Dot3(b, To)) ? angle : -angle;

		return Orthonormalize(RotateAxis(N, unitAxis, signedAngle), To);
	}

	// 接線 T を鉛直面内で Radians だけ曲げる (正 = 上向き / 負 = 下向きになる方の回転を選ぶ)。
	// T が鉛直なら FallbackAxis (T に直交する単位ベクトル) まわりに回す。Curve / CurveBack / 刈り込みの先端予測で共用
	inline XMVECTOR BendTowardUp(FXMVECTOR T, FXMVECTOR FallbackAxis, float Radians)
	{
		if (fabsf(Radians) <= 1.0e-6f) return T;
		const XMVECTOR up = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
		XMVECTOR axis = XMVector3Cross(T, up);
		axis = (LengthSq3(axis) < 1.0e-6f) ? FallbackAxis : XMVector3Normalize(axis);

		XMVECTOR candidate = RotateAxis(T, axis, Radians);
		if ((Dot3(candidate, up) - Dot3(T, up)) * Radians < 0.0f)
		{
			candidate = RotateAxis(T, axis, -Radians);
		}
		return candidate;
	}

	// 単位接線 T から天頂 (bUp) / 真下までの角 [rad]。水平成分から atan2 で求める (acos(t.y) は鉛直の近くで桁落ちして ~0.02 deg 残り、
	// 飽和させた枝がその分だけ毎セグメント鉛直から外れて戻る)
	inline float AngleToVertical(FXMVECTOR T, bool bUp)
	{
		const float tx = XMVectorGetX(T), ty = XMVectorGetY(T), tz = XMVectorGetZ(T);
		return atan2f(sqrtf(tx * tx + tz * tz), bUp ? ty : -ty);
	}

	// From から To への回転角が MaxRadians を超えるなら、From を To の側へ MaxRadians だけ回したものを返す (1 セグメントの曲がりの上限)。
	// To が From の真後ろ (回転軸が定まらない) なら FallbackAxis (From に直交する単位ベクトル) まわりに回す
	inline XMVECTOR LimitBend(FXMVECTOR From, FXMVECTOR To, FXMVECTOR FallbackAxis, float MaxRadians)
	{
		if (Dot3(From, To) >= cosf(MaxRadians)) return To;
		const XMVECTOR axis = SafeNormalize(XMVector3Cross(From, To), FallbackAxis);
		const XMVECTOR a = RotateAxis(From, axis, MaxRadians);
		const XMVECTOR b = RotateAxis(From, axis, -MaxRadians);
		return (Dot3(a, To) >= Dot3(b, To)) ? a : b;
	}

	// Weber & Penn の ShapeRatio: 樹冠の形ごとの Level 1 の枝の長さ倍率。R = 1 が樹冠の根元 (BaseOffset)、0 が幹の先端
	inline float ShapeRatio(ECrownShape Shape, float R)
	{
		const float r = Clamp(R, 0.0f, 1.0f);
		switch (Shape)
		{
		case ECrownShape::Conical:            return 0.2f + 0.8f * r;
		case ECrownShape::Spherical:          return 0.2f + 0.8f * sinf(PT_PI * r);
		case ECrownShape::Hemispherical:      return 0.2f + 0.8f * sinf(0.5f * PT_PI * r);
		case ECrownShape::TaperedCylindrical: return 0.5f + 0.5f * r;
		case ECrownShape::Flame:              return (r <= 0.7f) ? r / 0.7f : (1.0f - r) / 0.3f;
		case ECrownShape::InverseConical:     return 1.0f - 0.8f * r;
		case ECrownShape::TendFlame:          return (r <= 0.7f) ? 0.5f + 0.5f * r / 0.7f : 0.5f + 0.5f * (1.0f - r) / 0.3f;
		case ECrownShape::Cylindrical:
		default:                              return 1.0f;
		}
	}

	// ---- 二又分岐 (Weber & Penn の splits) の予定。乱数は使わない: 幹の BaseSplits + SegSplits の誤差拡散 ----
	//  片 (元の枝、または分岐で生まれたクローン) のセグメント s ごとに: 幹の最初の片で (s + 1) / SegmentsTotal >= BaseOffset となる
	//  最初のセグメントなら BaseSplits 本、それ以外は err += SegSplits; n = floor(err + 0.5); err -= n。
	//  幹の最初の片は根元の分岐 (BaseSplits) が済むまで SegSplits を数えない (Weber & Penn: 幹の最初の分岐は必ず根元の分岐で、
	//  樹冠の根元 BaseOffset は常に最初の片の中にある。そうしないと BaseOffset より下で切れた幹のクローンが BaseSplits を失う)。
	//  n > 0 で s が最後のセグメントでなければそこで切り、n + 1 本のクローンが残りを継ぐ。誤差 err はクローンへ引き継ぐ
	//  (枝 1 本の経路に沿った分岐数 ≒ SegSplits x セグメント数。片ごとに 0 から始めると SegSplits 0.5 以上が毎セグメント分岐になる)。
	//  戻り値 = 分岐するセグメント番号 (-1 = 分岐しない)、OutSplits = 分岐数、InOutError = 誤差の残り。
	//  GenerateStem と EstimateStems が同じ関数を使うので、見積りは片の数を正確に数える
	inline int FindSplitSegment(const FProceduralTreeLevelParams& lp, int BaseSplits, bool bTrunkBase, int Segments, int SegmentsTotal,
		float& InOutError, int& OutSplits)
	{
		const float segSplits = Clamp(lp.SegSplits, 0.0f, 1.0f);
		const float baseOffset = Clamp(lp.BaseOffset, 0.0f, 0.95f);
		bool bBasePending = bTrunkBase;
		for (int s = 0; s < Segments; ++s)
		{
			int n = 0;
			if (bBasePending)
			{
				if (static_cast<float>(s + 1) / static_cast<float>(SegmentsTotal) >= baseOffset)
				{
					bBasePending = false;
					n = BaseSplits;
				}
			}
			else
			{
				InOutError += segSplits;
				n = static_cast<int>(floorf(InOutError + 0.5f));
				InOutError -= static_cast<float>(n);
			}
			if (n > 0 && s < Segments - 1)
			{
				OutSplits = n;
				return s;
			}
		}
		OutSplits = 0;
		return -1;
	}

	// 1 本の枝が分岐で何片になるか (FindSplitSegment の再帰。クローンは同じ残りセグメント数と誤差を持つので全て同じ形)
	inline unsigned long long CountPieces(const FProceduralTreeLevelParams& lp, int BaseSplits, bool bTrunkBase, int Segments, int SegmentsTotal, float Error)
	{
		int splits = 0;
		const int s = FindSplitSegment(lp, BaseSplits, bTrunkBase, Segments, SegmentsTotal, Error, splits);
		if (s < 0) return 1;
		return 1ULL + static_cast<unsigned long long>(splits + 1) * CountPieces(lp, BaseSplits, false, Segments - s - 1, SegmentsTotal, Error);
	}

	// 片の取り分 [Lo, Hi] (元の枝の子 / 葉の配分区間 0..1) に入る個数。区間の端で丸めるので、区間を分け合う片の合計は常に Count
	inline int SharedCount(int Count, float Lo, float Hi)
	{
		const float n = static_cast<float>(Count);
		return static_cast<int>(floorf(n * Hi + 0.5f)) - static_cast<int>(floorf(n * Lo + 0.5f));
	}

	// ---- 決定的な乱数 (mt19937 + 自前の一様変換) ----
	struct FTreeRandom
	{
		std::mt19937 Engine;

		explicit FTreeRandom(unsigned int Seed) : Engine(Seed) {}

		// [0, 1)
		float Unit() { return static_cast<float>(Engine() >> 8) * (1.0f / 16777216.0f); }
		float Range(float Min, float Max) { return Min + (Max - Min) * Unit(); }
		float Signed(float Amplitude) { return Range(-Amplitude, Amplitude); }
	};

	// ---- 乱数を引かない決定的な選択 (色味 / アトラスのセル等) 用のハッシュ。ProceduralTreeTextures.cpp と同じ式 ----
	inline float LatticeHash(int X, int Y, unsigned int Seed)
	{
		unsigned int n = static_cast<unsigned int>(X) * 374761393u
			+ static_cast<unsigned int>(Y) * 668265263u
			+ Seed * 1013904223u;
		n = (n ^ (n >> 13)) * 1274126177u;
		n = n ^ (n >> 16);
		return static_cast<float>(n) * (1.0f / 4294967296.0f);
	}

	// 乱数サブストリームのシード (FNV-1a 32bit を A, B, C の 12 バイトに掛ける)。
	// 幹 = HashCombine(Params.Seed, 0, 0)、子 / クローン = HashCombine(親の Seed, 子番号 + 1, 生まれる枝自身の Level)。
	// 子番号はクローンを先に数える。第 3 引数はクローンなら親と同じ Level、通常の子なら親 + 1 (T12 もこの規約で)
	inline unsigned int HashCombine(unsigned int A, unsigned int B, unsigned int C)
	{
		const unsigned int words[3] = { A, B, C };
		const unsigned char* bytes = reinterpret_cast<const unsigned char*>(words);
		unsigned int hash = 2166136261u;
		for (size_t i = 0; i < sizeof(words); ++i)
		{
			hash ^= bytes[i];
			hash *= 16777619u;
		}
		return hash;
	}

	// ---- 枝 (ステム) の骨格 ----
	struct FStemPoint
	{
		XMFLOAT3 Position;
		XMFLOAT3 Tangent;	// 枝の進行方向 (単位)
		XMFLOAT3 Normal;	// フレームの X 軸 (Tangent に直交する単位)
		float    Radius;
		float    Distance;	// 根元からの距離 [m]
	};

	struct FStem
	{
		int   Level = 0;
		float Length = 0.0f;
		float BaseRadius = 0.0f;
		float TipRadius = 0.0f;
		std::vector<FStemPoint> Points;

		int   ParentStem = -1;			// m_Stems の添字 (-1 = 幹)
		float AttachT = 0.0f;			// 親の長さ比 (0..1)
		float ParentShade = 1.0f;		// 付け根での親の頂点カラー明度 (継ぎ目を隠す)
		int   SegmentsOwned = 0;		// この片が持つセグメント数 (分岐で切られると減る)
		int   SegmentsTotal = 0;		// 元の枝の Segments (子の数の配分に使う)
		bool  bClone = false;			// 分岐で生まれた続き (BaseOffset = 0)
		std::vector<std::pair<float, float>> ChildAttach;	// (t, 子の根元半径 (骨格パスの値)) パイプモデル用
		std::vector<int> ChildStems;	// 子枝の m_Stems 添字 (付け根まわりの親の暗化用)
		int   AncestorL1 = -1;			// 色相オフセット用の Level 1 の祖先 (自身が Level 1 ならその添字)
		float UVOffsetV = 0.0f;			// 樹皮 V の開始オフセット [m] (LatticeHash(Seed, 5) x タイル 1 枚分。AssignRadii で確定)
		unsigned int Seed = 0;			// この枝のサブストリーム
		XMFLOAT3 RootRadialDir = { 1.0f, 0.0f, 0.0f };	// 付け根での親の軸から外向きの方向 (接合部の沈み込みの向き)
		float CosDown = 1.0f;			// 付け根での親の接線と枝の方向の余弦 (沈み込み量の |cos(down)| 項)
		float Collar = 0.0f;			// この枝の襟の実効倍率 - 1 (根元リングが親の中に収まるよう JunctionCollar を抑えたもの。AssignRadii で確定)

		// ---- 分岐 (T12): 元の枝の中でのこの片の位置と取り分 ----
		int   SegmentStart = 0;			// 元の枝のセグメント番号で測ったこの片の根元 (クローン以外は 0)
		float ShareLo = 0.0f;			// 元の枝の子 / 葉の配分区間 [ShareLo, ShareEnd] (0..1)。分岐するごとにクローンが残りを等分する
		float ShareEnd = 1.0f;
		float DistanceOffset = 0.0f;	// 元の枝の根元からこの片の根元までの距離 [m] (幹の接地ランプをクローンへ続ける)
		float CloneRadiusRatio = 0.0f;	// クローン: 根元半径 / 親の片の先端半径 = 断面の取り分^(1 / PipeExponent)。AssignRadii が親の確定半径に合わせ直す

		// ---- テーパの形: 元の枝の長さ比 x で 1 - (1 - Taper) x^TaperPow。この片は x = TaperX0..TaperX1 ----
		float TaperX0 = 0.0f;
		float TaperX1 = 1.0f;
		float TaperPow = 1.0f;

		// ---- polish3: 樹皮 UV のタイル (AssignRadii で確定)。分岐のクローンは親の片のタイル数 / V 密度を継ぐ (溝が分岐を通って続く) ----
		float TilesAround = 1.0f;
		float VScale = 1.0f;
	};

	// 片の中での半径の補間係数 (根元 0 -> 先端 1): 元の枝の長さ比 X でのテーパ曲線を片の両端で正規化したもの (TaperPow 1 なら線形 = 片の長さ比)
	inline float TaperShape(const FStem& Stem, float X)
	{
		const float a = powf(Stem.TaperX0, Stem.TaperPow);
		const float b = powf(Stem.TaperX1, Stem.TaperPow);
		if (b - a < 1.0e-6f)
		{
			return Clamp((X - Stem.TaperX0) / fmaxf(Stem.TaperX1 - Stem.TaperX0, 1.0e-6f), 0.0f, 1.0f);
		}
		return Clamp((powf(Clamp(X, 0.0f, 1.0f), Stem.TaperPow) - a) / (b - a), 0.0f, 1.0f);
	}

	struct FStemSample
	{
		XMVECTOR Position;
		XMVECTOR Tangent;
		XMVECTOR Normal;
		float    Radius;
		float    Distance;
	};

	// 長さ比 T (0..1) の位置で骨格を補間する
	FStemSample SampleStem(const FStem& Stem, float T)
	{
		const int count = static_cast<int>(Stem.Points.size());
		const float f = Clamp(T, 0.0f, 1.0f) * static_cast<float>(count - 1);
		int i = static_cast<int>(f);
		if (i > count - 2) i = count - 2;
		if (i < 0) i = 0;
		const float a = (count > 1) ? Clamp(f - static_cast<float>(i), 0.0f, 1.0f) : 0.0f;

		const FStemPoint& p0 = Stem.Points[i];
		const FStemPoint& p1 = Stem.Points[(count > 1) ? i + 1 : i];

		FStemSample s;
		s.Position = XMVectorLerp(Load3(p0.Position), Load3(p1.Position), a);
		s.Tangent = SafeNormalize(XMVectorLerp(Load3(p0.Tangent), Load3(p1.Tangent), a), Load3(p0.Tangent));
		s.Normal = Orthonormalize(XMVectorLerp(Load3(p0.Normal), Load3(p1.Normal), a), s.Tangent);
		s.Radius = Lerp(p0.Radius, p1.Radius, a);
		s.Distance = Lerp(p0.Distance, p1.Distance, a);
		return s;
	}

	// 方位の基準面法線 planeN (葉の方位 / 左右交互の子の面): RMF 法線 (枝に沿って連続) と「枝から見た上」
	// (cross(T, cross(up, T))) を 0.6 で混ぜて接線に直交化したもの (空寄り。鉛直な枝では RMF 法線そのまま)
	inline XMVECTOR PlaneNormal(const FStemSample& S)
	{
		const XMVECTOR up = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
		const XMVECTOR skyward = SafeNormalize(XMVector3Cross(S.Tangent, XMVector3Cross(up, S.Tangent)), S.Normal);
		return Orthonormalize(SafeNormalize(XMVectorLerp(S.Normal, skyward, 0.6f), S.Normal), S.Tangent);
	}

	// 子枝 / 分岐クローンの生成要求 (親の骨格から先に決めてから再帰する)
	struct FChildSpec
	{
		XMFLOAT3 Origin;
		XMFLOAT3 Direction;
		XMFLOAT3 Normal;
		float    Length;
		float    Radius;
		int      Level;				// クローンは親と同じ階層
		int      ParentStem;		// m_Stems の添字 (-1 = 幹)
		float    AttachT;			// 親の長さ比 (0..1)
		float    ParentShade;		// 付け根での親の頂点カラー明度
		float    ParentRadius;		// 付け根での親の半径
		int      SegmentsRemaining;	// クローン: 残りセグメント数 / 通常の子: Level の Segments
		int      SegmentsTotal;		// 元の枝の Segments
		bool     bClone;			// 分岐で生まれた続き (BaseOffset = 0、BaseSplits を再適用しない)
		unsigned int Seed;			// この枝のサブストリーム
		XMFLOAT3 RadialDir;			// 付け根での親の軸から外向きの方向 (通常の子のみ。接合部の沈み込みの向き)
		float    CosDown;			// 親の接線と子の方向の余弦

		// ---- 分岐 (T12) ----
		int      SegmentStart;		// 元の枝のセグメント番号で測ったこの片の根元 (クローン以外は 0)
		float    ShareLo;			// 元の枝の子 / 葉の配分区間 (通常の子は 0..1、クローンは親の片の残りを等分した区間)
		float    ShareHi;
		float    SplitError;		// SegSplits の誤差拡散の残り (クローンが引き継ぐ。通常の子は 0)
		float    CloneRadiusRatio;	// クローン: 根元半径 / 親の片の先端半径
		float    DistanceOffset;	// 元の枝の根元からこの片の根元までの距離 [m]
		float    KeepFromVertical;	// クローン: 最初のセグメントの系統的な曲がり (Curve / Tropism) が鉛直の手前で止まる角 [rad] (= 分岐角。通常の子は 0)
		float    TipScale;			// 刈り込みで縮めた長さ / 元の長さ (0..1)。Taper に掛けて先端を細らせる (短くした枝が太い切り株で終わらない)。クローンは引き継ぐ
		float    UprightBend;		// polish3: 幹の脇の片が前半のセグメントで鉛直へ戻る合計角 [rad] (それ以外は 0)
		unsigned int KinkSeed;		// polish3: 屈曲の振幅 / 面を選ぶ鍵 (元の枝の Seed。クローンは引き継ぐ: 分岐しても屈曲の予定が続く)
	};

	// 葉 / 房カード 1 枚。骨格パスは乱数で「枝上の位置 / 方位 / 角度 / 捻り / 大きさ」(配置パラメータ) だけを決め、
	// 出力パスの ResolveLeafSpecs が半径の確定 (T9 AssignRadii) 後に Origin / Dir / Side / FaceN を解く (乱数は引かない)。
	// 骨格パスの仮の半径で原点を焼き込むと、パイプモデルで太った枝に葉柄の付け根が埋まるため
	struct FLeafSpec
	{
		// ---- 配置パラメータ (骨格パスで確定) ----
		float    T;				// 枝の長さ比 (枝先 = 1)
		float    Phi;			// 基準面法線 planeN から接線まわりの方位 [rad] (枝先の房: RMF 法線からの +U の回転 j x 90 deg)
		float    Angle;			// 枝の接線からの角度 [rad] (枝先の葉 / 房は接線寄りの固定比で、使わない)
		float    Roll;			// 葉軸まわりの捻り [rad]
		float    Size;
		float    HalfWidth;
		float    U0, U1, U2;	// 色 / 削れ / 枯れ用の決定的な値 (LatticeHash(Index, k, Seed))
		int      Index;			// stemIndex x 256 + スロット (トグルで並びが変わっても安定なハッシュの種)。
								// スロット: 単葉 j (0..63) / 親階層の葉 64 + j (T4) / 房 128 + k (T8)。重ねない
		int      Stem;			// 所属する枝 (m_Stems 添字)
		int      AtlasCell;		// 房: 0..3 / 単葉: 0
		bool     bCluster;
		bool     bTerminal;
		bool     bParentLevel;	// 最終階層の 1 つ上の枝の葉 (葉柄は一律 2 cm)
		bool     bTipBouquet;	// 上位階層 (最終階層の 2 つ上まで) の枝先の葉 / 房 (切られていない片の先端。主軸 / 主枝の先端が裸で終わらない)

		// ---- 出力パスで解く (ResolveLeafSpecs) ----
		XMFLOAT3 Origin;
		XMFLOAT3 Dir;		// 付け根 -> 葉先 (単位)
		XMFLOAT3 Side;		// +U 方向 (単位)
		XMFLOAT3 FaceN;		// カードの面法線 (単位)
		bool     bPruned;	// カードの中心が樹冠包絡の 1.1 / PruneRatio 倍の外 (PruneRatio > 0 のとき): 樹冠の統計にも出力にも入れない
		bool     bMirror;	// 房: アトラスのセルを左右反転して貼る (LatticeHash(Index, 4) > 0.5。表裏を入れ替えて接線の規約を守る)
	};

	// ============================================================
	//  FTreeGenerator
	//  (A) 骨格パス: GenerateStem が枝ごとのサブストリームで乱数を消費し、
	//      FStem を m_Stems (親が先、深さ優先)、葉の仕様を m_LeafSpecs に積む。
	//  (B) 出力パス: 骨格が揃ってから樹冠の統計 (ComputeCrownStats: 中心 / 葉密度グリッド) を取り、
	//      EmitBark / EmitLeaves でジオメトリを出す。乱数は引かない (見た目の選択は LatticeHash)。
	//  乱数の規約: 新しい draw は枝自身のストリーム内でだけ、かつ無条件に
	//  (回数が整数パラメータだけで決まるように) 引く。
	// ============================================================
	class FTreeGenerator
	{
	private:
		const FProceduralTreeParams& m_Params;

		std::vector<FStem>     m_Stems;		// 添字 = 枝 ID (親が先)
		std::vector<FLeafSpec> m_LeafSpecs;

		FProceduralMeshSection m_Bark;		// セクション 0: 幹 / 太い枝
		FProceduralMeshSection m_Leaves;	// セクション 1: 単葉カード
		FProceduralMeshSection m_Clusters;	// セクション 2: 葉の房カード
		FProceduralMeshSection m_Twig;		// セクション 3: 小枝 (根元半径 < TwigRadiusThreshold)
		unsigned int           m_NumTwigStems = 0;
		unsigned int           m_NumLeaves = 0;
		unsigned int           m_NumClusters = 0;

		// ---- 樹冠の統計 (出力パスの陰影用。骨格パスの後に ComputeCrownStats で確定する) ----
		static constexpr int   CrownGridSize = 24;		// 葉密度グリッドの一辺のセル数
		static constexpr int   OcclusionSteps = 5;		// 遮蔽の探索歩数 (1 歩 = 1 セル)
		static constexpr int   OcclusionDirs = 6;		// 真上 + 仰角 50 deg を方位 72 deg 刻みで 5 本
		static constexpr float MinLeafNormalDot = 0.12f;	// 葉の頂点法線と面法線の内積の下限 (巻き順の保証 > 0.1)
		static constexpr float TwigNormalBlend = 0.3f;	// 小枝の法線を樹冠の外向きへ寄せる割合 (x LeafNormalBlend)
		XMFLOAT3 m_CrownCenter = { 0.0f, 0.0f, 0.0f };	// 葉面積で重み付けした葉の中心 C
		XMFLOAT3 m_CrownRadius = { 1.0f, 1.0f, 1.0f };	// 軸ごとの半径 R (グリッドの範囲 = C ± R)
		XMFLOAT3 m_CrownCellSize = { 1.0f, 1.0f, 1.0f };
		float    m_CrownMinY = 0.0f;
		float    m_CrownHeight = 1.0f;
		std::vector<float>     m_CrownTau;		// セルごとの光学的厚さ (LeafAOFalloff x 葉面積 / セル断面)
		FProceduralNoise       m_Noise;			// 苔のまだら (Params.Seed)
		XMFLOAT3               m_MossDir = { -0.5f, 0.0f, 0.8660254f };	// 根元の苔が付く側 (水平、方位は LatticeHash(0, 78, Seed))
		float                  m_TrunkLength = 1.0f;	// 幹の名目長 [m] (根元の張り出しの高さ比 TrunkFlareHeight の基準)

		// ---- 樹冠包絡 (Weber & Penn の刈り込み包絡。幹の骨格が決まった時点で確定し、子の長さの刈り込みと葉の間引きに使う) ----
		bool     m_bPrune = false;				// PruneRatio > 0 かつ包絡の高さが正
		float    m_PruneBaseY = 0.0f;			// 樹冠の根元 yB = 幹の BaseOffset の位置の高さ
		float    m_PruneTopY = 1.0f;			// 樹冠の上端 yT = 幹の根元 + 幹の名目長
		XMFLOAT2 m_PruneAxis = { 0.0f, 0.0f };	// 幹の根元の (x, z) (包絡の軸は鉛直)

		// 幾何法線を Reference に合わせて巻き順を決めて三角形を追加する
		// (縮退三角形は追加しない)。頂点法線が面から離れる葉カード用
		static void AddTriangle(FProceduralMeshSection& Section,
			unsigned int I0, unsigned int I1, unsigned int I2, FXMVECTOR Reference)
		{
			const VERTEX_3D& v0 = Section.Vertices[I0];
			const VERTEX_3D& v1 = Section.Vertices[I1];
			const VERTEX_3D& v2 = Section.Vertices[I2];

			const XMVECTOR geometric = XMVector3Cross(
				XMVectorSubtract(Load3(v1.Position), Load3(v0.Position)),
				XMVectorSubtract(Load3(v2.Position), Load3(v0.Position)));
			if (LengthSq3(geometric) < 1.0e-18f)
			{
				return;
			}

			const bool bFlip = Dot3(geometric, Reference) < 0.0f;

			Section.Indices.push_back(I0);
			Section.Indices.push_back(bFlip ? I2 : I1);
			Section.Indices.push_back(bFlip ? I1 : I2);
		}

		// 頂点法線の和を基準にする版 (樹皮用)
		static void AddTriangle(FProceduralMeshSection& Section,
			unsigned int I0, unsigned int I1, unsigned int I2)
		{
			const XMVECTOR reference = XMVectorAdd(XMVectorAdd(
				Load3(Section.Vertices[I0].Normal), Load3(Section.Vertices[I1].Normal)), Load3(Section.Vertices[I2].Normal));
			AddTriangle(Section, I0, I1, I2, reference);
		}

		static VERTEX_3D MakeVertex(FXMVECTOR Position, FXMVECTOR Normal, FXMVECTOR Tangent,
			const XMFLOAT2& UV, const XMFLOAT4& Color)
		{
			VERTEX_3D v;
			v.Position = Store3(Position);
			v.Normal = Store3(Normal);
			v.Tangent = Store3(Tangent);
			v.TexCoord = UV;
			v.Color = Color;
			return v;
		}

		// 長さ比 T での枝の頂点カラー明度 (幹の根元だけ接地の陰で暗くする)
		static float StemShade(int Level, float T)
		{
			return (Level == 0) ? Lerp(0.72f, 1.0f, Clamp(T, 0.0f, 1.0f)) : 1.0f;
		}

		// テーパの形の指数 (パイプモデルあり = v2 の幹 / 枝は凸形 PipeTaperPower、なし = 線形)
		float TaperPower() const { return (m_Params.PipeExponent > 0.0f) ? PipeTaperPower : 1.0f; }

		// 分岐で切られた片か (先端は枝先でなく分岐点 = クローンの根元に埋まる): 枝先の葉 / 房を付けない
		static bool IsCutPiece(const FStem& Stem)
		{
			return Stem.SegmentsOwned < Stem.SegmentsTotal - Stem.SegmentStart;
		}

		// ============================================================
		//  (A) 骨格パス
		// ============================================================

		// ---- 樹冠包絡 (刈り込み包絡): 高さ比 h = (y - yB) / (yT - yB) での半径
		//  R(h) = PruneWidth x (yT - yB) x (h < PruneWidthPeak ? (h / PruneWidthPeak)^PrunePowerHigh : ((1 - h) / (1 - PruneWidthPeak))^PrunePowerLow)
		//  PruneWidthPeak = 最大半径になる高さ比、PrunePowerHigh = 山より下側の形、PrunePowerLow = 山より上側の形 (ヘッダ / Details の定義)。
		//  形は PruneEnvelopeFloor (最大半径の 0.25 倍) を下回らない (上端 / 根元で 0 に絞ると幹の先端から出る枝が全て切り株になる)。
		//  h は 0..1 に飽和し、-Overshoot..1 + Overshoot (幹の先端から上へ伸びる分) の外は半径 0 (= 軸上以外は全て外) ----
		float PruneRadius(float Y) const
		{
			const float height = m_PruneTopY - m_PruneBaseY;
			const float hRaw = (Y - m_PruneBaseY) / fmaxf(height, 1.0e-6f);
			if (hRaw < -PruneEnvelopeOvershoot || hRaw > 1.0f + PruneEnvelopeOvershoot) return 0.0f;
			const float h = Clamp(hRaw, 0.0f, 1.0f);
			const float peak = Clamp(m_Params.PruneWidthPeak, 0.05f, 0.95f);
			const float shape = (h < peak)
				? powf(h / peak, fmaxf(m_Params.PrunePowerHigh, 1.0e-3f))
				: powf((1.0f - h) / (1.0f - peak), fmaxf(m_Params.PrunePowerLow, 1.0e-3f));
			return m_Params.PruneWidth * height * fmaxf(shape, PruneEnvelopeFloor);
		}

		// P が包絡 (半径 Scale 倍 + Margin [m]) の内側か: 幹の根元を通る鉛直軸からの水平距離 <= Scale x R(h) + Margin (R(h) = 0 の外では Margin も効かない)
		bool InsideEnvelope(FXMVECTOR P, float Scale, float Margin = 0.0f) const
		{
			const XMFLOAT3 p = Store3(P);
			const float dx = p.x - m_PruneAxis.x;
			const float dz = p.z - m_PruneAxis.y;
			const float radius = PruneRadius(p.y);
			return radius > 0.0f && sqrtf(dx * dx + dz * dz) <= Scale * radius + Margin;
		}

		// ---- 刈り込み (枝は消さず短くするだけ。乱数は引かない): 予測した先端 P(L) = Origin + L x (Direction を鉛直面内で (Curve + CurveBack) / 2 曲げたもの)
		//  が包絡の外なら L を [0, Length] で 8 回二分して交点 L' を求め、max(0.15 Length, lerp(Length, L', PruneRatio)) にする。
		//  原点も外 (親の先端が包絡を越えている) なら L' = 0 で 0.15 Length の切り株になる ----
		float PruneLength(FXMVECTOR Origin, FXMVECTOR Direction, FXMVECTOR FrameNormal, float Length, const FProceduralTreeLevelParams& cp) const
		{
			const XMVECTOR dir = BendTowardUp(Direction, FrameNormal, 0.5f * (cp.Curve + cp.CurveBack) * PT_DEG2RAD);
			if (InsideEnvelope(XMVectorAdd(Origin, XMVectorScale(dir, Length)), 1.0f)) return Length;

			float lo = 0.0f, hi = Length;
			for (int i = 0; i < 8; ++i)
			{
				const float mid = 0.5f * (lo + hi);
				if (InsideEnvelope(XMVectorAdd(Origin, XMVectorScale(dir, mid)), 1.0f)) lo = mid; else hi = mid;
			}
			return fmaxf(0.15f * Length, Lerp(Length, lo, m_Params.PruneRatio));
		}

		// ---- 枝に付ける単葉の配置パラメータ (枝自身のストリームで引く)。原点 / 向きは出力パスの ResolveLeafSpecs が解く ----
		// 引く順: 裸枝判定 1 回 -> 1 枚につき 6 回 (位置の揺れ / 方位 / 角度 / 大きさ / 捻り / 色味の枠)。
		// 出力しない葉 (裸枝、付け根側の間引き) の分も引くので、回数は整数パラメータだけで決まる。
		//  位置: Count 枚を TMin..1 に枝先寄り (t = TMin + (1 - TMin) u^(1 / (1 + LeafTipBias))) で配り、
		//        最後の LeafTerminalCount 枚は枝先 (t = 1) に接線寄りの扇状 (1.15 倍) で付ける。
		//  枚数: 親の付け根側の枝ほど少なく (nominal = round(Count x (0.5 + 0.5 AttachT)))。付け根側のスロットから間引き、
		//        枝先の葉は常に残す (出力枚数 = min(Count, nominal + LeafTerminalCount))。
		//        房カードと混ぜるとき (ClustersPerTwig > 0、最終階層のみ) は枝先でない単葉を round(nominal x LeafMixFactor) 枚に減らす
		//        (輪郭は単葉が作り、樹冠の内側は房が埋める。draw は同じ)。
		//  裸枝: 最終階層の枝だけ、U_bare < LeafBareTwigFraction なら 1 枚も出さない (親階層の葉は裸枝にしない。draw は同じ)。
		//  方位: 黄金角の螺旋。開始位相は枝ごとに LatticeHash で散らす (draw を増やさず、全ての枝で最初の葉が同じ向きになるのを避ける)。
		// bParentLevel = 最終階層の 1 つ上の枝 (Index スロット 64 + j、葉柄は一律 2 cm)。bTipBouquet = 上位階層の枝先 (Count = 枝先の葉だけ、
		// bParentLevel の規則で引く)。戻り値 = 裸枝の draw の結果 (房カード (CollectClusters) も同じゲート)
		bool CollectLeaves(const FStem& Stem, int StemIndex, FTreeRandom& Random, int Count, float TMin, bool bParentLevel, bool bTipBouquet = false)
		{
			const bool bBare = Random.Unit() < m_Params.LeafBareTwigFraction;
			if (Count <= 0) return bBare;

			const bool bEmitNone = bBare && !bParentLevel;
			// 分岐で切られた片の先端は枝先でなく分岐点 (クローンの根元に埋まる) なので枝先の葉は付けない
			const int terminalCount = IsCutPiece(Stem) ? 0 : ClampInt(m_Params.LeafTerminalCount, 0, Count);
			const int nominal = static_cast<int>(floorf(static_cast<float>(Count) * (0.5f + 0.5f * Clamp(Stem.AttachT, 0.0f, 1.0f)) + 0.5f));
			const bool bMix = !bParentLevel && m_Params.ClustersPerTwig > 0;
			const int kept = ClampInt(bMix ? static_cast<int>(floorf(static_cast<float>(nominal) * LeafMixFactor + 0.5f)) : nominal, 0, Count - terminalCount);
			const int firstKept = (Count - terminalCount) - kept;	// これより付け根側の枝先でない葉は出さない
			const float tipExponent = 1.0f / (1.0f + fmaxf(m_Params.LeafTipBias, 0.0f));
			const int slotBase = StemIndex * 256 + (bParentLevel ? 64 : 0);	// FLeafSpec::Index 参照
			const float phase = LatticeHash(StemIndex, 9, m_Params.Seed) * PT_TWO_PI;	// 螺旋 / 枝先の扇の開始位相
			float phi = phase;

			for (int j = 0; j < Count; ++j)
			{
				const bool bTerminal = j >= Count - terminalCount;

				const float tJitter = Random.Signed(0.3f);
				const float phiJitter = Random.Signed(40.0f) * PT_DEG2RAD;
				const float angle = (m_Params.LeafAngle + Random.Signed(m_Params.LeafAngleVariance)) * PT_DEG2RAD;
				float size = fmaxf(0.01f, m_Params.LeafSize * (1.0f + Random.Signed(m_Params.LeafSizeVariance)));
				const float roll = Random.Signed(35.0f) * PT_DEG2RAD;
				Random.Unit();	// 色味の枠 (色は LatticeHash で決めるので値は使わない)

				float t;
				if (bTerminal)
				{
					// 枝先: 接線まわりに等間隔 (+ 揺れ)、大きめ
					t = 1.0f;
					phi = phase + static_cast<float>(j - (Count - terminalCount)) * PT_TWO_PI / static_cast<float>(terminalCount) + phiJitter;
					size *= 1.15f;
				}
				else
				{
					const float u = Clamp((static_cast<float>(j) + 0.5f + tJitter) / static_cast<float>(Count), 0.0f, 1.0f);
					t = TMin + (1.0f - TMin) * powf(u, tipExponent);
					phi += 137.5f * PT_DEG2RAD + phiJitter;
					size *= Lerp(1.0f, 0.8f, t * t);	// 枝先ほど若く小さい
				}

				if (bEmitNone || !(bTerminal || j >= firstKept)) continue;

				const int leafIndex = slotBase + j;

				FLeafSpec spec{};
				spec.T = t;
				spec.Phi = phi;
				spec.Angle = angle;
				spec.Roll = roll;
				spec.Size = size;
				spec.HalfWidth = size * m_Params.LeafAspect * 0.5f;
				spec.U0 = LatticeHash(leafIndex, 0, m_Params.Seed);
				spec.U1 = LatticeHash(leafIndex, 1, m_Params.Seed);
				spec.U2 = LatticeHash(leafIndex, 2, m_Params.Seed);
				spec.Index = leafIndex;
				spec.Stem = StemIndex;
				spec.AtlasCell = 0;
				spec.bCluster = false;
				spec.bTerminal = bTerminal;
				spec.bParentLevel = bParentLevel;
				spec.bTipBouquet = bTipBouquet;
				m_LeafSpecs.push_back(spec);
			}
			return bBare;
		}

		// ---- 枝に付ける房カード (アトラスのセル 1 枚 = 葉 5〜6 枚の房) の配置パラメータ。最終階層の枝だけ、単葉の後に同じストリームで引く ----
		// 引く順: 1 枚につき 5 回 (位置の揺れ / 方位の揺れ / 捻り / 大きさ / 予備)。出さない枝 (裸枝) の分も引くので、回数は枚数だけで決まる。
		//  枝の途中の Count 枚: t = ClusterTMin + (1 - ClusterTMin) (k + 0.5 + 揺れ (±0.2)) / Count (最終階層は t >= 0.12 の 88 %、側枝の胴は TMin 0.55)、方位は単葉と同じ黄金角の螺旋
		//        (開始位相は LatticeHash)、角度は LeafAngle x 0.8、大きさ ClusterSize x (1 ± LeafSizeVariance)、幅 = 大きさ x ClusterAspect。原点は枝の表面 (葉柄なし)。
		//  枝先の TerminalCount 枚: t = 1 (枝の軸の先端)、向きは normalize(lerp(接線, 上, 0.3))、j 枚目は +U を接線まわりに j x 90 deg 回す (捻りは使わない)。
		//  アトラスのセルは LatticeHash(StemIndex, 11 + k) で選ぶ。Index スロットは 128 + k (単葉 / 親階層の葉と重ねない)。
		//  裸枝 (bBare) なら 1 枚も出さない (単葉と同じゲート)。分岐で切られた片の先端には枝先の房を付けない (呼び出し側が TerminalCount 0 を渡す)。
		//  bTipBouquet = 上位階層の枝先の房。原点 / 向きは出力パスの ResolveLeafSpecs が解く
		//  TMin = 枝の途中の房を置き始める長さ比 (最終階層 ClusterTMin、側枝の胴 ParentClusterTMin)。
		//  枝先の房は小枝ごとに LatticeHash(StemIndex, 31) > 1 - TerminalClusterKeep のときだけ出す (draw は引く。上位階層の枝先の房は常に出す)
		void CollectClusters(int StemIndex, FTreeRandom& Random, int Count, int TerminalCount, bool bBare, bool bTipBouquet = false, float TMin = ClusterTMin)
		{
			const int total = ((Count < 0) ? 0 : Count) + ((TerminalCount < 0) ? 0 : TerminalCount);
			const float phase = LatticeHash(StemIndex, 10, m_Params.Seed) * PT_TWO_PI;	// 螺旋の開始位相 (単葉の 9 とは別)
			const bool bSkipTerminal = !bTipBouquet && LatticeHash(StemIndex, 31, m_Params.Seed) <= 1.0f - TerminalClusterKeep;
			float phi = phase;

			for (int k = 0; k < total; ++k)
			{
				const float tJitter = Random.Signed(0.2f);
				const float phiJitter = Random.Signed(40.0f) * PT_DEG2RAD;
				const float roll = Random.Signed(35.0f) * PT_DEG2RAD;
				const float size = fmaxf(0.01f, m_Params.ClusterSize * (1.0f + Random.Signed(m_Params.LeafSizeVariance)));
				Random.Unit();	// 予備 (1 枚 5 回に固定)
				if (bBare) continue;

				const bool bTerminal = k >= Count;
				if (bTerminal && bSkipTerminal) continue;
				const int index = StemIndex * 256 + 128 + k;

				FLeafSpec spec{};
				if (bTerminal)
				{
					spec.T = 1.0f;
					spec.Phi = static_cast<float>(k - Count) * 90.0f * PT_DEG2RAD;	// +U の接線まわりの回転 (RMF 法線から)
					spec.Angle = 0.0f;
					spec.Roll = 0.0f;
				}
				else
				{
					spec.T = Clamp(TMin + (1.0f - TMin) * (static_cast<float>(k) + 0.5f + tJitter) / static_cast<float>(Count), TMin, 1.0f);
					phi += 137.5f * PT_DEG2RAD + phiJitter;
					spec.Phi = phi;
					spec.Angle = m_Params.LeafAngle * 0.8f * PT_DEG2RAD;
					spec.Roll = roll;
				}
				spec.Size = size;
				spec.HalfWidth = 0.5f * size * m_Params.ClusterAspect;
				spec.U0 = LatticeHash(index, 0, m_Params.Seed);
				spec.U1 = LatticeHash(index, 1, m_Params.Seed);
				spec.U2 = LatticeHash(index, 2, m_Params.Seed);
				spec.Index = index;
				spec.Stem = StemIndex;
				spec.AtlasCell = ClampInt(static_cast<int>(LatticeHash(StemIndex, 11 + k, m_Params.Seed) * 4.0f), 0, 3);
				spec.bMirror = LatticeHash(index, 4, m_Params.Seed) > 0.5f;
				spec.bCluster = true;
				spec.bTerminal = bTerminal;
				spec.bParentLevel = false;
				spec.bTipBouquet = bTipBouquet;
				m_LeafSpecs.push_back(spec);
			}
		}

		// ---- 枝 1 本の骨格を作り、子枝の仕様 -> 葉の仕様 -> 再帰 の順で処理する ----
		void GenerateStem(const FChildSpec& Spec)
		{
			const int level = Spec.Level;
			const FProceduralTreeLevelParams& lp = m_Params.Level[level];
			const int segments = (Spec.SegmentsRemaining < 1) ? 1 : Spec.SegmentsRemaining;
			float segmentLength = Spec.Length / static_cast<float>(segments);	// polish3: 樹冠の天井で縮めた枝は作り直す (buildPath)
			const XMVECTOR up = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);

			FTreeRandom random(Spec.Seed);
			const int stemIndex = static_cast<int>(m_Stems.size());

			// 親に子の添字を登録する (親は m_Stems に積まれてから再帰するので必ず存在する)
			if (Spec.ParentStem >= 0)
			{
				m_Stems[Spec.ParentStem].ChildStems.push_back(stemIndex);
			}

			FStem stem;
			stem.Level = level;
			stem.Length = Spec.Length;
			stem.BaseRadius = fmaxf(Spec.Radius, MinStemRadius);
			stem.SegmentsOwned = segments;
			stem.SegmentsTotal = (Spec.SegmentsTotal < 1) ? segments : Spec.SegmentsTotal;
			// 先端は MinTipRadius を下回らない (ただし根元より太くはしない)。骨格パスの半径は線形テーパ (AssignRadii がパイプモデルで置き換える)。
			// テーパは元の枝 (分岐前) の全長で測る: クローンの先端 / 根元 = 元の枝のテーパ直線のクローンの終点 / 始点の比 (分岐した枝が片ごとに
			// 細り直すと、分岐数の比 (CloneRadiusRatio) に加えて二重に細る)。クローン以外は SegmentStart 0 なので従来どおり Base x Taper。
			// 刈り込みで縮めた枝は Taper に TipScale (縮めた長さ / 元の長さ) を掛けて先端を細らせる (太い切り株で終わらない)。
			// テーパの形は元の枝の長さ比 x で 1 - (1 - Taper) x^p (p = TaperPower: パイプモデルありで 1.5 = 根元側は太いまま先で細る凸形、なしで 1 = 直線)
			{
				const float taper = Clamp(lp.Taper, 0.0f, 1.0f) * Clamp(Spec.TipScale, 0.0f, 1.0f);
				const float total = static_cast<float>(stem.SegmentsTotal);
				stem.TaperPow = TaperPower();
				stem.TaperX0 = Clamp(static_cast<float>(Spec.SegmentStart) / total, 0.0f, 1.0f);
				stem.TaperX1 = Clamp(static_cast<float>(Spec.SegmentStart + segments) / total, 0.0f, 1.0f);
				const float taperStart = Lerp(1.0f, taper, powf(stem.TaperX0, stem.TaperPow));
				const float taperEnd = Lerp(1.0f, taper, powf(stem.TaperX1, stem.TaperPow));
				stem.TipRadius = fminf(stem.BaseRadius, fmaxf(stem.BaseRadius * taperEnd / fmaxf(taperStart, 1.0e-4f), MinTipRadius));
			}
			stem.RootRadialDir = Spec.RadialDir;
			stem.CosDown = Spec.CosDown;
			stem.ParentStem = Spec.ParentStem;
			stem.AttachT = Spec.AttachT;
			stem.ParentShade = Spec.ParentShade;
			stem.bClone = Spec.bClone;
			// 色相オフセット用の Level 1 の祖先。クローンは元の片と同じ (分岐した枝は 1 本として同じ色味)
			stem.AncestorL1 = (Spec.bClone || level != 1)
				? ((Spec.ParentStem >= 0) ? m_Stems[Spec.ParentStem].AncestorL1 : -1) : stemIndex;
			stem.UVOffsetV = 0.0f;
			stem.Seed = Spec.Seed;
			stem.SegmentStart = Spec.SegmentStart;
			stem.DistanceOffset = Spec.DistanceOffset;
			stem.CloneRadiusRatio = Spec.CloneRadiusRatio;
			// 元の枝 (分岐前) の名目長。子の長さ / 半径の比はこれに対して取る (片の長さではなく)
			const float nominalLength = Spec.Length / static_cast<float>(segments) * static_cast<float>(stem.SegmentsTotal);

			// ---- セグメント列 (曲がり + 低域通過した揺れ + 屈光性 + 回転最小フレーム)。1 セグメント 4 回引く (揺れの振幅 / 方位 + 分岐の位相 / 揺れ) ----
			//  揺れは先に全セグメント分を引いてベクトル v[s] = w (cos az, sin az) (フレームの N / B 成分 [deg]) にし、隣接セグメントで
			//  低域通過 (v' = 0.25 v[s-1] + 0.5 v[s] + 0.25 v[s+1]、端は折り返し) してから使う: ジグザグにならず、引く回数は Segments だけで決まる
			//  分岐 (T12) 用にセグメントごとにさらに 2 回 (クローンの方位の位相 / 分岐角と長さの揺れ) 引く。分岐しなくても引く (回数は Segments だけで決まる)
			std::vector<XMVECTOR> positions(segments + 1);
			std::vector<XMVECTOR> normals(segments + 1);
			std::vector<XMVECTOR> segmentDirs(segments);
			std::vector<XMFLOAT2> wobble(segments);
			std::vector<float> splitPhase(segments);
			std::vector<float> splitJitter(segments);

			for (int s = 0; s < segments; ++s)
			{
				const float w = random.Signed(lp.CurveVariance);
				const float az = random.Range(0.0f, PT_TWO_PI);
				wobble[s] = { w * cosf(az), w * sinf(az) };
				splitPhase[s] = random.Unit();
				splitJitter[s] = random.Unit();
			}

			// ---- 分岐の予定 (乱数は使わない): 切るセグメントと分岐数。切られた片は owned セグメントだけ持ち、残りをクローンが継ぐ ----
			int splitCount = 0;
			float splitError = Spec.SplitError;
			const int splitSegment = FindSplitSegment(lp, m_Params.BaseSplits, level == 0 && !Spec.bClone, segments, stem.SegmentsTotal, splitError, splitCount);
			const int owned = (splitSegment >= 0) ? splitSegment + 1 : segments;

			// 曲がり / 揺れ / 屈光性の 1 セグメント分は元の枝 (分岐前) の Segments で割り、前半 / 後半は元の枝のセグメント番号で選ぶ:
			// 分岐したクローンは元の枝の予定をそのまま続ける (Curve = 枝全体の曲がり、Tropism = 水平な枝の全長での曲がり、という契約は片に分かれても同じ)
			const float invSegments = 1.0f / static_cast<float>(stem.SegmentsTotal);
			const float halfSegments = 0.5f * static_cast<float>(stem.SegmentsTotal);
			const float tropismPerSegment = lp.Tropism * m_Params.TropismScale * invSegments;	// [deg]、(1 - t.y) を掛ける前
			// 1 セグメントの曲がりの上限: 40 deg、ただし隣接リングが交差しない範囲 (リングは隣の接線の平均に直交するので、曲がり β で
			// 内側の点が互いに r sin(β/2) ずつ寄る: 間隔 L > 2 r sin(β/2))。刈り込みで短くなった切り株 (セグメント < 半径) だけに効く
			const float ringRadius = stem.BaseRadius * (1.0f + fmaxf(m_Params.JunctionCollar, 0.0f));

			XMVECTOR position, tangent, normal;
			// 骨格の経路を現在の segmentLength で作る (乱数は上で引き済み。樹冠の天井で縮めたら同じ角度の予定で作り直す)
			auto buildPath = [&]()
			{
				position = Load3(Spec.Origin);
				tangent = SafeNormalize(Load3(Spec.Direction), up);
				normal = Orthonormalize(Load3(Spec.Normal), tangent);
				positions[0] = position;
				normals[0] = normal;
				const float maxBendRad = fminf(MaxBendPerSegmentDeg * PT_DEG2RAD,
					2.0f * asinf(Clamp(0.45f * segmentLength / fmaxf(ringRadius, 1.0e-6f), 0.0f, 1.0f)));

				for (int s = 0; s < owned; ++s)
				{
					XMVECTOR t = tangent;
					// 分岐直後のセグメントでは系統的な曲がり (曲がり + 屈光性) を鉛直の手前 (分岐角) で止める: 垂れた小枝では鉛直で飽和する
					// 屈光性 / CurveBack が分岐角 (最小 8 deg) を 1 セグメントで打ち消し、兄弟のクローンが同じ真下の向きに重なって二重の面になる。
					// 曲がり自体は予定どおり続ける (元の枝の S 字や屈光性の合計は分岐しても保たれる)
					const float keepFromVertical = (Spec.bClone && s == 0) ? fmaxf(Spec.KeepFromVertical, 0.0f) : 0.0f;	// [rad]
					const int logicalSegment = Spec.SegmentStart + s;	// 元の枝のセグメント番号

					// 系統的な曲がり: 鉛直面内で上 (+) / 下 (-) へ (鉛直な接線ならフレームの法線まわり)。
					// CurveBack 0 なら Curve を全長に配る。非 0 なら前半 Curve、後半 CurveBack の S 字 (各半分を Segments / 2 で配る。
					// 奇数セグメントでは中央のセグメントが前半と後半を半分ずつ受け、合計は常に Curve + CurveBack)。
					// 屈光性と同じく鉛直 (天頂 / 真下) を越えては曲げない: 真下に達した小枝が CurveBack で折り返してジグザグになるのを防ぐ
					float curveDeg;
					if (lp.CurveBack == 0.0f)
					{
						curveDeg = lp.Curve * invSegments;
					}
					else
					{
						const float inFront = Clamp(halfSegments - static_cast<float>(logicalSegment), 0.0f, 1.0f);	// このセグメントのうち前半に属する割合
						curveDeg = (lp.Curve * inFront + lp.CurveBack * (1.0f - inFront)) / halfSegments;
					}
					{
						const float toVerticalDeg = fmaxf(AngleToVertical(t, curveDeg >= 0.0f) - keepFromVertical, 0.0f) / PT_DEG2RAD;	// 天頂 / 真下までの残りの角
						curveDeg = Clamp(curveDeg, -toVerticalDeg, toVerticalDeg);
						t = BendTowardUp(t, normal, curveDeg * PT_DEG2RAD);
					}

					// 低域通過した揺れ: 軸 = v' の向き (N / B 平面、曲げた後の接線に直交化)、角度 = |v'| / Segments
					{
						const XMFLOAT2& v0 = wobble[(s > 0) ? s - 1 : s];
						const XMFLOAT2& v1 = wobble[s];
						const XMFLOAT2& v2 = wobble[(s + 1 < segments) ? s + 1 : s];
						const float vx = 0.25f * v0.x + 0.5f * v1.x + 0.25f * v2.x;
						const float vy = 0.25f * v0.y + 0.5f * v1.y + 0.25f * v2.y;
						const float amplitude = sqrtf(vx * vx + vy * vy);
						if (amplitude > 1.0e-6f)
						{
							const XMVECTOR binormal = XMVector3Cross(tangent, normal);
							const XMVECTOR axis = Orthonormalize(XMVectorAdd(XMVectorScale(normal, vx), XMVectorScale(binormal, vy)), t);
							t = RotateAxis(t, axis, amplitude * invSegments * PT_DEG2RAD);
						}
					}

					// 屈光性: 鉛直面内で Tropism x TropismScale x (1 - t.y) / Segments [deg] 曲げる (曲がりと同じ軸 / 符号の規則:
					//  + で上 (+Y) へ、- で垂れる)。直立した枝で 0、水平で 1 倍、垂れた枝ほど強く効く。1 セグメント ±12 deg まで。
					//  鉛直 (天頂 / 真下) を越えては引かない (越えると鉛直面内で折り返してジグザグになる): 残りの角で飽和させる
					{
						const float ty = Clamp(XMVectorGetY(t), -1.0f, 1.0f);
						const float tropismRad = Clamp(tropismPerSegment * (1.0f - ty), -MaxTropismPerSegmentDeg, MaxTropismPerSegmentDeg) * PT_DEG2RAD;
						const float toVertical = fmaxf(AngleToVertical(t, tropismRad >= 0.0f) - keepFromVertical, 0.0f);	// 天頂 / 真下までの残りの角
						t = BendTowardUp(t, normal, Clamp(tropismRad, -toVertical, toVertical));
					}

					// polish3: 幹の脇の片は 2 番目からのセグメント (片の前半) で UprightBend だけ鉛直へ戻す (最初のセグメントは分岐角いっぱいに開いてから立ち上がる。天頂は越えない)
					if (Spec.UprightBend > 0.0f)
					{
						const int upright = (segments >= 3) ? segments / 2 : 1;
						if (s >= 1 && s <= upright)
						{
							const float bend = fminf(Spec.UprightBend / static_cast<float>(upright), AngleToVertical(t, true));
							t = BendTowardUp(t, normal, bend);
						}
					}

					// polish3: 屈曲 (仮軸分枝のジグザグ)。Level 1 / 2 と幹の分岐より上の片だけ。元の枝のセグメント番号 j で 0 番は +k/2、以降 (-1)^j k を
					// 交互に折る (向きは +k/2 と -k/2 を行き来し、枝全体の向きは変わらない)。k = LimbKinkFactor x CurveVariance x (0.6..1)、
					// 折る面は枝ごとの方位 + 0.45 rad / セグメントで回る (平面のジグザグでなく、うねる)。乱数は引かない
					if ((level == 1 || level == 2 || (level == 0 && Spec.bClone)) && lp.CurveVariance > 0.0f)
					{
						const unsigned int key = Spec.KinkSeed;
						const float amplitude = fminf(LimbKinkFactor * lp.CurveVariance, 20.0f)
							* Lerp(0.6f, 1.0f, LatticeHash(static_cast<int>(key), 50 + logicalSegment, m_Params.Seed));
						const float kinkDeg = (logicalSegment == 0) ? 0.5f * amplitude : ((logicalSegment & 1) ? -amplitude : amplitude);
						const float az = LatticeHash(static_cast<int>(key), 49, m_Params.Seed) * PT_TWO_PI + 0.45f * static_cast<float>(logicalSegment);
						const XMVECTOR binormal = XMVector3Cross(tangent, normal);
						const XMVECTOR axis = Orthonormalize(XMVectorAdd(XMVectorScale(normal, cosf(az)), XMVectorScale(binormal, sinf(az))), t);
						t = RotateAxis(t, axis, kinkDeg * PT_DEG2RAD);
					}

					// 1 セグメントの合計の曲がり (曲がり + 揺れ + 屈光性) を抑える (隣接リングのフレームが捻れず、チューブが自己交差しない)
					t = LimitBend(tangent, t, normal, maxBendRad);

					normal = TransportNormal(normal, tangent, t);
					tangent = t;

					segmentDirs[s] = t;
					position = XMVectorAdd(position, XMVectorScale(t, segmentLength));
					positions[s + 1] = position;
					normals[s + 1] = normal;
				}
			};
			buildPath();

			// ---- polish3: 樹冠の天井。切られていない片 (Level 1 以上) の実際の経路の最も高い点が包絡の上端 (高さ比 1 + PruneEnvelopeOvershoot、それより上は半径 0 で
			//      葉も全て間引かれる) を越えたら、先端の高さがちょうど天井 (少し内側) になるまで枝を一様に縮めて作り直す (向きの予定は同じ、乱数は引かない)。
			//      予測した先端で刈り込む PruneLength の取りこぼし (曲がり / 揺れ / 屈曲で上へ反った枝) を拾い、樹冠の頂が平らな丸屋根になる ----
			if (m_bPrune && level >= 1 && owned == segments)
			{
				// 縮めても 1 セグメントを MinTrimSegment [m] より短くはしない (数 mm の切り株で隣接リングと先端の半球が潰れ、小枝の法線の寄せで裏返る)。
				// 半径に依らない絶対値 (骨格パスの半径はテーパの形 = PipeExponent で変わる: 半径で決めると PipeExponent が骨格を動かす)
				const float minScale = fminf(1.0f, MinTrimSegment / fmaxf(segmentLength, 1.0e-6f));
				const float ceilingY = m_PruneBaseY + (1.0f + 0.9f * PruneEnvelopeOvershoot) * (m_PruneTopY - m_PruneBaseY);
				const float originY = Spec.Origin.y;
				float tipY = XMVectorGetY(positions[owned]);
				for (int k = 1; k <= owned; ++k) tipY = fmaxf(tipY, XMVectorGetY(positions[k]));
				if (tipY > ceilingY && tipY > originY + 1.0e-4f)
				{
					const float scale = Clamp((ceilingY - originY) / (tipY - originY), 0.15f, 1.0f);
					segmentLength *= fmaxf(scale, minScale);
					buildPath();
				}
				// ---- polish3: 小枝 / 側枝の先を葉の間引きの範囲に収める。最終階層とその 1 つ上 (側枝) の切られていない片の先端が範囲 (TwigTrimScale x R(h) + LeafCullMargin /
				//      PruneRatio - TwigTipCardReach) の外なら、先端が範囲に入る倍率 (8 回の二分、根元も外なら BareTwigKeep) で枝を一様に縮めて作り直す。
				//      葉は後で長さ比で付くので短くなった枝に沿い、枝先の葉 / 房は新しい先端に付く: 包絡の上で葉を全て間引かれた裸の鞭が突き出ない。
				//      骨格と包絡だけで決まる (葉のパラメータは枝を動かさない。判定は接合部の沈み込みの前の位置 = 沈み込みは剛体の平行移動のまま) ----
				if (level >= m_Params.Levels - 2)
				{
					const float margin = fmaxf(LeafCullMargin / fmaxf(m_Params.PruneRatio, 1.0e-3f) - TwigTipCardReach, 0.0f);
					const XMVECTOR origin = Load3(Spec.Origin);
					const XMVECTOR reach = XMVectorSubtract(positions[owned], origin);
					if (!InsideEnvelope(positions[owned], TwigTrimScale, margin))
					{
						float scale = BareTwigKeep;
						if (InsideEnvelope(origin, TwigTrimScale, margin))
						{
							float lo = 0.0f, hi = 1.0f;
							for (int i = 0; i < 8; ++i)
							{
								const float mid = 0.5f * (lo + hi);
								if (InsideEnvelope(XMVectorAdd(origin, XMVectorScale(reach, mid)), TwigTrimScale, margin)) lo = mid; else hi = mid;
							}
							scale = fmaxf(lo, BareTwigKeep);
						}
						segmentLength *= fmaxf(scale, minScale);
						buildPath();
					}
				}
				stem.Length = static_cast<float>(segments) * segmentLength;
			}

			// ---- 分岐で切る: 片は owned セグメントだけ (長さ / 先端半径はそこでの線形テーパの値。先端の半球はクローンの中に埋まる) ----
			if (owned < segments)
			{
				const float xCut = Clamp(static_cast<float>(Spec.SegmentStart + owned) / static_cast<float>(stem.SegmentsTotal), 0.0f, 1.0f);
				stem.TipRadius = Lerp(stem.BaseRadius, stem.TipRadius, TaperShape(stem, xCut));
				stem.TaperX1 = xCut;
				stem.Length = static_cast<float>(owned) * segmentLength;
			}
			stem.SegmentsOwned = owned;

			// ---- 子 / 葉の取り分: 元の枝のうち子を生やせる部分 (BaseOffset より先) に占めるこの片の割合。区間 [ShareLo, ShareEnd] で持ち、
			//      分岐の残り [ShareEnd, ShareHi] はクローンが等分する (全ての片の取り分は元の枝 1 本分にちょうど足し上がる = 子の数の保存) ----
			const float totalSegments = static_cast<float>(stem.SegmentsTotal);
			const float baseSegments = Clamp(lp.BaseOffset, 0.0f, 0.95f) * totalSegments;
			auto childBearing = [&](float SegmentPos)	// 元の枝のセグメント位置 -> 子を生やせる部分の中の割合 0..1
				{
					return Clamp((SegmentPos - baseSegments) / fmaxf(totalSegments - baseSegments, 1.0e-3f), 0.0f, 1.0f);
				};
			const float segStart = static_cast<float>(Spec.SegmentStart);
			const float bearing0 = childBearing(segStart);
			const float bearing1 = childBearing(segStart + static_cast<float>(segments));
			const float bearingEnd = childBearing(segStart + static_cast<float>(owned));
			stem.ShareLo = Spec.ShareLo;
			stem.ShareEnd = (owned < segments && bearing1 > bearing0)
				? Spec.ShareLo + (Spec.ShareHi - Spec.ShareLo) * ((bearingEnd - bearing0) / (bearing1 - bearing0))
				: Spec.ShareHi;

			// ---- 骨格点 (接線は隣接セグメントの平均で滑らかにする) ----
			stem.Points.resize(owned + 1);
			for (int i = 0; i <= owned; ++i)
			{
				XMVECTOR pointTangent;
				if (i == 0)              pointTangent = segmentDirs[0];
				else if (i == owned)     pointTangent = segmentDirs[owned - 1];
				else                     pointTangent = SafeNormalize(XMVectorAdd(segmentDirs[i - 1], segmentDirs[i]), segmentDirs[i]);

				const float f = static_cast<float>(i) / static_cast<float>(owned);

				FStemPoint& pt = stem.Points[i];
				pt.Position = Store3(positions[i]);
				pt.Tangent = Store3(pointTangent);
				pt.Normal = Store3(Orthonormalize(normals[i], pointTangent));
				pt.Radius = Lerp(stem.BaseRadius, stem.TipRadius, TaperShape(stem, Lerp(stem.TaperX0, stem.TaperX1, f)));
				pt.Distance = f * stem.Length;
			}

			// ---- 樹冠包絡の確定 (幹の最初の片): 根元 yB = 幹の BaseOffset の位置 (距離で測る: 分岐で切られた片でも同じ)、上端 yT = 根元 + 幹の名目長 ----
			if (level == 0 && !Spec.bClone)
			{
				const float baseT = Clamp(Clamp(lp.BaseOffset, 0.0f, 0.95f) * m_TrunkLength / fmaxf(stem.Length, 1.0e-4f), 0.0f, 1.0f);
				m_PruneBaseY = XMVectorGetY(SampleStem(stem, baseT).Position);
				m_PruneTopY = Spec.Origin.y + m_TrunkLength;
				m_PruneAxis = { Spec.Origin.x, Spec.Origin.z };
				m_bPrune = m_Params.PruneRatio > 0.0f && m_PruneTopY - m_PruneBaseY > 1.0e-3f;
			}

			// ---- 子枝の仕様 (1 本 4 回引く: 位置の揺れ / 分岐角の揺れ / 下向き角の揺れ / 長さの揺れ)。子の数は元の枝の Branches をセグメント数比で配分する。
			//  位置 (頂芽優勢): u = (k + 0.5 + 揺れ) / count、t = baseOffset + (1 - baseOffset) u^(1 / (1 + TipBias))。
			//  比 r = 1 - (t - baseOffset) / (1 - baseOffset) (1 = 樹冠の根元側、0 = 親の先端)。
			//  方位: 螺旋 (phi += Rotate + 揺れ)。bAlternate なら基準面法線 planeN (空寄り) から ±(180 - Rotate + 揺れ) で左右交互 (平面的な小枝)。
			//  下向き角 = DownAngle + DownAngleShape x (1.6 r - 0.6) + 揺れ (根元側 +Shape で水平寄り、先端側 -0.6 Shape で立つ)、5..175 deg。
			//  長さ = 親の長さ x Length x [Level 1: ShapeRatio(CrownShape, r)] x lerp(1, LengthShape, 1 - r) x (1 + 揺れ) を樹冠包絡で刈り込む。
			//  子は必ず push する (枝数は整数パラメータだけで決まる。刈り込まれても切り株が残る) ----
			std::vector<FChildSpec> children;

			// ---- 分岐のクローン (通常の子より先に積む = 子番号はクローンから数える)。切った先端から残りのセグメントを継ぐ ----
			//  分岐角 = max(8 deg, SplitAngle + 揺れ x SplitAngleVariance - 0.5 x 鉛直からの傾き) (鉛直な幹ほど大きく開き、水平な枝は開かない)。
			//  非対称 (音叉の Y にしない): k = 0 は主軸を継ぐ片で分岐角 x MainSplitDeviation だけ逸れ、方位は脇の片の反対側。
			//  脇の片 (k >= 1) は分岐角いっぱいに開き、方位 = 位相 + (k - 1) x 360 / (本数 - 1) (フレームの N / B 面内)。
			//  断面の取り分 (パイプモデル): 脇の片の合計 S = lerp(SideShareMin, SideShareMax, LatticeHash) x (本数 - 1) (0.75 まで) を等分、主軸は 1 - S。
			//  半径 = r x 取り分^(1 / PipeExponent) (出力パスで親の確定半径に合わせる)。長さ = 残りのセグメント x (1 ± 0.1)、脇の片は
			//  さらに lerp(SideLengthMin, 1, LatticeHash) 倍 (主軸より短い)。根元は先端から (0.3 + 0.15 k) r 戻した位置 (根元の円盤を親の中に埋める。
			//  片ごとに深さを変えるのは、半径が下限で揃った兄弟の根元リングが同じ中心で交わって頂点が重ならないように)。
			//  見た目の選択は全て LatticeHash (draw は増やさない)
			if (splitSegment >= 0)
			{
				const int clones = splitCount + 1;
				const XMVECTOR t = segmentDirs[owned - 1];
				const XMVECTOR n = Orthonormalize(normals[owned], t);
				const XMVECTOR b = SafeNormalize(XMVector3Cross(t, n), AnyPerpendicular(t));
				const float rSplit = stem.TipRadius;
				const float declinationDeg = acosf(Clamp(XMVectorGetY(t), -1.0f, 1.0f)) / PT_DEG2RAD;
				const float jitter = 2.0f * splitJitter[splitSegment] - 1.0f;
				const float splitAngle = Clamp(lp.SplitAngle + jitter * lp.SplitAngleVariance - 0.5f * declinationDeg, 8.0f, 90.0f) * PT_DEG2RAD;
				const float pipeN = fmaxf(m_Params.PipeExponent, 1.0f);
				const float sideShare = fminf(Lerp(SideShareMin, SideShareMax, LatticeHash(static_cast<int>(Spec.Seed), 6, m_Params.Seed))
					* static_cast<float>(clones - 1), 0.75f);
				const float shareWidth = (Spec.ShareHi - stem.ShareEnd) / static_cast<float>(clones);
				const float phase = splitPhase[splitSegment] * PT_TWO_PI;

				for (int k = 0; k < clones; ++k)
				{
					const bool bMain = k == 0;
					const float phi = bMain ? phase + PT_PI : phase + static_cast<float>(k - 1) * PT_TWO_PI / static_cast<float>(clones - 1);
					const float deviation = bMain ? MainSplitDeviation * splitAngle : splitAngle;
					const float share = bMain ? 1.0f - sideShare : sideShare / static_cast<float>(clones - 1);
					const float radiusRatio = powf(fmaxf(share, 1.0e-3f), 1.0f / pipeN);
					const float lengthScale = bMain ? 1.0f : Lerp(SideLengthMin, 1.0f, LatticeHash(static_cast<int>(Spec.Seed), 7 + k, m_Params.Seed));
					const XMVECTOR lateral = XMVectorAdd(XMVectorScale(n, cosf(phi)), XMVectorScale(b, sinf(phi)));
					const XMVECTOR dir = SafeNormalize(XMVectorAdd(XMVectorScale(t, cosf(deviation)), XMVectorScale(lateral, sinf(deviation))), t);

					FChildSpec clone;
					clone.Origin = Store3(XMVectorSubtract(positions[owned], XMVectorScale(t, (0.3f + 0.15f * static_cast<float>(k)) * rSplit)));
					clone.Direction = Store3(dir);
					clone.Normal = Store3(TransportNormal(n, t, dir));
					clone.Length = fmaxf(0.01f, static_cast<float>(segments - owned) * segmentLength * (1.0f + 0.1f * jitter) * lengthScale);
					clone.Radius = rSplit * radiusRatio;
					clone.Level = level;
					clone.ParentStem = stemIndex;
					clone.AttachT = Spec.AttachT;			// 元の枝の付け根の位置 (葉の枚数の名目値に使う)
					clone.ParentShade = RingShade(stem, stem.Length);	// 切った先端の明度を続ける
					clone.ParentRadius = rSplit;
					clone.SegmentsRemaining = segments - owned;
					clone.SegmentsTotal = stem.SegmentsTotal;
					clone.bClone = true;
					clone.Seed = HashCombine(Spec.Seed, static_cast<unsigned int>(children.size()) + 1u, static_cast<unsigned int>(level));
					clone.RadialDir = Spec.RadialDir;
					clone.CosDown = 1.0f;
					clone.SegmentStart = Spec.SegmentStart + owned;
					clone.ShareLo = stem.ShareEnd + static_cast<float>(k) * shareWidth;
					clone.ShareHi = (k + 1 == clones) ? Spec.ShareHi : stem.ShareEnd + static_cast<float>(k + 1) * shareWidth;
					clone.SplitError = splitError;
					clone.CloneRadiusRatio = radiusRatio;
					clone.DistanceOffset = stem.DistanceOffset + stem.Length;
					clone.KeepFromVertical = deviation;
					clone.TipScale = Spec.TipScale;
					// polish3: 幹の脇の片は前半で分岐角 x SideCloneUpright だけ立ち上がる (壺形)。屈曲の予定は元の枝のまま続ける
					clone.UprightBend = (level == 0 && !bMain) ? SideCloneUpright * splitAngle : 0.0f;
					clone.KinkSeed = Spec.KinkSeed;
					// polish3: 主軸 (幹の片) の先端は樹冠の上端 yT から LeaderTopMargin x ClusterSize 下で止める (予測した先端 = 原点 + 向き x 長さ。
					// 枝先の葉 / 房が包絡の上で間引かれて裸の先が突き出ない)。半分より短くはしない
					if (level == 0 && m_bPrune)
					{
						const float limitY = m_PruneTopY - LeaderTopMargin * m_Params.ClusterSize;
						const float dirY = XMVectorGetY(dir);
						const float tipY = clone.Origin.y + dirY * clone.Length;
						if (tipY > limitY && dirY > 0.1f)
						{
							clone.Length = fmaxf(0.5f * clone.Length, (limitY - clone.Origin.y) / dirY);
						}
					}
					children.push_back(clone);
				}
			}

			const int childLevel = level + 1;
			if (childLevel < m_Params.Levels)
			{
				const FProceduralTreeLevelParams& cp = m_Params.Level[childLevel];
				// 子の数 = この片の取り分 (元の枝 1 本の Branches を、分岐した片が子を生やせる長さの比で分け合う。合計は常に Branches)
				const int count = SharedCount(cp.Branches, stem.ShareLo, stem.ShareEnd);
				// 子を生やし始める位置 (片の長さ比): 元の枝の BaseOffset の位置より先 (クローンが BaseOffset より先で始まれば 0)
				const float baseOffset = Clamp((baseSegments - segStart) / static_cast<float>(owned), 0.0f, 1.0f);
				const float tipExponent = 1.0f / (1.0f + fmaxf(cp.TipBias, 0.0f));
				float phi = random.Range(0.0f, PT_TWO_PI);	// 螺旋の開始位相 (bAlternate では使わないが draw は同じ)

				// polish3: 二又の分岐点のまわり (切られた片の先端 / クローンの根元から ForkClearance) には主枝を置かない (分岐点に 3 本が集まって
				// 接ぎ木のように見えない)。子の位置の範囲 [baseOffset, 1] をその内側へ写す (子の数は変わらない)。幹の片だけ
				const bool bCutHere = owned < segments;
				float forkLo = baseOffset, forkHi = 1.0f;
				if (childLevel == 1 && stem.Length > 1.0e-3f)
				{
					if (Spec.bClone) forkLo = fmaxf(forkLo, fminf(ForkClearance / stem.Length, 0.5f));
					if (bCutHere) forkHi = fminf(forkHi, fmaxf(1.0f - ForkClearance / stem.Length, 0.5f));
					forkHi = fmaxf(forkHi, forkLo);
				}

				for (int k = 0; k < count; ++k)
				{
					// 枠の位置を LatticeHash で揺らす: Level 1 (主枝) は ±LimbSlotJitter (群れと隙間)、それより下は ±ChildSlotJitter
					// (小枝が等間隔の櫛にならない)。draw は同じ
					const float slotJitter = ((childLevel == 1) ? LimbSlotJitter : ChildSlotJitter)
						* (2.0f * LatticeHash(static_cast<int>(Spec.Seed), 40 + k, m_Params.Seed) - 1.0f);
					// 枠の揺れで子が親の端 (分岐点 / 先端) にちょうど乗らないよう、揺れなしの範囲 (k + 0.5 ± 0.25) の両端 0.25 / count .. 1 - 0.25 / count に収める
					// (クローンの根元 t = 0 の子はパイプモデルの窓で分岐直後のリングを細らせ、分岐の太さが合わなくなる)
					const float uEdge = 0.25f / static_cast<float>(count);
					float u = Clamp((static_cast<float>(k) + 0.5f + random.Signed(0.25f) + slotJitter) / static_cast<float>(count), uEdge, 1.0f - uEdge);
					const float rotateJitter = random.Signed(cp.RotateVariance);
					const float downJitter = random.Signed(cp.DownAngleVariance);
					const float lengthJitter = random.Signed(cp.LengthVariance);
					// polish3: 主枝は LimbTiers 段の中心へ LimbTierPull だけ寄せる (段ごとに群れ、段の間に空が抜ける)
					if (childLevel == 1)
					{
						const float tiers = static_cast<float>(LimbTiers);
						const float tierCenter = (fminf(floorf(u * tiers), tiers - 1.0f) + 0.5f) / tiers;
						u = Lerp(u, tierCenter, LimbTierPull);
					}

					float t = Clamp(baseOffset + (1.0f - baseOffset) * ((cp.TipBias > 0.0f) ? powf(u, tipExponent) : u), baseOffset, 1.0f);
					if (childLevel == 1 && (forkLo > baseOffset || forkHi < 1.0f))
					{
						t = forkLo + (forkHi - forkLo) * Clamp((t - baseOffset) / fmaxf(1.0f - baseOffset, 1.0e-4f), 0.0f, 1.0f);
					}
					// 分岐点から ForkDownRange 以内の主枝は下向き角を ForkMaxDownDeg までに抑える (分岐点から真横に出る細い枝にしない)
					bool bNearFork = false;
					if (childLevel == 1)
					{
						const float dFork = fminf(Spec.bClone ? t * stem.Length : 1.0e9f, bCutHere ? (1.0f - t) * stem.Length : 1.0e9f);
						bNearFork = dFork < ForkDownRange;
					}
					// 比 r は元の枝全体で測る (分岐した片でも樹冠の根元 1 -> 元の枝の先端 0 が続く)
					const float r = 1.0f - childBearing(segStart + t * static_cast<float>(owned));

					const FStemSample s = SampleStem(stem, t);
					XMVECTOR radialDir;
					if (cp.bAlternate)
					{
						const float side = (k & 1) ? 1.0f : -1.0f;
						radialDir = RotateAxis(PlaneNormal(s), s.Tangent, side * (180.0f - cp.Rotate + rotateJitter) * PT_DEG2RAD);
					}
					else
					{
						phi += (cp.Rotate + rotateJitter) * PT_DEG2RAD;
						radialDir = RotateAxis(s.Normal, s.Tangent, phi);
					}

					const float downDeg = Clamp(cp.DownAngle + cp.DownAngleShape * (1.6f * r - 0.6f) + downJitter, 5.0f, 175.0f);
					const float down = (bNearFork ? fminf(downDeg, ForkMaxDownDeg) : downDeg) * PT_DEG2RAD;
					const XMVECTOR childDir = SafeNormalize(
						XMVectorAdd(XMVectorScale(s.Tangent, cosf(down)), XMVectorScale(radialDir, sinf(down))), s.Tangent);
					const XMVECTOR childNormal = Orthonormalize(radialDir, childDir);

					// 親の先端側ほど短く (LengthShape)。Level 1 は樹冠の形 (ShapeRatio) も掛ける
					float shape = Lerp(1.0f, cp.LengthShape, 1.0f - r);
					if (childLevel == 1) shape *= ShapeRatio(m_Params.CrownShape, r);
					float childLength = fmaxf(0.01f, nominalLength * cp.Length * shape * (1.0f + lengthJitter));
					float tipScale = 1.0f;	// 刈り込みで縮めた比 (子の Taper に掛ける: 短くした枝ほど先端が細い)
					const float naturalLength = childLength;	// 刈り込み前の長さ (半径の比はこれで取る: 包絡で縮めた主枝が細い棒にならない)
					if (m_bPrune)
					{
						const float pruned = PruneLength(s.Position, childDir, childNormal, childLength, cp);
						tipScale = Clamp(pruned / childLength, 0.0f, 1.0f);
						childLength = pruned;
					}
					// 子の半径 = 親の半径 x (子の長さ / 親の長さ)^RatioPower (Weber & Penn)。上限は Radius x 親 (0.9 まで)、下限は MinTipRadius。
					// 親の半径は骨格パスのテーパ値 (出力パスの AssignRadii が 0.85 x 親の確定半径で改めて抑える)、親の長さは元の枝の名目長、
					// 子の長さは刈り込み前の長さ (刈り込みは先端の Taper (TipScale) だけを細らせる)
					// polish3: 主枝 (Level 1) の下限は付け根での親の半径 x LimbMinRadiusRatio (上限を越えない): 樹冠の中に太い骨格の枝が見える
					const float radiusHi = s.Radius * fminf(cp.Radius, 0.9f);
					const float radiusLo = (childLevel == 1) ? fminf(fmaxf(MinTipRadius, LimbMinRadiusRatio * s.Radius), radiusHi) : MinTipRadius;
					const float childRadius = Clamp(
						s.Radius * powf(fmaxf(naturalLength / fmaxf(nominalLength, 1.0e-4f), 1.0e-4f), cp.RatioPower),
						radiusLo, radiusHi);

					FChildSpec child;
					child.Origin = Store3(s.Position);
					child.Direction = Store3(childDir);
					child.Normal = Store3(childNormal);
					child.Length = childLength;
					child.Radius = childRadius;
					child.Level = childLevel;
					child.ParentStem = stemIndex;
					child.AttachT = t;
					child.ParentShade = ChildShade(stem, t);
					child.ParentRadius = s.Radius;
					child.SegmentsRemaining = (cp.Segments < 1) ? 1 : cp.Segments;
					child.SegmentsTotal = child.SegmentsRemaining;
					child.bClone = false;
					// 子番号はクローンを先に数える (クローンは children の先頭に積んである)
					child.Seed = HashCombine(Spec.Seed, static_cast<unsigned int>(children.size()) + 1u,
						static_cast<unsigned int>(childLevel));
					child.RadialDir = Store3(radialDir);
					child.CosDown = cosf(down);
					child.SegmentStart = 0;
					child.ShareLo = 0.0f;
					child.ShareHi = 1.0f;
					child.SplitError = 0.0f;
					child.CloneRadiusRatio = 0.0f;
					child.DistanceOffset = 0.0f;
					child.KeepFromVertical = 0.0f;
					child.TipScale = tipScale;
					child.UprightBend = 0.0f;
					child.KinkSeed = child.Seed;

					stem.ChildAttach.emplace_back(t, childRadius);
					children.push_back(child);
				}
			}

			// ---- 葉の仕様 (子の後に引く)。最終階層: LeavesPerBranch 枚を t >= 0.1 に、
			//      その 1 つ上の階層: LeavesOnParentLevel 枚を t >= ParentLeafTMin (0.35) に + 房 ClustersOnParentLevel 枚を t >= 0.55 に ----
			//      枚数は子と同じ取り分で分岐した片が分け合う (片ごとに最低 1 枚)。
			//      最終階層の房カード (ClustersPerTwig > 0 のときだけ。0 = 房を使わない、枝先の房も出さない) は単葉の後に同じ取り分で集める:
			//      裸枝ゲートは単葉と共通、枝先の房は切られていない片だけ
			if (m_Params.bGenerateLeaves)
			{
				if (level == m_Params.Levels - 1)
				{
					const bool bBare = CollectLeaves(stem, stemIndex, random, SharedLeafCount(stem, m_Params.LeavesPerBranch), 0.1f, false);
					if (m_Params.ClustersPerTwig > 0)
					{
						CollectClusters(stemIndex, random, SharedLeafCount(stem, m_Params.ClustersPerTwig), IsCutPiece(stem) ? 0 : m_Params.TerminalClusters, bBare);
					}
				}
				else if (level == m_Params.Levels - 2)
				{
					// 側枝の胴: 単葉 LeavesOnParentLevel 枚を t >= ParentLeafTMin に、房 ClustersOnParentLevel 枚を t >= ParentClusterTMin に
					// (ClustersPerTwig > 0 のとき。裸枝にしない、枝先の房は付けない)。どちらも枚数は整数パラメータだけで決まる
					if (m_Params.LeavesOnParentLevel > 0)
					{
						CollectLeaves(stem, stemIndex, random, SharedLeafCount(stem, m_Params.LeavesOnParentLevel), ParentLeafTMin, true);
					}
					if (m_Params.ClustersPerTwig > 0)
					{
						CollectClusters(stemIndex, random, SharedLeafCount(stem, ClustersOnParentLevel), 0, false, false, ParentClusterTMin);
					}
				}
				else if (level <= m_Params.Levels - 3)
				{
					// 上位階層の枝先 (幹の主軸の先端 / 主枝の先端) の葉 / 房: 切られていない片に枝先の葉 LeafTerminalCount 枚 (親階層の葉の規則:
					// 葉柄 2 cm、裸枝にしない) + 枝先の房 TerminalClusters 枚 (ClustersPerTwig > 0 のとき)。draw は枚数だけで決まる (切られた片は 0)。
					// 主軸 / 主枝の先端が裸で終わらない (Oak の既定では全ての先端が Taper で 1.2 cm 以下まで細る。枚数は半径に依らない = T9 の不変条件)
					const int tipLeaves = IsCutPiece(stem) ? 0 : ClampInt(m_Params.LeafTerminalCount, 0, 4);
					CollectLeaves(stem, stemIndex, random, tipLeaves, 1.0f, true, true);	// 裸枝の draw は引くが使わない (親階層の規則)
					if (m_Params.ClustersPerTwig > 0)
					{
						CollectClusters(stemIndex, random, 0, IsCutPiece(stem) ? 0 : m_Params.TerminalClusters, false, true);
					}
				}
			}

			m_Stems.push_back(std::move(stem));

			// ---- 再帰 (深さ優先、push 順) ----
			for (const FChildSpec& child : children)
			{
				GenerateStem(child);
			}
		}

		// ============================================================
		//  (B) 出力パス (乱数は引かない)
		// ============================================================

		// ---- 小枝セクションの判定 (Run と同じ規則: 確定した根元半径 < TwigRadiusThreshold)。細い小枝は周 4 分割以下 / 先細り補正なし / 襟なし ----
		bool IsTwig(const FStem& Stem) const { return Stem.BaseRadius < m_Params.TwigRadiusThreshold; }
		bool IsThinTwig(const FStem& Stem) const { return IsTwig(Stem) && Stem.BaseRadius < ThinTwigRadius; }

		// ---- 樹皮 UV: 根元の円周で整数タイルに丸めた U のタイル数と、U と同じテクセル密度にした V の密度 [1/m] (正方テクセル) ----
		//      (分岐のクローンは AssignRadii が親の片の値を継がせる: Stem.TilesAround / Stem.VScale。ここは丸めの規則だけ)
		void ComputeBarkTiling(const FStem& Stem, float& OutTilesAround, float& OutVScale) const
		{
			const float circumference = PT_TWO_PI * fmaxf(Stem.BaseRadius, 1.0e-5f);
			OutTilesAround = fmaxf(1.0f, floorf(circumference * m_Params.BarkUVTilesPerMeter * m_Params.Level[Stem.Level].BarkUVScale + 0.5f));
			OutVScale = OutTilesAround / circumference;
		}
		static void BarkTiling(const FStem& Stem, float& OutTilesAround, float& OutVScale)
		{
			OutTilesAround = Stem.TilesAround;
			OutVScale = Stem.VScale;
		}

		// 幹の根元の張り出し (幹の最初の片だけ): 根元からの距離 Distance [m] での半径倍率 1 + TrunkFlare x (1 - s / (TrunkFlareHeight x 幹長))^2
		float FlareFactor(const FStem& Stem, float Distance) const
		{
			if (Stem.Level != 0 || Stem.bClone || m_Params.TrunkFlare <= 0.0f) return 1.0f;
			const float x = Clamp(1.0f - Distance / fmaxf(m_Params.TrunkFlareHeight * m_TrunkLength, 1.0e-4f), 0.0f, 1.0f);
			return 1.0f + m_Params.TrunkFlare * x * x;
		}

		// 子枝の襟 (根元リングの膨らみ): 根元からの距離 SDist [m] での半径倍率 1 + Collar x (1 - s / (2.5 rc))^2。
		// 分岐で切られた片は先端 (クローンの根元が埋まる) までに襟を消す (2.5 rc より短い片: 先端のリングが確定半径のままでクローンと太さが揃う)
		float CollarFactor(const FStem& Stem, float SDist) const
		{
			if (Stem.Collar <= 0.0f) return 1.0f;
			const float zone = IsCutPiece(Stem) ? fminf(2.5f * Stem.BaseRadius, Stem.Length) : 2.5f * Stem.BaseRadius;
			const float x = 1.0f - Clamp(SDist / fmaxf(zone, 1.0e-6f), 0.0f, 1.0f);
			return 1.0f + Stem.Collar * x * x;
		}

		// リングの頂点カラーの明度: 幹 = 接地ランプ (0.72 -> 1、元の幹の根元からの距離で測るので分岐した片にも続く)、
		// 子枝 = 親の付け根の明度 x 0.75 から 3 rc で 1 へ (継ぎ目を隠す)、クローンは切った先端の明度をそのまま続ける
		float RingShade(const FStem& Stem, float Distance) const
		{
			if (Stem.Level == 0)
			{
				// 接地ランプ (幹全長で 0.72 -> 1) x 根元 0.3 m の接地の陰 (0.82 -> 1)
				const float d = Stem.DistanceOffset + Distance;
				return StemShade(0, d / fmaxf(m_TrunkLength, 1.0e-4f)) * Lerp(0.82f, 1.0f, SmoothStep(0.0f, 0.3f, d));
			}
			if (Stem.bClone) return Stem.ParentShade;
			return Lerp(Stem.ParentShade * 0.75f, 1.0f, SmoothStep(0.0f, 3.0f * Stem.BaseRadius, Distance));
		}

		// 子の付け根 (片の長さ比 T) で子へ引き継ぐ明度: 幹の接地ランプ、それ以外は 1
		float ChildShade(const FStem& Stem, float T) const
		{
			return (Stem.Level == 0) ? RingShade(Stem, T * Stem.Length) : 1.0f;
		}

		// 分岐した片の葉の枚数 (子と同じ取り分で分け合う。片ごとに最低 1 枚)
		static int SharedLeafCount(const FStem& Stem, int Count)
		{
			if (Count <= 0) return 0;
			const int n = SharedCount(Count, Stem.ShareLo, Stem.ShareEnd);
			return (n < 1) ? 1 : n;
		}

		// 子の付け根まわりの親の暗化: 子の根元中心から 1 rc 以内で 0.9、4 rc で 1 へなだらかに戻す (付け根の陰。輪の縁が見えない)。分岐のクローンは暗化しない (同じ枝の続き)
		float AttachDarkening(const FStem& Stem, FXMVECTOR Position) const
		{
			float factor = 1.0f;
			for (int childIndex : Stem.ChildStems)
			{
				const FStem& child = m_Stems[childIndex];
				if (child.bClone) continue;
				const float rc = child.BaseRadius;
				const float dist = sqrtf(LengthSq3(XMVectorSubtract(Position, Load3(child.Points[0].Position))));
				factor = fminf(factor, Lerp(0.9f, 1.0f, SmoothStep(1.0f * rc, 4.0f * rc, dist)));
			}
			return factor;
		}

		// 根元リング (半径 RingRadius、面は枝の最初の接線に直交) の全ての頂点が親の軸から Limit 以内に収まる最大の沈み込み量 d。
		// リング上の点 v(phi) = 中心 + RingRadius (cos phi N0 + sin phi B0) の親の軸からの距離は |d radialDir + v_perp|、
		// v_perp = v から親の接線成分を除いたもの。phi を 32 分割して各点で d の 2 次不等式を解き、最小を取る (0 以上)
		static float MaxSinkInside(const FStem& Stem, const FStemSample& Parent, float RingRadius, float Limit)
		{
			const XMVECTOR t0 = Load3(Stem.Points[0].Tangent);
			const XMVECTOR n0 = Orthonormalize(Load3(Stem.Points[0].Normal), t0);
			const XMVECTOR b0 = SafeNormalize(XMVector3Cross(t0, n0), AnyPerpendicular(t0));
			const XMVECTOR radialDir = Load3(Stem.RootRadialDir);
			const XMVECTOR axis = Parent.Tangent;

			float dMax = 1.0e9f;
			for (int k = 0; k < 32; ++k)
			{
				const float phi = static_cast<float>(k) * (PT_TWO_PI / 32.0f);
				const XMVECTOR w = XMVectorScale(XMVectorAdd(XMVectorScale(n0, cosf(phi)), XMVectorScale(b0, sinf(phi))), RingRadius);
				const XMVECTOR wPerp = XMVectorSubtract(w, XMVectorScale(axis, Dot3(w, axis)));
				const float b = Dot3(radialDir, wPerp);
				const float c = LengthSq3(wPerp);
				const float disc = b * b - c + Limit * Limit;
				if (disc < 0.0f) return 0.0f;	// 中心を軸に置いても収まらない (親より太い子): 沈めない
				dMax = fminf(dMax, -b + sqrtf(disc));
			}
			return fmaxf(dMax, 0.0f);
		}

		// ---- 半径の確定 (出力パス、乱数は引かない)。親は子より先に並ぶので m_Stems の順に:
		//  (1) 子は根元半径を 0.85 x 親の確定半径 (付け根) までに抑える (先端も同じ比で縮める)
		//  (2) パイプモデル (PipeExponent n > 0): r_pipe(f) = (Tip^n + Σ_c w_c(f) r_c^n)^(1/n)、w_c は子の付け根 t_c の手前で 1 -> 0 (SmoothStep)。
		//      r(f) = lerp(テーパ曲線 (TaperShape), r_pipe(f), 0.75): 根元半径を保ち、子を出す毎に段階的に細くなる (どちらも単調非増加)。
		//      子の無い切られた片は一定 Base を CutPieceHold だけ混ぜる。樹皮 V: 分岐のクローンは親の片の先端の V から続ける
		//  (3) 接合部の沈み込み (JunctionSink > 0): 子の根元を親の軸から radialDir x d へ (d = R - JunctionSink rc - rc |cos down|)。
		//      根元リング (襟込み) が親のチューブからはみ出さない範囲に d を抑える。骨格は親の軸上に作ってあるので子孫ごと同じ量だけ平行移動する
		//  (4) 襟の実効倍率 (根元リングが親に収まる範囲に抑えた JunctionCollar) と樹皮 V のオフセット (枝ごとにタイル 1 枚分の中の決定的な位置)
		void AssignRadii()
		{
			std::vector<XMVECTOR> offsets(m_Stems.size(), XMVectorZero());	// 沈み込みによる平行移動 (祖先の分を含む)
			const float n = m_Params.PipeExponent;

			for (size_t index = 0; index < m_Stems.size(); ++index)
			{
				FStem& stem = m_Stems[index];
				const int segments = static_cast<int>(stem.Points.size()) - 1;
				const bool bChild = stem.ParentStem >= 0 && !stem.bClone;
				XMVECTOR offset = (stem.ParentStem >= 0) ? offsets[stem.ParentStem] : XMVectorZero();

				// (1) 親の確定半径 (親は平行移動済み、半径も確定済み)
				FStemSample parent{};
				if (bChild)
				{
					parent = SampleStem(m_Stems[stem.ParentStem], stem.AttachT);
					const float maxBase = 0.85f * parent.Radius;
					if (stem.BaseRadius > maxBase && maxBase > 0.0f)
					{
						const float scale = maxBase / stem.BaseRadius;
						stem.BaseRadius = maxBase;
						stem.TipRadius = fminf(stem.BaseRadius, fmaxf(stem.TipRadius * scale, MinTipRadius));
					}
				}
				else if (stem.bClone && stem.ParentStem >= 0)
				{
					// 分岐のクローン: 根元半径 = 切った片の確定した先端半径 x CloneRadiusRatio (= 断面の取り分^(1 / PipeExponent)、主軸 1 - S / 脇 S。
					// 根元の円盤はその先端の半球の中に埋まる)。MinStemRadius を下回らない (ただし親の先端より太くはしない)
					const float parentTip = m_Stems[stem.ParentStem].Points.back().Radius;
					const float newBase = fmaxf(parentTip * stem.CloneRadiusRatio, fminf(MinStemRadius, parentTip));
					const float scale = newBase / fmaxf(stem.BaseRadius, 1.0e-6f);
					stem.BaseRadius = newBase;
					stem.TipRadius = fminf(stem.BaseRadius, fmaxf(stem.TipRadius * scale, MinTipRadius));
				}

				// (2) 半径の分布。パイプモデルの形 r_pipe(f)^n = Tip^n + (Base^n - Tip^n) x Σ_c w_c r_c^n / Σ_c r_c^n: 根元 = Base、先端 = Tip を保ったまま、
				//     余剰の断面 (Base^n - Tip^n) を子の r_c^n の比で配り、子が出るたびに段階的に細る (子が細すぎても太すぎても根元 / 先端の半径は変わらない。
				//     以前の Base / r_pipe(0) 正規化は子が細いと先端まで太いままになり、主軸の先端が太い切り口で終わっていた)。
				//     分岐で切られた片 (先端は分岐点で、上の全てを支える) は子が無くてもこの配合 (線形テーパ 0.25 + 一定 Base 0.75) にする:
				//     幹の根元の片が最初の分岐まで Taper の直線で細るのを防ぐ (切られていない子無しの枝 (小枝) は線形テーパのまま先端まで細る)
				const bool bPipe = n > 0.0f && (!stem.ChildAttach.empty() || IsCutPiece(stem)) && segments >= 1;
				float tipN = 0.0f, excessN = 0.0f, sumAllN = 0.0f;
				if (bPipe)
				{
					tipN = powf(stem.TipRadius, n);
					excessN = fmaxf(powf(stem.BaseRadius, n) - tipN, 0.0f);
					for (const std::pair<float, float>& c : stem.ChildAttach) sumAllN += powf(c.second, n);
				}
				for (int k = 0; k <= segments; ++k)
				{
					const float f = (segments > 0) ? static_cast<float>(k) / static_cast<float>(segments) : 0.0f;
					float r = Lerp(stem.BaseRadius, stem.TipRadius, TaperShape(stem, Lerp(stem.TaperX0, stem.TaperX1, f)));
					if (bPipe)
					{
						float rPipe = stem.BaseRadius;	// 子が無い切られた片: 一定 (上の全てを支える)
						if (sumAllN > 0.0f)
						{
							float sumW = 0.0f;
							for (const std::pair<float, float>& c : stem.ChildAttach)
							{
								// 子の断面は付け根の直後 (t_c - 0.1 .. t_c + 0.5 セグメント、先端を越えない) で抜ける: 付け根では親はまだ太い
								// (子の 0.85 x 親の上限 / 沈み込みは付け根の半径を使う)。窓を先端で止めるので f = 1 では Tip に戻る
								const float hi = fminf(c.first + 0.5f / static_cast<float>(segments), 1.0f);
								const float w = 1.0f - SmoothStep(hi - 0.6f / static_cast<float>(segments), hi, f);
								sumW += w * powf(c.second, n);
							}
							rPipe = powf(tipN + excessN * (sumW / sumAllN), 1.0f / n);
						}
						// 子の無い切られた片 (幹の根元の片) は一定 Base を CutPieceHold だけ混ぜる (分岐の手前で幹が細って見える)
						r = Lerp(r, rPipe, (sumAllN > 0.0f) ? 0.75f : CutPieceHold);
						// 先端の下限 (MinTipRadius、根元より太くはしない) を守る (単調性は保たれる)
						r = fmaxf(r, fminf(MinTipRadius, stem.BaseRadius));
					}
					stem.Points[k].Radius = r;
				}

				// (3) + (4) 接合部
				stem.Collar = 0.0f;
				if (bChild)
				{
					const float rc = stem.BaseRadius;
					// 襟の実効倍率 (根元リングが親に収まる範囲)。細い小枝は襟を出さないが、沈み込みの判定には襟付きの半径を使う:
					// 沈み込み量が小枝セクションへの振り分け (TwigRadiusThreshold) に依存しない = 振り分けを変えても位置はビット単位で同じ
					const float collar = (m_Params.JunctionCollar > 0.0f) ? fminf(m_Params.JunctionCollar, fmaxf(0.0f, 0.95f * parent.Radius / rc - 1.0f)) : 0.0f;
					if (!IsThinTwig(stem))
					{
						stem.Collar = collar;
					}
					if (m_Params.JunctionSink > 0.0f)
					{
						float d = fmaxf(0.0f, parent.Radius - m_Params.JunctionSink * rc - rc * fabsf(stem.CosDown));
						d = fminf(d, MaxSinkInside(stem, parent, rc * (1.0f + collar), 0.98f * parent.Radius));
						offset = XMVectorAdd(offset, XMVectorScale(Load3(stem.RootRadialDir), fmaxf(d, 0.0f)));
					}
				}
				offsets[index] = offset;
				if (LengthSq3(offset) > 0.0f)
				{
					for (FStemPoint& pt : stem.Points)
					{
						pt.Position = Store3(XMVectorAdd(Load3(pt.Position), offset));
					}
				}

				// 樹皮 V の開始位置: タイル 1 枚分の中の決定的な位置。鍵は枝のサブストリームのシード (木の経路で決まり、
				// 他の枝の数が変わっても動かない。m_Stems の添字は深い階層の Branches で変わる)
				// 分岐のクローンは親の片の先端の V (根元を 0.3 r 戻した分を引く) から続ける: 樹皮の溝が分岐を通って続き、片ごとに模様が始まり直さない。
				// polish3: クローンは親の片の周方向のタイル数と V 密度も継ぐ (細い片では溝が少し詰まるが、本数と位相が分岐で跳ばない。実際の樹皮と同じ)
				if (stem.bClone && stem.ParentStem >= 0)
				{
					const FStem& parentPiece = m_Stems[stem.ParentStem];
					stem.TilesAround = parentPiece.TilesAround;
					stem.VScale = parentPiece.VScale;
					stem.UVOffsetV = parentPiece.Length - 0.3f * parentPiece.Points.back().Radius + parentPiece.UVOffsetV;
				}
				else
				{
					ComputeBarkTiling(stem, stem.TilesAround, stem.VScale);
					stem.UVOffsetV = LatticeHash(static_cast<int>(stem.Seed), 5, m_Params.Seed) / stem.VScale;
				}
			}
		}

		// ---- 配置パラメータ -> カードの原点 / 向き。半径の確定 (T9 AssignRadii) の後、ComputeCrownStats の前に呼ぶ ----
		// 単葉: 方位の基準面 planeN = RMF 法線 (枝に沿って連続) と「枝から見た上」(cross(T, cross(up, T))) を 0.6 で混ぜた面法線
		//  (葉群が空を向く)、radialDir = planeN を接線まわりに Phi 回す。原点 = 枝の表面 + 葉柄 (min(LeafPetioleLength, max(1 cm, 2 r))、
		//  親階層の葉は一律 2 cm。隙間はテクスチャの葉柄が覆う)。r は出力される樹皮の半径そのもの (葉柄の付け根が樹皮に埋まらない)。
		//  向き: 枝先の葉は接線寄り (lerp(T, radialDir, 0.35))、他は接線から Angle。葉面はおおむね上を向かせ Roll だけ捻る
		//  (T3 が樹冠の外向きへ反転することがある)
		// 房 (bCluster): 枝の途中の房は単葉と同じ規則で原点を枝の表面に (葉柄なし。アトラスの小枝が付け根を覆う)。
		//  枝先の房は枝の軸の先端から normalize(lerp(T, up, 0.3)) の向きへ (アトラスの小枝の付け根が実際の枝先に続く)、
		//  +U = RMF 法線を接線まわりに Phi (= j x 90 deg) 回して向きに直交化したもの、面法線 = cross(向き, +U) (T3 が樹冠の外向きへ揃える)
		void ResolveLeafSpecs()
		{
			const XMVECTOR up = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);

			for (FLeafSpec& spec : m_LeafSpecs)
			{
				const FStemSample s = SampleStem(m_Stems[spec.Stem], spec.T);
				XMVECTOR origin, leafDir, side, normal;

				if (spec.bCluster && spec.bTerminal)
				{
					leafDir = SafeNormalize(XMVectorLerp(s.Tangent, up, 0.3f), s.Tangent);
					side = Orthonormalize(RotateAxis(s.Normal, s.Tangent, spec.Phi), leafDir);
					normal = SafeNormalize(XMVector3Cross(leafDir, side), AnyPerpendicular(leafDir));
					origin = s.Position;
				}
				else
				{
					const XMVECTOR radialDir = RotateAxis(PlaneNormal(s), s.Tangent, spec.Phi);

					leafDir = spec.bTerminal
						? SafeNormalize(XMVectorLerp(s.Tangent, radialDir, 0.35f), s.Tangent)
						: SafeNormalize(XMVectorAdd(XMVectorScale(s.Tangent, cosf(spec.Angle)), XMVectorScale(radialDir, sinf(spec.Angle))), s.Tangent);

					const float standOff = spec.bCluster ? 0.0f : spec.bParentLevel
						? 0.02f
						: fminf(m_Params.LeafPetioleLength, fmaxf(0.01f, s.Radius * 2.0f));
					origin = XMVectorAdd(s.Position, XMVectorScale(radialDir, s.Radius + standOff));

					side = XMVector3Cross(up, leafDir);
					side = (LengthSq3(side) < 1.0e-4f) ? AnyPerpendicular(leafDir) : XMVector3Normalize(side);
					normal = XMVector3Normalize(XMVector3Cross(leafDir, side));
					if (XMVectorGetY(normal) < 0.0f)
					{
						side = XMVectorNegate(side);
						normal = XMVectorNegate(normal);
					}
					side = RotateAxis(side, leafDir, spec.Roll);
					normal = RotateAxis(normal, leafDir, spec.Roll);
				}

				// 房カードは運ぶ枝 (片) の長さで縮める (数 cm の小枝に 30 cm の房を付けない)。単葉は縮めない。
				// polish3: さらに Level 1 の枝ごと (±LimbClusterVariance) と小枝ごと (±TwigClusterVariance) に大きさを変え (輪郭の房の大きさが
				// 枝ごとに違う = 大小の塊)、枝の途中の房は付け根を中心に面内で ±ClusterRollDeg 回す (同じ扇が同じ向きで並ばない)。どれも LatticeHash
				if (spec.bCluster)
				{
					const FStem& owner = m_Stems[spec.Stem];
					float scale = Lerp(ClusterMinScale, 1.0f, SmoothStep(ClusterMinLength, ClusterFullLength, owner.Length));
					scale *= 1.0f + LimbClusterVariance * (2.0f * LatticeHash(owner.AncestorL1, 61, m_Params.Seed) - 1.0f);
					scale *= 1.0f + TwigClusterVariance * (2.0f * LatticeHash(static_cast<int>(owner.Seed), 62, m_Params.Seed) - 1.0f);
					spec.Size *= scale;
					spec.HalfWidth *= scale;
					if (!spec.bTerminal)
					{
						const float roll = ClusterRollDeg * PT_DEG2RAD * (2.0f * LatticeHash(spec.Index, 5, m_Params.Seed) - 1.0f);
						leafDir = RotateAxis(leafDir, normal, roll);
						side = Orthonormalize(RotateAxis(side, normal, roll), leafDir);
					}
				}

				spec.Origin = Store3(origin);
				spec.Dir = Store3(leafDir);
				spec.Side = Store3(side);
				spec.FaceN = Store3(normal);
				// 樹冠包絡の間引き: カードの中心が 1.1 R(h) + LeafCullMargin / PruneRatio より外なら出さない (房カードも同じ規則)。
				// PruneRatio 1 では 1.1 R(h) + 0.5 m (予測のずれで包絡を越えた枝の葉 + カード 1 枚分)。PruneRatio < 1 では枝がはみ出しの
				// (1 - PruneRatio) 倍を残して刈られるので余裕も 1 / PruneRatio 倍に広げる (余裕なしだと残した枝が丸裸になる)
				spec.bPruned = m_bPrune && !InsideEnvelope(CardCenter(spec), 1.1f, LeafCullMargin / fmaxf(m_Params.PruneRatio, 1.0e-3f));
			}
		}

		// ---- 樹冠の統計: 中心 / 範囲 / 葉密度グリッド (葉の仕様から。葉が無ければ幹の先端に 1 m の箱) ----
		static float CardArea(const FLeafSpec& Spec, float ClusterAspect)
		{
			// 房カードはアトラスの透過分を差し引いた実効面積 (0.6 倍)
			return Spec.bCluster ? Spec.Size * Spec.Size * ClusterAspect * 0.6f : Spec.Size * 2.0f * Spec.HalfWidth;
		}

		static XMVECTOR CardCenter(const FLeafSpec& Spec)
		{
			return XMVectorAdd(Load3(Spec.Origin), XMVectorScale(Load3(Spec.Dir), 0.5f * Spec.Size));
		}

		void ComputeCrownStats()
		{
			const int n = CrownGridSize;
			m_CrownTau.assign(static_cast<size_t>(n) * n * n, 0.0f);

			XMVECTOR weighted = XMVectorZero();
			float totalArea = 0.0f;
			for (const FLeafSpec& spec : m_LeafSpecs)
			{
				if (spec.bPruned) continue;	// 包絡で間引いた葉は出力されないので統計にも入れない
				const float area = CardArea(spec, m_Params.ClusterAspect);
				weighted = XMVectorAdd(weighted, XMVectorScale(CardCenter(spec), area));
				totalArea += area;
			}

			if (totalArea <= 1.0e-8f)
			{
				const XMFLOAT3 top = (m_Stems.empty() || m_Stems[0].Points.empty())
					? XMFLOAT3{ 0.0f, 0.0f, 0.0f } : m_Stems[0].Points.back().Position;
				m_CrownCenter = top;
				m_CrownRadius = { 1.0f, 1.0f, 1.0f };
				m_CrownMinY = top.y - 1.0f;
				m_CrownHeight = 2.0f;
			}
			else
			{
				const XMVECTOR center = XMVectorScale(weighted, 1.0f / totalArea);
				XMVECTOR maxAbs = XMVectorZero();
				XMVECTOR minP = XMVectorReplicate(FLT_MAX);
				XMVECTOR maxP = XMVectorReplicate(-FLT_MAX);
				for (const FLeafSpec& spec : m_LeafSpecs)
				{
					if (spec.bPruned) continue;
					const XMVECTOR c = CardCenter(spec);
					maxAbs = XMVectorMax(maxAbs, XMVectorAbs(XMVectorSubtract(c, center)));
					minP = XMVectorMin(minP, c);
					maxP = XMVectorMax(maxP, c);
				}
				m_CrownCenter = Store3(center);
				m_CrownRadius = Store3(XMVectorMax(XMVectorScale(maxAbs, 1.15f), XMVectorReplicate(0.3f)));
				m_CrownMinY = XMVectorGetY(minP);
				m_CrownHeight = fmaxf(XMVectorGetY(maxP) - m_CrownMinY, 1.0e-3f);
			}

			m_CrownCellSize = {
				2.0f * m_CrownRadius.x / static_cast<float>(n),
				2.0f * m_CrownRadius.y / static_cast<float>(n),
				2.0f * m_CrownRadius.z / static_cast<float>(n) };

			// セル断面は 3 軸のセル寸法の幾何平均の 2 乗 (軸ごとに寸法が違うため)
			const float cellSide = cbrtf(m_CrownCellSize.x * m_CrownCellSize.y * m_CrownCellSize.z);
			const float tauPerArea = m_Params.LeafAOFalloff / fmaxf(cellSide * cellSide, 1.0e-8f);

			for (const FLeafSpec& spec : m_LeafSpecs)
			{
				if (spec.bPruned) continue;
				const int idx = CrownCellIndex(CardCenter(spec));
				if (idx >= 0)
				{
					m_CrownTau[idx] += tauPerArea * CardArea(spec, m_Params.ClusterAspect);
				}
			}
		}

		// グリッド座標 (セル単位、範囲外も可)
		XMFLOAT3 CrownGridCoord(FXMVECTOR P) const
		{
			const XMFLOAT3 p = Store3(P);
			return {
				(p.x - (m_CrownCenter.x - m_CrownRadius.x)) / m_CrownCellSize.x,
				(p.y - (m_CrownCenter.y - m_CrownRadius.y)) / m_CrownCellSize.y,
				(p.z - (m_CrownCenter.z - m_CrownRadius.z)) / m_CrownCellSize.z };
		}

		// セルの線形添字 (範囲外は -1)
		static int CrownCellIndexOf(float X, float Y, float Z)
		{
			const int n = CrownGridSize;
			const int ix = static_cast<int>(floorf(X));
			const int iy = static_cast<int>(floorf(Y));
			const int iz = static_cast<int>(floorf(Z));
			if (ix < 0 || iy < 0 || iz < 0 || ix >= n || iy >= n || iz >= n) return -1;
			return (iz * n + iy) * n + ix;
		}

		int CrownCellIndex(FXMVECTOR P) const
		{
			const XMFLOAT3 g = CrownGridCoord(P);
			// 範囲内の点はグリッドに入れる (縁の丸め誤差で落ちないようクランプ)
			const float hi = static_cast<float>(CrownGridSize) - 0.5f;
			return CrownCellIndexOf(Clamp(g.x, 0.0f, hi), Clamp(g.y, 0.0f, hi), Clamp(g.z, 0.0f, hi));
		}

		bool InsideCrownBox(FXMVECTOR P) const
		{
			const XMFLOAT3 g = CrownGridCoord(P);
			const float n = static_cast<float>(CrownGridSize);
			return g.x >= 0.0f && g.y >= 0.0f && g.z >= 0.0f && g.x <= n && g.y <= n && g.z <= n;
		}

		// 樹冠中心から P への方向 (軸ごとに R で正規化した楕円体の外向き)
		XMVECTOR CrownDir(FXMVECTOR P) const
		{
			const XMVECTOR rel = XMVectorDivide(XMVectorSubtract(P, Load3(m_CrownCenter)), Load3(m_CrownRadius));
			return SafeNormalize(XMVectorSetW(rel, 0.0f), XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f));
		}

		// P の露出度 (0 = 葉に埋もれている .. 1 = 遮られない)。上半球 6 方向へ 1 セルずつ 5 歩進んで光学的厚さを積む
		float Exposure(FXMVECTOR P) const
		{
			if (m_CrownTau.empty()) return 1.0f;

			// 真上 + 仰角 50 deg x 方位 0 / 72 / 144 / 216 / 288 deg
			static const XMFLOAT3 dirs[OcclusionDirs] =
			{
				{  0.0f,     1.0f,     0.0f     },
				{  0.64279f, 0.76604f, 0.0f     },
				{  0.19863f, 0.76604f, 0.61133f },
				{ -0.52003f, 0.76604f, 0.37782f },
				{ -0.52003f, 0.76604f,-0.37782f },
				{  0.19863f, 0.76604f,-0.61133f },
			};

			const XMFLOAT3 g = CrownGridCoord(P);
			float occlusion = 0.0f;
			for (const XMFLOAT3& d : dirs)
			{
				for (int k = 1; k <= OcclusionSteps; ++k)
				{
					const float s = static_cast<float>(k);
					const int idx = CrownCellIndexOf(g.x + d.x * s, g.y + d.y * s, g.z + d.z * s);
					if (idx >= 0) occlusion += m_CrownTau[idx];
				}
			}
			return expf(-occlusion / static_cast<float>(OcclusionDirs));
		}

		// 樹皮の頂点カラーのうち位置だけで決まる項 (樹冠内の暗化 = 露出度、苔のまだら、子の付け根まわりの暗化)。
		// 太い枝は頂点ごとに、小枝 (半径がまだら / 露出度グリッドのセルより桁違いに細い) はリングごとに 1 回だけ評価する (生成時間の大半がここだった)
		struct FBarkShadeContext
		{
			float Occlusion = 1.0f;		// 樹冠内の暗化 (Lerp(BarkAOInner, 1, 露出度)。樹冠の箱の外 / 無効なら 1)
			float Patch = 0.0f;			// 苔のまだら (低周波ノイズ 0..1。BarkMoss 0 なら 0)
			float Darkening = 1.0f;		// 子の付け根まわりの暗化 (AttachDarkening)
			float Height = 1.0e3f;		// 地面 (木の原点の高さ) からの高さ [m] (根元の苔)
		};

		FBarkShadeContext BarkShadeContext(const FStem& Stem, FXMVECTOR Position) const
		{
			FBarkShadeContext ctx;
			if (m_Params.BarkAOInner < 1.0f && InsideCrownBox(Position))
			{
				ctx.Occlusion = Lerp(m_Params.BarkAOInner, 1.0f, Exposure(Position));
				// polish3: 幹は樹冠の根元 (最も低い葉の中心) より下では暗くしない (開けた幹が樹冠の箱に入っても黒くならない。0.4 m でなじませる)
				if (Stem.Level == 0)
				{
					const float y = XMVectorGetY(Position);
					ctx.Occlusion = Lerp(1.0f, ctx.Occlusion, SmoothStep(m_CrownMinY - 0.1f, m_CrownMinY + 0.3f, y));
				}
			}
			if (m_Params.BarkMoss > 0.0f)
			{
				const XMFLOAT3 p = Store3(Position);
				ctx.Patch = 0.5f + 0.5f * m_Noise.FBM3D(p.x * 1.5f, p.y * 1.5f, p.z * 1.5f, 3, 2.0f, 0.5f);
				ctx.Height = p.y;
			}
			ctx.Darkening = AttachDarkening(Stem, Position);
			return ctx;
		}

		// 樹皮の頂点カラー (決定的、乱数は引かない):
		//  Ramp (幹の接地ランプ / 親からの明度の引き継ぎ) x 子の付け根まわりの暗化 x 樹冠内の暗化 (露出度。樹冠の箱の中だけ)
		//  x 下面をわずかに暗く (空からの光、0.88..1。苔も樹冠内暗化も無効なら掛けない = v1 と同じ明度) x 苔 (上向きの面に低周波ノイズのまだらで緑を乗せる)
		XMFLOAT4 BarkVertexColor(const FBarkShadeContext& Ctx, FXMVECTOR Normal, float Ramp) const
		{
			const float normalY = XMVectorGetY(Normal);
			const bool bShaded = m_Params.BarkMoss > 0.0f || m_Params.BarkAOInner < 1.0f;

			float shade = Ramp * Ctx.Darkening * Ctx.Occlusion;
			if (bShaded)
			{
				shade *= Lerp(0.88f, 1.0f, 0.5f + 0.5f * normalY);
			}

			XMFLOAT4 color{ shade, shade, shade, 1.0f };
			if (m_Params.BarkMoss > 0.0f)
			{
				// 上向きの面の苔 + 幹の根元の苔。根元は地面から BasalMossHeight まで (まだらで高さも揺れる)、種ごとの方位 m_MossDir (LatticeHash、
				// 北側に相当) の側に寄せ、その反対側には付けない。どちらも低周波のまだら (Patch) で斑にする。
				// 色は黄緑 (R / B を下げる。G = 明度はそのまま): 暗い樹皮の上でも苔と分かる
				const float side = SmoothStep(-0.25f, 0.75f, Dot3(Normal, Load3(m_MossDir)));
				const float reach = BasalMossHeight * Lerp(0.55f, 1.0f, Ctx.Patch);
				const float basal = SmoothStep(reach, 0.0f, Ctx.Height) * side * SmoothStep(0.35f, 0.60f, Ctx.Patch);
				const float moss = fminf(m_Params.BarkMoss * SmoothStep(0.3f, 0.9f, normalY) * SmoothStep(0.45f, 0.70f, Ctx.Patch)
					+ fminf(1.6f * m_Params.BarkMoss, 1.0f) * basal, 1.0f);
				color.x *= Lerp(1.0f, 0.68f, moss);
				color.z *= Lerp(1.0f, 0.32f, moss);
			}
			return color;
		}

		// ---- 樹皮チューブの 1 リング分のサンプル (骨格点、または挿入リング: 根元の張り出し / 襟) ----
		struct FRing
		{
			XMVECTOR Position, Tangent, Normal;
			float Radius;		// 出力する半径 (張り出し / 襟込み)
			float Distance;		// 根元からの距離 [m]
			float Shade;		// 頂点カラーの明度 (接地ランプ / 親からの引き継ぎ)
			float Slope;		// -dr/ds (先細りの法線補正。隣のリングとの差分)
		};

		// ---- 小枝のリングごとの位置の項。露出度 / 苔のまだら / 高さ (低周波: 樹冠のグリッドのセル、苔のノイズの ~0.7 m 周期) は端のリングと 3 本おきの
		//      リングで評価して間は距離で線形補間し、子の付け根まわりの暗化 (局所的) は全てのリングで評価する (生成時間の大半が小枝の位置の項だった) ----
		std::vector<FBarkShadeContext> RingContexts(const FStem& Stem, const std::vector<FRing>& Rings) const
		{
			const int count = static_cast<int>(Rings.size());
			std::vector<FBarkShadeContext> out(static_cast<size_t>(count));
			std::vector<int> anchors;
			for (int k = 0; k < count; ++k)
			{
				if (k == 0 || k == count - 1 || k % 3 == 0)
				{
					out[static_cast<size_t>(k)] = BarkShadeContext(Stem, Rings[static_cast<size_t>(k)].Position);
					anchors.push_back(k);
				}
			}
			for (size_t a = 0; a + 1 < anchors.size(); ++a)
			{
				const int k0 = anchors[a], k1 = anchors[a + 1];
				const FBarkShadeContext& c0 = out[static_cast<size_t>(k0)];
				const FBarkShadeContext& c1 = out[static_cast<size_t>(k1)];
				const float d0 = Rings[static_cast<size_t>(k0)].Distance, d1 = Rings[static_cast<size_t>(k1)].Distance;
				for (int k = k0 + 1; k < k1; ++k)
				{
					const float f = Clamp((Rings[static_cast<size_t>(k)].Distance - d0) / fmaxf(d1 - d0, 1.0e-6f), 0.0f, 1.0f);
					FBarkShadeContext& c = out[static_cast<size_t>(k)];
					c.Occlusion = Lerp(c0.Occlusion, c1.Occlusion, f);
					c.Patch = Lerp(c0.Patch, c1.Patch, f);
					c.Height = Lerp(c0.Height, c1.Height, f);
					c.Darkening = AttachDarkening(Stem, Rings[static_cast<size_t>(k)].Position);
				}
			}
			return out;
		}

		// ---- 骨格 -> 樹皮チューブ (閉じた面。SDF の内外判定のため)。bTwig = 小枝セクション (法線を樹冠の外向きへ少し寄せる) ----
		//  リング列 = 骨格点 + 挿入リング (幹の最初の片: 根元の張り出しを解像する 0.03 / 0.06 / 0.10 x 幹長、子枝: 襟の 0.6 rc) を距離順に。
		//  先端は TipRound > 0 なら半球 (35 / 70 deg のリング + 極)、0 なら平らな扇。根元は法線 -T の平らな円盤 (親の中に埋まる)。
		//  頂点の並び: リング (各 radial + 1、継ぎ目を複製) ... -> [半球 35 deg リング, 70 deg リング, 極] または [先端中心]
		//              -> 根元の円盤リング (radial + 1) -> 根元中心
		//  UV: U = 周方向 (根元の円周で整数タイル)、V = 距離 x 同じテクセル密度 (正方テクセル) + 枝ごとのオフセット
		void EmitBark(const FStem& Stem, FProceduralMeshSection& Out, bool bTwig)
		{
			const FProceduralTreeLevelParams& lp = m_Params.Level[Stem.Level];
			const bool bThin = bTwig && Stem.BaseRadius < ThinTwigRadius;
			const int radialParam = (lp.RadialSegments < 3) ? 3 : lp.RadialSegments;
			const int radial = bThin ? ((radialParam < 4) ? radialParam : 4) : radialParam;
			const int ringVerts = radial + 1;	// 継ぎ目の頂点を複製 (UV の連続性)
			const int pointCount = static_cast<int>(Stem.Points.size());
			const float length = fmaxf(Stem.Length, 1.0e-4f);

			float tilesAround, vScale;
			BarkTiling(Stem, tilesAround, vScale);

			// 小枝は樹冠の中の葉と一緒に陰影が付くよう、法線を少しだけ樹冠の外向きへ寄せる。
			// polish3: 半径の 4 倍より短い切り株 (刈り込みで数 mm になった枝) は寄せない (リングが重なって潰れた面が寄せで裏返る)
			const float twigBlend = (bTwig && Stem.Length > 4.0f * Stem.BaseRadius) ? TwigNormalBlend * m_Params.LeafNormalBlend : 0.0f;

			// ---- 挿入リングの距離 ----
			std::vector<float> extra;
			if (Stem.Level == 0 && !Stem.bClone && m_Params.TrunkFlare > 0.0f)
			{
				extra.push_back(0.03f * m_TrunkLength);
				extra.push_back(0.06f * m_TrunkLength);
				extra.push_back(0.10f * m_TrunkLength);
			}
			if (Stem.Collar > 0.0f && pointCount > 1)
			{
				extra.push_back(fminf(0.6f * Stem.BaseRadius, 0.5f * Stem.Points[1].Distance));
			}
			std::sort(extra.begin(), extra.end());

			// ---- リング列 (距離順。骨格点と重なる挿入リングは捨てる) ----
			std::vector<FRing> rings;
			rings.reserve(static_cast<size_t>(pointCount) + extra.size());
			auto pushRing = [&](FXMVECTOR P, FXMVECTOR T, FXMVECTOR N, float Radius, float Distance)
				{
					FRing ring;
					ring.Position = P;
					ring.Tangent = T;
					ring.Normal = N;
					ring.Radius = Radius * FlareFactor(Stem, Distance) * CollarFactor(Stem, Distance);
					ring.Distance = Distance;
					ring.Shade = RingShade(Stem, Distance);
					ring.Slope = 0.0f;
					rings.push_back(ring);
				};
			// 挿入リングが骨格点と重なる判定の幅: 幹の張り出しリングは片の長さの 1e-3 (長さ比で置くので)、子枝の襟のリング (0.6 rc) は
			// 絶対値 (細く長い枝で 0.6 rc が長さの 1e-3 を下回っても襟のリングを落とさない = 子枝は常に襟のリングを持つ)
			const float eps = (Stem.Level == 0) ? 1.0e-3f * length : 1.0e-7f;
			size_t e = 0;
			for (int i = 0; i < pointCount; ++i)
			{
				const FStemPoint& pt = Stem.Points[i];
				for (; e < extra.size() && extra[e] < pt.Distance - eps; ++e)
				{
					if (extra[e] <= rings.back().Distance + eps) continue;
					const FStemSample s = SampleStem(Stem, extra[e] / length);
					pushRing(s.Position, s.Tangent, s.Normal, s.Radius, extra[e]);
				}
				while (e < extra.size() && extra[e] <= pt.Distance + eps) ++e;
				pushRing(Load3(pt.Position), Load3(pt.Tangent), Load3(pt.Normal), pt.Radius, pt.Distance);
			}
			const int ringCount = static_cast<int>(rings.size());

			// 先細りの法線補正 (-dr/ds): 隣のリングとの差分 (端は片側差分)。細い小枝は補正しない
			if (!bThin && ringCount > 1)
			{
				for (int k = 0; k < ringCount; ++k)
				{
					const FRing& prev = rings[(k > 0) ? k - 1 : k];
					const FRing& next = rings[(k + 1 < ringCount) ? k + 1 : k];
					rings[k].Slope = -(next.Radius - prev.Radius) / fmaxf(next.Distance - prev.Distance, 1.0e-5f);
				}
			}

			const unsigned int base = static_cast<unsigned int>(Out.Vertices.size());

			// 巻き順の基準はブレンド前の幾何法線 (リング = 半径方向 + 先細り補正、キャップ = ±接線)。
			// 法線を寄せても巻き順 (閉じた面) がブレンド無しと同じになる
			std::vector<XMFLOAT3> refNormals;
			refNormals.reserve(static_cast<size_t>(ringCount + 2) * ringVerts + 2);
			auto addBarkTriangle = [&](unsigned int I0, unsigned int I1, unsigned int I2)
				{
					const XMVECTOR reference = XMVectorAdd(XMVectorAdd(
						Load3(refNormals[I0 - base]), Load3(refNormals[I1 - base])), Load3(refNormals[I2 - base]));
					AddTriangle(Out, I0, I1, I2, reference);
				};
			// 頂点 1 つ: 法線を寄せる分だけ接線も直交化し直す (シェーダは直交化しない)。色 = 明度 x 位置の項 (RingCtx: 小枝はリングごと、太い枝は頂点ごとに評価)
			auto pushVertex = [&](FXMVECTOR Position, FXMVECTOR GeometricNormal, FXMVECTOR Tangent, const XMFLOAT2& UV, float Shade, const FBarkShadeContext* RingCtx)
				{
					refNormals.push_back(Store3(GeometricNormal));
					XMVECTOR normal = GeometricNormal;
					XMVECTOR tangent = Tangent;
					if (twigBlend > 0.0f)
					{
						normal = SafeNormalize(XMVectorLerp(normal, CrownDir(Position), twigBlend), normal);
						tangent = Orthonormalize(tangent, normal);
					}
					Out.Vertices.push_back(MakeVertex(Position, normal, tangent, UV,
						BarkVertexColor(RingCtx ? *RingCtx : BarkShadeContext(Stem, Position), normal, Shade)));
				};
			// リング 1 周 (継ぎ目を複製)。Radius / 中心 / 幾何法線の成分 (RadialWeight x 半径方向 + TangentWeight x 接線) / V を指定
			// 小枝は位置の項をリングごとに 1 回評価し (RingContexts)、先端の半球 / 根元の円盤 / 極の頂点は端のリングの値を使い回す (数 mm しか離れていない)
			const std::vector<FBarkShadeContext> ringCtxs = bTwig ? RingContexts(Stem, rings) : std::vector<FBarkShadeContext>();
			const FBarkShadeContext firstCtx = bTwig ? ringCtxs.front() : FBarkShadeContext{};
			const FBarkShadeContext tipCtx = bTwig ? ringCtxs.back() : FBarkShadeContext{};
			// 周方向の cos / sin (リングごとに同じなので枝 1 本で 1 回)
			float cosTable[33], sinTable[33];
			for (int j = 0; j < ringVerts && j < 33; ++j)
			{
				const float theta = static_cast<float>(j) / static_cast<float>(radial) * PT_TWO_PI;
				cosTable[j] = cosf(theta);
				sinTable[j] = sinf(theta);
			}
			auto pushRingVertices = [&](const FRing& Ring, float Radius, FXMVECTOR Center, float RadialWeight, float TangentWeight, float V, const FBarkShadeContext* Shared)
				{
					const XMVECTOR B = SafeNormalize(XMVector3Cross(Ring.Tangent, Ring.Normal), AnyPerpendicular(Ring.Tangent));
					const FBarkShadeContext ringCtx = (bTwig && Shared) ? *Shared : FBarkShadeContext{};
					for (int j = 0; j < ringVerts; ++j)
					{
						const float c = cosTable[j];
						const float s = sinTable[j];
						const XMVECTOR radialDir = XMVectorAdd(XMVectorScale(Ring.Normal, c), XMVectorScale(B, s));
						const XMVECTOR position = XMVectorAdd(Center, XMVectorScale(radialDir, Radius));
						const XMVECTOR normal = SafeNormalize(XMVectorAdd(XMVectorScale(radialDir, RadialWeight), XMVectorScale(Ring.Tangent, TangentWeight)), radialDir);
						// +U 方向 = 周方向 (cross(radial, dRadial/dTheta) = +T で dP/dV = cross(N, dP/dU) を満たす)
						const XMVECTOR tangent = XMVectorAdd(XMVectorScale(Ring.Normal, -s), XMVectorScale(B, c));
						pushVertex(position, normal, tangent, { static_cast<float>(j) / static_cast<float>(radial) * tilesAround, V }, Ring.Shade, bTwig ? &ringCtx : nullptr);
					}
				};
			auto connectRings = [&](unsigned int RingA, unsigned int RingB)
				{
					for (int j = 0; j < radial; ++j)
					{
						const unsigned int a = RingA + static_cast<unsigned int>(j);
						const unsigned int b = a + 1;
						const unsigned int c = RingB + static_cast<unsigned int>(j);
						const unsigned int d = c + 1;
						addBarkTriangle(a, b, c);
						addBarkTriangle(b, d, c);
					}
				};
			auto fan = [&](unsigned int Ring, unsigned int Center)
				{
					for (int j = 0; j < radial; ++j)
					{
						addBarkTriangle(Ring + static_cast<unsigned int>(j), Ring + static_cast<unsigned int>(j) + 1, Center);
					}
				};

			// ---- リング ----
			for (int k = 0; k < ringCount; ++k)
			{
				const FRing& ring = rings[k];
				pushRingVertices(ring, ring.Radius, ring.Position, 1.0f, ring.Slope, (ring.Distance + Stem.UVOffsetV) * vScale, bTwig ? &ringCtxs[k] : nullptr);
			}
			for (int k = 0; k + 1 < ringCount; ++k)
			{
				connectRings(base + static_cast<unsigned int>(k * ringVerts), base + static_cast<unsigned int>((k + 1) * ringVerts));
			}

			// ---- 先端 ----
			{
				const FRing& tip = rings.back();
				const float tipRound = Clamp(lp.TipRound, 0.0f, 1.0f);
				unsigned int lastRing = base + static_cast<unsigned int>((ringCount - 1) * ringVerts);
				float apexOffset = 0.0f;
				if (tipRound > 0.0f)
				{
					// 半球: 中心 = 先端 + T h sin a、半径 r cos a、法線 = cos a radial + sin a T、V は弧長ぶん進める
					const float h = tipRound * tip.Radius;
					apexOffset = h;
					static const float angles[2] = { 35.0f * PT_DEG2RAD, 70.0f * PT_DEG2RAD };
					for (float a : angles)
					{
						const unsigned int ringStart = static_cast<unsigned int>(Out.Vertices.size());
						pushRingVertices(tip, tip.Radius * cosf(a), XMVectorAdd(tip.Position, XMVectorScale(tip.Tangent, h * sinf(a))),
							cosf(a), sinf(a), (tip.Distance + h * sinf(a) + Stem.UVOffsetV) * vScale, &tipCtx);
						connectRings(lastRing, ringStart);
						lastRing = ringStart;
					}
				}
				const XMVECTOR apex = XMVectorAdd(tip.Position, XMVectorScale(tip.Tangent, apexOffset));
				const unsigned int apexIndex = static_cast<unsigned int>(Out.Vertices.size());
				pushVertex(apex, tip.Tangent, tip.Normal, { tilesAround * 0.5f, (tip.Distance + apexOffset + Stem.UVOffsetV) * vScale }, tip.Shade, bTwig ? &tipCtx : nullptr);
				fan(lastRing, apexIndex);
			}

			// ---- 根元 (平らな円盤。親の中に埋まる)。側面のリング頂点は共有せず、法線 -T の頂点を複製する:
			//      襟で +T 側へ傾いた側面の法線を円盤の扇に使うと、頂点法線の和が -T と食い違う (巻き順判定 / 小枝の法線ブレンド) ----
			{
				const FRing& root = rings.front();
				const XMVECTOR N = XMVectorNegate(root.Tangent);
				const unsigned int discRing = static_cast<unsigned int>(Out.Vertices.size());
				pushRingVertices(root, root.Radius, root.Position, 0.0f, -1.0f, Stem.UVOffsetV * vScale, &firstCtx);
				const unsigned int rootIndex = static_cast<unsigned int>(Out.Vertices.size());
				pushVertex(root.Position, N, root.Normal, { tilesAround * 0.5f, Stem.UVOffsetV * vScale }, root.Shade, bTwig ? &firstCtx : nullptr);
				fan(discRing, rootIndex);
			}
		}

		// 回転の向きを規約に依存せず決める: Axis まわりの正の微小回転で dot(V, Toward) が増えるなら +1、減るなら -1
		static float RotationSignToward(FXMVECTOR V, FXMVECTOR Axis, FXMVECTOR Toward)
		{
			const XMVECTOR probe = RotateAxis(V, Axis, 0.05f);
			return (Dot3(probe, Toward) >= Dot3(V, Toward)) ? 1.0f : -1.0f;
		}

		// ---- 葉の仕様 -> カード (単葉はセクション 1、房はセクション 2) ----
		// ジオメトリは平らな四角形のまま、法線 (樹冠の外向きへ寄せる + 垂れ + 皿状) と
		// 頂点カラー (樹冠内の暗化 / 日向 = 黄緑、日陰 = 青緑 / 枝ごとの色相 / 枯れ葉 / 縁の削れ α) で陰影を付ける
		void EmitLeaves()
		{
			const XMVECTOR up = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
			const XMVECTOR one = XMVectorReplicate(1.0f);
			const XMVECTOR hueYellow = XMVectorSet(1.10f, 1.02f, 0.70f, 1.0f);	// 日向の葉 (黄緑へ)
			const XMVECTOR hueBlue = XMVectorSet(0.80f, 0.98f, 1.05f, 1.0f);	// 日陰の葉 (青緑へ)
			const XMVECTOR deadColor = XMVectorSet(0.62f, 0.40f, 0.16f, 1.0f);	// 枯れ葉 (茶)
			const XMVECTOR leafTint = XMVectorSet(m_Params.LeafTint.x, m_Params.LeafTint.y, m_Params.LeafTint.z, 1.0f);
			const float cupRad = m_Params.LeafCup * PT_DEG2RAD;

			for (const FLeafSpec& spec : m_LeafSpecs)
			{
				if (spec.bPruned) continue;	// 樹冠包絡の外 (頂点も出さない。NumLeaves は出力したカードだけ数える)
				FProceduralMeshSection& out = spec.bCluster ? m_Clusters : m_Leaves;

				const XMVECTOR origin = Load3(spec.Origin);
				const XMVECTOR dir = Load3(spec.Dir);
				XMVECTOR side = Load3(spec.Side);
				XMVECTOR faceN = Load3(spec.FaceN);

				// ---- 樹冠から見た位置: 外向き方向と露出度 (葉密度グリッド) ----
				const XMVECTOR center = CardCenter(spec);
				const XMVECTOR nSph = CrownDir(center);
				const float exposure = Exposure(center);
				// 枯れ葉は単葉だけ (房 1 枚 = 葉 5〜6 枚分の茶色い塊になり、LeafDeadFraction の見た目を房が支配してしまう)
				const bool bDead = !spec.bCluster && spec.U2 < m_Params.LeafDeadFraction;

				// 面法線 (= +U 側から見た表) を樹冠の外側に揃える。Side も一緒に反転するので dP/dV = cross(N, Side) = -Dir は保たれる
				if (Dot3(faceN, nSph) < 0.0f)
				{
					faceN = XMVectorNegate(faceN);
					side = XMVectorNegate(side);
				}

				// ---- 曲げた法線: 面法線を樹冠の外向きへ寄せて上向き成分を足す。面から MinLeafNormalDot 以上は離さない (巻き順) ----
				XMVECTOR nBlend = SafeNormalize(XMVectorAdd(
					XMVectorLerp(faceN, nSph, m_Params.LeafNormalBlend), XMVectorScale(up, m_Params.LeafNormalUpBias)), faceN);
				nBlend = ClampToHemisphere(nBlend, faceN, MinLeafNormalDot);

				// 葉先側の法線を枝側 (-Dir) へ倒す (垂れ)。葉ごとに ±min(15 deg, LeafDroop) 散らす (LeafDroop 0 なら散らさない = 面法線のまま)、
				// 枯れ葉はさらに 20 deg
				const float droopRad = (m_Params.LeafDroop + (spec.U1 - 0.5f) * 2.0f * fminf(15.0f, m_Params.LeafDroop)
					+ (bDead ? 20.0f : 0.0f)) * PT_DEG2RAD;
				const float droopSign = RotationSignToward(nBlend, side, XMVectorNegate(dir));
				const XMVECTOR nTip = ClampToHemisphere(RotateAxis(nBlend, side, droopSign * droopRad), faceN, MinLeafNormalDot);

				// 左右の縁の法線を内側へ倒す (皿状): 左 (-Side) の列は +Side へ、右の列は -Side へ
				const float cupSign = RotationSignToward(nBlend, dir, side);
				XMVECTOR normals[4] =
				{
					ClampToHemisphere(RotateAxis(nBlend, dir,  cupSign * cupRad), faceN, MinLeafNormalDot),	// 付け根 左
					ClampToHemisphere(RotateAxis(nBlend, dir, -cupSign * cupRad), faceN, MinLeafNormalDot),	// 付け根 右
					ClampToHemisphere(RotateAxis(nTip,   dir, -cupSign * cupRad), faceN, MinLeafNormalDot),	// 葉先 右
					ClampToHemisphere(RotateAxis(nTip,   dir,  cupSign * cupRad), faceN, MinLeafNormalDot),	// 葉先 左
				};

				// ---- 頂点カラー ----
				// 樹冠内 (露出度が低い) と樹冠の下側を暗く。0.45 を下回らない (樹冠の塊が黒い切り絵にならない)
				const float ao = Clamp(Lerp(m_Params.LeafAOInner, 1.0f, exposure)
					* Lerp(m_Params.LeafAOBottom, 1.0f, 0.5f + 0.5f * XMVectorGetY(nSph)), 0.45f, 1.0f);

				XMVECTOR color;
				if (bDead)
				{
					color = XMVectorScale(XMVectorMultiply(leafTint, deadColor), ao);
				}
				else
				{
					// 同じ Level 1 の枝に付く葉は似た色相に (枝ごとのオフセット)
					const int ancestorL1 = (spec.Stem >= 0) ? m_Stems[spec.Stem].AncestorL1 : -1;
					const float branchOffset = m_Params.LeafBranchVariance * (LatticeHash(ancestorL1, 7, m_Params.Seed) - 0.5f);
					// カードごとの値 U0 を低周波の空間ノイズ (0.6 m) と半々に混ぜる: 同じ房の中で隣の葉が明暗に飛ばず、色が樹冠の上をなだらかに移る
					const XMFLOAT3 c = Store3(center);
					const float spatial = Clamp(0.5f + 0.5f * m_Noise.FBM3D(c.x / 0.6f + 17.0f, c.y / 0.6f, c.z / 0.6f, 2, 2.0f, 0.5f) * 1.4f, 0.0f, 1.0f);
					const float u0 = 0.5f * spec.U0 + 0.5f * spatial;
					const float yellow = Clamp(0.5f * exposure + 0.5f * u0 - 0.25f + branchOffset, 0.0f, 1.0f);
					const float blue = Clamp(0.5f * (1.0f - exposure) + 0.5f * (1.0f - u0) - 0.25f - branchOffset, 0.0f, 1.0f);

					color = XMVectorScale(leafTint, ao * (1.0f + m_Params.LeafTintVariance * (u0 - 0.5f)));
					color = XMVectorMultiply(color, XMVectorLerp(one, hueYellow, m_Params.LeafHueVariance * yellow));
					color = XMVectorMultiply(color, XMVectorLerp(one, hueBlue, m_Params.LeafHueVariance * blue));

					// 樹冠の上ほど緑を少し強く (下 0.94 .. 上 1.04)。色相のばらつきの一部なので LeafHueVariance で効き目を決める
					const float height = Clamp((XMVectorGetY(center) - m_CrownMinY) / m_CrownHeight, 0.0f, 1.0f);
					const float greenGain = Lerp(1.0f, Lerp(0.94f, 1.04f, height), m_Params.LeafHueVariance);
					color = XMVectorSetY(color, XMVectorGetY(color) * greenGain);
				}

				// 1 を超える成分は全体を割って収める (Lumen カードの RGBA8 アルベドと浮動小数の GBuffer で色が食い違わないように)。
				// 下限 0.1: 極端な設定でも真っ黒な葉を出さない
				XMFLOAT4 vertexColor;
				XMStoreFloat4(&vertexColor, color);
				const float maxComponent = fmaxf(vertexColor.x, fmaxf(vertexColor.y, vertexColor.z));
				if (maxComponent > 1.0f)
				{
					vertexColor.x /= maxComponent;
					vertexColor.y /= maxComponent;
					vertexColor.z /= maxComponent;
				}
				vertexColor.x = Clamp(vertexColor.x, 0.1f, 1.0f);
				vertexColor.y = Clamp(vertexColor.y, 0.1f, 1.0f);
				vertexColor.z = Clamp(vertexColor.z, 0.1f, 1.0f);
				// 縁の削れ (マスクは テクスチャ α x 頂点 α。カバレッジ保存ミップの前提 >= 0.7)
				vertexColor.w = Clamp(1.0f - m_Params.LeafErosion * LatticeHash(spec.Index, 3, m_Params.Seed), 0.7f, 1.0f);

				// ---- 房のセルの左右反転 (bMirror): 表裏を入れ替える (+U = -Side、面法線 / 頂点法線も反転)。外から見ると裏面になり、
				//      TwoSided の裏面は法線を反転して陰影を付けるので明るさは同じまま、アトラスの絵だけが左右反転する
				//      (dP/dV = cross(N, +U) = -Dir の規約と巻き順 (面法線 = 頂点法線側) はそのまま保たれる) ----
				if (spec.bCluster && spec.bMirror)
				{
					side = XMVectorNegate(side);
					faceN = XMVectorNegate(faceN);
					for (XMVECTOR& nv : normals) nv = XMVectorNegate(nv);
				}

				// ---- ジオメトリ (平らな四角形)。枯れ葉は 0.7 倍 (幅も同じ比率で、テクスチャが伸びないように) ----
				const float sizeScale = bDead ? 0.7f : 1.0f;
				const XMVECTOR tipPos = XMVectorAdd(origin, XMVectorScale(dir, spec.Size * sizeScale));
				const XMVECTOR sideOffset = XMVectorScale(side, spec.HalfWidth * sizeScale);
				const XMVECTOR positions[4] =
				{
					XMVectorSubtract(origin, sideOffset),
					XMVectorAdd(origin, sideOffset),
					XMVectorAdd(tipPos, sideOffset),
					XMVectorSubtract(tipPos, sideOffset),
				};
				// UV: 単葉はテクスチャ全面。房は 2x2 アトラスのセル (AtlasCell、原点 ((c & 1), (c >> 1)) x 0.5) から AtlasCellInset を除いた範囲
				// (uv = セル原点 + inset + (u, v) x (0.5 - 2 inset)。ベイカ (ProceduralTreeTextures.cpp) と共有の式)。どちらも房 / 葉の先端が V = 0
				XMFLOAT2 uvs[4] = { { 0.0f, 1.0f }, { 1.0f, 1.0f }, { 1.0f, 0.0f }, { 0.0f, 0.0f } };
				if (spec.bCluster)
				{
					const float inset = FProceduralTreeTextures::AtlasCellInset;
					const float span = 0.5f - 2.0f * inset;
					const float cellX = static_cast<float>(spec.AtlasCell & 1) * 0.5f;
					const float cellY = static_cast<float>(spec.AtlasCell >> 1) * 0.5f;
					for (XMFLOAT2& uv : uvs)
					{
						uv = { cellX + inset + uv.x * span, cellY + inset + uv.y * span };
					}
				}

				// V = 1 が付け根、V = 0 が葉先 (dP/dV = -Dir = cross(N, Side))。
				// 接線は +U (Side) を各頂点の法線に直交化したもの (シェーダは直交化しないので)
				const unsigned int b = static_cast<unsigned int>(out.Vertices.size());
				for (int v = 0; v < 4; ++v)
				{
					const XMVECTOR tangent = Orthonormalize(side, normals[v]);
					out.Vertices.push_back(MakeVertex(positions[v], normals[v], tangent, uvs[v], vertexColor));
				}

				const size_t indicesBefore = out.Indices.size();
				AddTriangle(out, b + 0, b + 1, b + 2, faceN);
				AddTriangle(out, b + 0, b + 2, b + 3, faceN);

				// 実際に三角形が出たカードだけ数える (縮退カードは数えない)
				if (out.Indices.size() > indicesBefore)
				{
					if (spec.bCluster) ++m_NumClusters;
					else               ++m_NumLeaves;
				}
			}
		}

	public:
		explicit FTreeGenerator(const FProceduralTreeParams& Params)
			: m_Params(Params)
			, m_Noise(Params.Seed)
		{
			m_Bark.MaterialIndex = FProceduralTreeBuilder::BarkMaterialIndex;
			m_Leaves.MaterialIndex = FProceduralTreeBuilder::LeafMaterialIndex;
			m_Leaves.bAffectDistanceField = false;	// 開いた面は SDF の符号判定を乱す
			m_Clusters.MaterialIndex = FProceduralTreeBuilder::ClusterMaterialIndex;
			m_Clusters.bAffectDistanceField = false;
			m_Twig.MaterialIndex = FProceduralTreeBuilder::TwigMaterialIndex;
			m_Twig.bAffectDistanceField = false;	// 64^3 のボクセルより細いのでベイクに入れない
			const float mossAzimuth = LatticeHash(0, 78, Params.Seed) * PT_TWO_PI;
			m_MossDir = { cosf(mossAzimuth), 0.0f, sinf(mossAzimuth) };
		}

		FProceduralMeshData Run(FProceduralTreeStats* OutStats)
		{
			// ---- (A) 骨格パス。木全体のストリームからは幹の長さだけ引く ----
			FTreeRandom rootRandom(m_Params.Seed);
			const FProceduralTreeLevelParams& trunk = m_Params.Level[0];
			const float length = fmaxf(0.05f, trunk.Length * (1.0f + rootRandom.Signed(trunk.LengthVariance)));

			FChildSpec root;
			root.Origin = { 0.0f, -m_Params.GroundOffset, 0.0f };
			// 幹の傾き = Level[0].DownAngle (鉛直からの角、MaxTrunkLeanDeg まで。DownAngleVariance は使わない)、方位は LatticeHash。
			// 鉛直な杭に見えないよう根元から少し傾け、幹の屈光性 / 揺れで上をなだらかに戻す。Sapling (v1) は 0 = 鉛直
			{
				const float lean = Clamp(trunk.DownAngle, 0.0f, MaxTrunkLeanDeg) * PT_DEG2RAD;
				const float azimuth = LatticeHash(0, 77, m_Params.Seed) * PT_TWO_PI;
				root.Direction = { sinf(lean) * cosf(azimuth), cosf(lean), sinf(lean) * sinf(azimuth) };
				root.Normal = Store3(Orthonormalize(XMVectorSet(1.0f, 0.0f, 0.0f, 0.0f), Load3(root.Direction)));
			}
			root.Length = length;
			root.Radius = trunk.Radius;
			root.Level = 0;
			root.ParentStem = -1;
			root.AttachT = 0.0f;
			root.ParentShade = 1.0f;
			root.ParentRadius = 0.0f;
			root.SegmentsRemaining = (trunk.Segments < 1) ? 1 : trunk.Segments;
			root.SegmentsTotal = root.SegmentsRemaining;
			root.bClone = false;
			root.Seed = HashCombine(m_Params.Seed, 0u, 0u);
			root.RadialDir = { 1.0f, 0.0f, 0.0f };
			root.CosDown = 1.0f;
			root.SegmentStart = 0;
			root.ShareLo = 0.0f;
			root.ShareHi = 1.0f;
			root.SplitError = 0.0f;
			root.CloneRadiusRatio = 0.0f;
			root.DistanceOffset = 0.0f;
			root.KeepFromVertical = 0.0f;
			root.TipScale = 1.0f;
			root.UprightBend = 0.0f;
			root.KinkSeed = root.Seed;
			m_TrunkLength = length;

			GenerateStem(root);

			// ---- (B) 出力パス: 半径を確定 (パイプモデル / 親による上限 / 接合部の沈み込み) -> 葉の原点 / 向きを解く -> 樹冠の統計 (中心 / 葉密度) -> ジオメトリ ----
			AssignRadii();
			ResolveLeafSpecs();
			ComputeCrownStats();

			// 出力バッファを先に確保する (数万頂点の push_back の再確保が生成時間に効く)。リング = 骨格点 + 挿入リング (最大 4) + 半球 2 + 円盤 1
			{
				size_t barkVerts = 0, twigVerts = 0;
				for (const FStem& stem : m_Stems)
				{
					const size_t ringVerts = static_cast<size_t>(ClampInt(m_Params.Level[stem.Level].RadialSegments, 3, 32)) + 1;
					const size_t verts = (stem.Points.size() + 7) * ringVerts + 2;
					(stem.BaseRadius < m_Params.TwigRadiusThreshold ? twigVerts : barkVerts) += verts;
				}
				m_Bark.Vertices.reserve(barkVerts);
				m_Bark.Indices.reserve(barkVerts * 6);
				m_Twig.Vertices.reserve(twigVerts);
				m_Twig.Indices.reserve(twigVerts * 6);
				size_t leafCards = 0, clusterCards = 0;
				for (const FLeafSpec& spec : m_LeafSpecs) (spec.bCluster ? clusterCards : leafCards) += 1;
				m_Leaves.Vertices.reserve(leafCards * 4);
				m_Leaves.Indices.reserve(leafCards * 6);
				m_Clusters.Vertices.reserve(clusterCards * 4);
				m_Clusters.Indices.reserve(clusterCards * 6);
			}

			for (const FStem& stem : m_Stems)
			{
				const bool bTwig = stem.BaseRadius < m_Params.TwigRadiusThreshold;
				EmitBark(stem, bTwig ? m_Twig : m_Bark, bTwig);
				if (bTwig) ++m_NumTwigStems;
			}
			EmitLeaves();

			// セクションは常に NumSections 本 (添字 = MaterialIndex)
			FProceduralMeshData data;
			data.Sections.push_back(std::move(m_Bark));
			data.Sections.push_back(std::move(m_Leaves));
			data.Sections.push_back(std::move(m_Clusters));
			data.Sections.push_back(std::move(m_Twig));

			if (OutStats)
			{
				*OutStats = FProceduralTreeStats{};
				OutStats->NumStems = static_cast<unsigned int>(m_Stems.size());
				OutStats->NumTwigStems = m_NumTwigStems;
				OutStats->NumLeaves = m_NumLeaves;
				OutStats->NumClusters = m_NumClusters;
				OutStats->NumVertices = data.GetNumVertices();
				OutStats->NumTriangles = data.GetNumTriangles();
				for (unsigned int i = 0; i < FProceduralTreeBuilder::NumSections && i < data.Sections.size(); ++i)
				{
					OutStats->NumTrianglesPerSection[i] = data.Sections[i].GetNumTriangles();
					if (data.Sections[i].bAffectDistanceField)
					{
						OutStats->NumDistanceFieldTriangles += data.Sections[i].GetNumTriangles();
					}
				}
			}
			return data;
		}
	};
}



// ============================================================
//  FProceduralTreeParams
// ============================================================

FProceduralTreeParams::FProceduralTreeParams()
{
	// 既定値 = Oak5m プリセット (約 5 m の広葉樹、4 階層: 幹 (二又の主幹 + 分岐する主軸) / 主枝 7 本 / 側枝 / 葉の付く小枝)。
	// グローバル値はヘッダのメンバ初期化子が v2 の名目値、ここで Oak5m 用に上書きする (樹冠包絡 / 葉の枚数 / 大きさ)。
	// 狙い (polish3: 開けた場所で育ったオーク): 幹の根元半径 0.17 m、4 deg 傾いた幹が 2 m で非対称に二又し、脇の主幹は開いてから立ち上がる。
	// 高さ ~5 m、樹冠は幅 ~6 m の平たい丸屋根 (主軸の先は葉の中で止まり、頂は包絡の天井で切りそろう)。主枝は少数で太く (親の 0.45 倍以上)、
	// 下の主枝は水平に張り出し、主枝 / 側枝は屈曲してうねる。側枝は主枝の外側 70 % に付き、樹冠の内側に骨格の枝と空が見える

	// ---- 樹冠包絡: 最大半径 0.8 x 樹冠の高さ、山は樹冠の 62 % の高さ (頂を平たく)、下側はべき 0.3 (低い主枝も横へ張る)、上側は 0.7 ----
	PruneRatio = 0.5f;
	PruneWidth = 0.80f;
	PruneWidthPeak = 0.62f;
	PrunePowerLow = 0.7f;
	PrunePowerHigh = 0.3f;

	// ---- 幹 / 樹皮: 根元の張り出し 0.6 (下 0.1 x 幹長で消える板根)、2.2 cm 未満の枝は小枝セクション (滑らかな若い樹皮。SDF 非寄与)。
	//      主枝 (2.2 cm 以上) は溝のある樹皮になる。樹冠内の暗化は弱め (0.85)、幹は樹冠の根元より下では暗くしない ----
	TrunkFlare = 0.6f;
	TrunkFlareHeight = 0.10f;
	TwigRadiusThreshold = 0.022f;
	BarkAOInner = 0.85f;
	BarkMoss = 0.55f;	// 上向きの面 + 幹の根元の苔 (種ごとの方位の側にまだらに)

	// ---- 葉: 小枝 1 本に 6 枚 (枝先 1 枚)、側枝の胴に 4 枚 (+ 房 2 枚)、葉 10 cm。カードは枚数より面積に使う ----
	LeavesPerBranch = 6;
	LeavesOnParentLevel = 4;
	LeafSize = 0.10f;
	LeafSizeVariance = 0.25f;
	LeafTipBias = 1.0f;
	LeafTerminalCount = 1;

	// ---- 房カード: 小枝 1 本に 5 枚 (t >= 0.12 に散る) + 枝先 1 枚 (6 割の小枝)、30 cm x 1.25 ----
	ClustersPerTwig = 5;
	TerminalClusters = 1;
	ClusterSize = 0.30f;
	ClusterAspect = 1.25f;

	// ---- 葉の陰影 / 色: 樹冠の内側と下側の暗さを弱める (黒い塊にしない)、色のばらつきは葉ごとより主枝ごと ----
	LeafAOInner = 0.65f;
	LeafAOBottom = 0.8f;
	LeafTintVariance = 0.15f;
	LeafBranchVariance = 0.3f;
	LeafHueVariance = 0.4f;

	// ---- Level 0: 幹 (根元半径 0.17 m、張り出し込みで 0.27 m、DownAngle = 傾き 4 deg。BaseOffset 0.34 = 5 / 12 セグメント (~2 m) で非対称に二又:
	//      主軸を継ぐ片は分岐角の 1/4 だけ逸れ、脇の片は 40 +- 15 deg 開いて細く、前半で分岐角の半分だけ立ち上がる。先端は Taper 0.16 で細る。
	//      分岐より上の片は CurveVariance 40 の揺れ + 屈曲でうねる。
	//      太さは凸形のテーパ (x^1.5) + パイプモデル (子枝が出るたびに段階的に細る) が決める) ----
	{
		FProceduralTreeLevelParams& l = Level[0];
		l.Branches = 0;
		l.Length = 4.7f;
		l.LengthVariance = 0.05f;
		l.LengthShape = 1.0f;
		l.Radius = 0.17f;
		l.Taper = 0.16f;
		l.DownAngle = 4.0f;
		l.DownAngleVariance = 0.0f;
		l.Rotate = 0.0f;
		l.RotateVariance = 0.0f;
		l.Curve = 0.0f;
		l.CurveBack = 0.0f;
		l.CurveVariance = 40.0f;
		l.BaseOffset = 0.34f;
		l.Segments = 12;
		l.RadialSegments = 16;
		l.Tropism = 5.0f;
		l.DownAngleShape = 0.0f;
		l.TipBias = 0.0f;
		l.bAlternate = false;
		l.RatioPower = 1.0f;
		l.TipRound = 0.8f;
		l.SegSplits = 0.10f;
		l.SplitAngle = 40.0f;
		l.SplitAngleVariance = 15.0f;
		l.BarkUVScale = 1.0f;
	}
	// ---- Level 1: 幹から出る主枝 7 本 (長さは CrownShape と包絡が決める、長さ +-40 %、付け根は LatticeHash で 3 段に群れ、分岐点の
	//      まわり 0.25 m は空ける)。半径は刈り込み前の長さの 0.5 乗で、付け根の親の 0.45 倍以上 / 0.8 倍以下 (少数の太い骨格の枝)、
	//      先端は Taper 0.15。下の主枝は水平に張り出し、上は立つ (DownAngle 70 + Shape 40 +- 25)、S 字 (25 / -25) + 屈曲 (+-13 deg) ----
	{
		FProceduralTreeLevelParams& l = Level[1];
		l.Branches = 7;
		l.Length = 0.7f;
		l.LengthVariance = 0.40f;
		l.LengthShape = 1.0f;
		l.Radius = 0.8f;
		l.Taper = 0.15f;
		l.DownAngle = 70.0f;
		l.DownAngleVariance = 25.0f;
		l.Rotate = 137.5f;
		l.RotateVariance = 45.0f;
		l.Curve = 25.0f;
		l.CurveBack = -25.0f;
		l.CurveVariance = 45.0f;
		l.BaseOffset = 0.10f;
		l.Segments = 8;
		l.RadialSegments = 9;
		l.Tropism = 10.0f;
		l.DownAngleShape = 40.0f;
		l.TipBias = 0.0f;
		l.bAlternate = false;
		l.RatioPower = 0.5f;
		l.TipRound = 0.8f;
		l.SegSplits = 0.10f;
		l.SplitAngle = 35.0f;
		l.SplitAngleVariance = 10.0f;
		l.BarkUVScale = 1.5f;
	}
	// ---- Level 2: 側枝 (主枝のまわりに螺旋で 12 本、主枝の外側 70 % (BaseOffset 0.3) に散る: 樹冠の内側は骨格の枝と空。
	//      下向き角 45 +- 25、屈曲 (+-9 deg) でうねる。6 セグメント) ----
	{
		FProceduralTreeLevelParams& l = Level[2];
		l.Branches = 12;
		l.Length = 0.5f;
		l.LengthVariance = 0.2f;
		l.LengthShape = 1.0f;
		l.Radius = 0.7f;
		l.Taper = 0.2f;
		l.DownAngle = 45.0f;
		l.DownAngleVariance = 25.0f;
		l.Rotate = 137.5f;
		l.RotateVariance = 30.0f;
		l.Curve = 10.0f;
		l.CurveBack = -15.0f;
		l.CurveVariance = 30.0f;
		l.BaseOffset = 0.30f;
		l.Segments = 6;
		l.RadialSegments = 6;
		l.Tropism = 4.0f;
		l.DownAngleShape = 10.0f;
		l.TipBias = 0.0f;
		l.bAlternate = false;
		l.RatioPower = 1.3f;
		l.TipRound = 0.8f;
		l.SegSplits = 0.0f;
		l.SplitAngle = 35.0f;
		l.SplitAngleVariance = 10.0f;
		l.BarkUVScale = 3.0f;
	}
	// ---- Level 3: 小枝 (葉が付く。側枝 1 本に 5 本、左右交互だが開き角 +-45 / 付け根の位置 +-0.4 枠 (LatticeHash) で揃わない櫛にしない。
	//      前半は少し持ち上がり (Curve 12) 先端は垂れる、揺れ 55。包絡の外へ出た先は葉の間引きの範囲で切る。4 セグメント) ----
	{
		FProceduralTreeLevelParams& l = Level[3];
		l.Branches = 5;
		l.Length = 0.55f;
		l.LengthVariance = 0.25f;
		l.LengthShape = 0.8f;
		l.Radius = 0.8f;
		l.Taper = 0.3f;
		l.DownAngle = 40.0f;
		l.DownAngleVariance = 20.0f;
		l.Rotate = 60.0f;
		l.RotateVariance = 45.0f;
		l.Curve = 12.0f;
		l.CurveBack = -25.0f;
		l.CurveVariance = 55.0f;
		l.BaseOffset = 0.10f;
		l.Segments = 4;
		l.RadialSegments = 5;
		l.Tropism = -15.0f;
		l.DownAngleShape = 10.0f;
		l.TipBias = 0.25f;
		l.bAlternate = true;
		l.RatioPower = 1.3f;
		l.TipRound = 0.8f;
		l.SegSplits = 0.0f;
		l.SplitAngle = 30.0f;
		l.SplitAngleVariance = 10.0f;
		l.BarkUVScale = 4.0f;
	}
}

FProceduralTreeParams FProceduralTreeParams::MakePreset(ETreePreset Preset)
{
	FProceduralTreeParams p;	// = Oak5m
	if (Preset != ETreePreset::Sapling)
	{
		return p;
	}

	// ---- Sapling = v1 の既定値 (Levels 3, 14 枝, 葉 0.22 m)。v2 の機能は全て中立にする ----
	p.Levels = 3;
	p.TropismScale = 1.0f;
	p.BarkUVTilesPerMeter = 1.0f;
	p.BaseSplits = 0;
	p.CrownShape = ECrownShape::Cylindrical;
	p.PruneRatio = 0.0f;
	p.TrunkFlare = 0.0f;
	p.GroundOffset = 0.0f;
	p.PipeExponent = 0.0f;
	p.JunctionSink = 0.0f;
	p.JunctionCollar = 0.0f;
	p.TwigRadiusThreshold = 0.0f;
	p.BarkAOInner = 1.0f;
	p.BarkMoss = 0.0f;

	p.bGenerateLeaves = true;
	p.LeavesPerBranch = 12;
	p.LeavesOnParentLevel = 0;
	p.LeafSize = 0.22f;
	p.LeafSizeVariance = 0.3f;
	p.LeafAspect = 0.6f;
	p.LeafAngle = 50.0f;
	p.LeafAngleVariance = 20.0f;
	p.LeafTipBias = 0.0f;
	p.LeafTerminalCount = 0;
	p.LeafBareTwigFraction = 0.0f;
	p.LeafPetioleLength = 0.0f;
	p.LeafDroop = 0.0f;
	p.LeafCup = 0.0f;

	p.ClustersPerTwig = 0;
	p.TerminalClusters = 0;

	p.LeafNormalBlend = 0.0f;
	p.LeafNormalUpBias = 0.0f;
	p.LeafAOInner = 1.0f;
	p.LeafAOBottom = 1.0f;
	p.LeafHueVariance = 0.0f;
	p.LeafBranchVariance = 0.0f;
	p.LeafDeadFraction = 0.0f;
	p.LeafTintVariance = 0.25f;
	p.LeafErosion = 0.0f;
	p.LeafTint = { 1.0f, 1.0f, 1.0f };

	// v1 の屈光性 (全長で接線に 0.2 を足す ≒ 11.5 deg) を各階層の Tropism [deg] へ
	const float legacyTropismDeg = 0.2f * 57.2957795f;

	struct FV1Level { int Branches; float Length, LengthVariance, LengthShape, Radius, Taper, DownAngle, DownAngleVariance, Rotate, RotateVariance, Curve, CurveVariance, BaseOffset; int Segments, RadialSegments; };
	const FV1Level v1[MaxLevels] =
	{
		{ 0,  4.0f,  0.0f,  1.0f,  0.16f, 0.55f,  0.0f,  0.0f,   0.0f,  0.0f,  0.0f, 12.0f, 0.35f, 10, 14 },
		{ 14, 0.55f, 0.15f, 0.45f, 0.6f,  0.3f,  60.0f, 12.0f, 137.5f, 25.0f, 20.0f, 35.0f, 0.15f, 7,  8 },
		{ 7,  0.5f,  0.2f,  0.7f,  0.55f, 0.25f, 50.0f, 15.0f, 137.5f, 30.0f, 10.0f, 50.0f, 0.1f,  4,  6 },
		{ 4,  0.5f,  0.2f,  0.8f,  0.55f, 0.2f,  45.0f, 20.0f, 137.5f, 40.0f,  0.0f, 60.0f, 0.1f,  3,  5 },
	};
	for (int i = 0; i < MaxLevels; ++i)
	{
		FProceduralTreeLevelParams& l = p.Level[i];
		const FV1Level& s = v1[i];
		l.Branches = s.Branches;
		l.Length = s.Length;
		l.LengthVariance = s.LengthVariance;
		l.LengthShape = s.LengthShape;
		l.Radius = s.Radius;
		l.Taper = s.Taper;
		l.DownAngle = s.DownAngle;
		l.DownAngleVariance = s.DownAngleVariance;
		l.Rotate = s.Rotate;
		l.RotateVariance = s.RotateVariance;
		l.Curve = s.Curve;
		l.CurveVariance = s.CurveVariance;
		l.BaseOffset = s.BaseOffset;
		l.Segments = s.Segments;
		l.RadialSegments = s.RadialSegments;

		l.Tropism = legacyTropismDeg;
		l.CurveBack = 0.0f;
		l.DownAngleShape = 0.0f;
		l.TipBias = 0.0f;
		l.bAlternate = false;
		l.RatioPower = 1.0f;
		l.TipRound = 0.0f;
		l.SegSplits = 0.0f;
		l.SplitAngle = 30.0f;
		l.SplitAngleVariance = 8.0f;
		l.BarkUVScale = 1.0f;
	}
	return p;
}

namespace
{
	// 階層ごとの片 (枝 + 分岐のクローン) の数と、分岐前の枝 (論理的な枝) の数。分岐は乱数を使わない (FindSplitSegment) ので正確に数えられる:
	// pieces_i = stems_i x (1 本の枝の片の数)、stems_{i+1} = stems_i x Branches_{i+1} (子の数は分岐した片が分け合い、片ごとに増えない)。
	// Branches は Sanitize 前でも 0..64 に、Segments は 1..32 に丸めて数える (生成も同じ値を使う)
	void CountPiecesPerLevel(const FProceduralTreeParams& P, double OutPieces[FProceduralTreeParams::MaxLevels], double OutStems[FProceduralTreeParams::MaxLevels])
	{
		const int levels = ClampInt(P.Levels, 1, FProceduralTreeParams::MaxLevels);
		double stems = 1.0;
		for (int i = 0; i < levels; ++i)
		{
			const int segments = ClampInt(P.Level[i].Segments, 1, 32);
			const int baseSplits = (i == 0) ? ClampInt(P.BaseSplits, 0, 3) : 0;
			OutStems[i] = stems;
			OutPieces[i] = stems * static_cast<double>(CountPieces(P.Level[i], baseSplits, i == 0, segments, segments, 0.0f));
			if (i + 1 < levels)
			{
				stems *= static_cast<double>(ClampInt(P.Level[i + 1].Branches, 0, 64));
			}
		}
	}
}

namespace
{
	// 1 本の枝の全ての片が持つセグメント数の合計 (CountPieces と同じ再帰。クローンは残りのセグメントを継ぐので分岐するほど増える)
	unsigned long long CountPieceSegments(const FProceduralTreeLevelParams& lp, int BaseSplits, bool bTrunkBase, int Segments, int SegmentsTotal, float Error)
	{
		int splits = 0;
		const int s = FindSplitSegment(lp, BaseSplits, bTrunkBase, Segments, SegmentsTotal, Error, splits);
		if (s < 0) return static_cast<unsigned long long>(Segments);
		return static_cast<unsigned long long>(s + 1) + static_cast<unsigned long long>(splits + 1) * CountPieceSegments(lp, BaseSplits, false, Segments - s - 1, SegmentsTotal, Error);
	}

	// polish3: 三角形数の上界 (Sanitize 用)。片 1 本 = 側面 2 R x (持つセグメント + 1 (襟のリング)) + 半球 2 リング 4 R + 先端 / 根元の扇 2 R
	// = 2 R x セグメント + 8 R、幹の最初の片は張り出しのリング 3 本分 6 R を足す。カードは 2 枚。細い小枝の周 4 分割は数えない (上界のまま)
	double EstimateTrianglesBound(const FProceduralTreeParams& P)
	{
		const int levels = ClampInt(P.Levels, 1, FProceduralTreeParams::MaxLevels);
		double pieces[FProceduralTreeParams::MaxLevels] = {}, stems[FProceduralTreeParams::MaxLevels] = {};
		CountPiecesPerLevel(P, pieces, stems);
		double tris = 0.0;
		for (int i = 0; i < levels; ++i)
		{
			const FProceduralTreeLevelParams& lp = P.Level[i];
			const int segments = ClampInt(lp.Segments, 1, 32);
			const double radial = static_cast<double>(ClampInt(lp.RadialSegments, 3, 32));
			const double segs = stems[i] * static_cast<double>(CountPieceSegments(lp, (i == 0) ? ClampInt(P.BaseSplits, 0, 3) : 0, i == 0, segments, segments, 0.0f));
			tris += 2.0 * radial * segs + 8.0 * radial * pieces[i] + ((i == 0) ? 6.0 * radial : 0.0);
		}
		return tris + 2.0 * static_cast<double>(P.EstimateCards());
	}
}

unsigned long long FProceduralTreeParams::EstimateStems() const
{
	// 総数 = Σ pieces_i (分岐の片を正確に数えるので、生成される NumStems と一致する)
	const int levels = ClampInt(Levels, 1, MaxLevels);
	double pieces[MaxLevels] = {}, stems[MaxLevels] = {};
	CountPiecesPerLevel(*this, pieces, stems);
	double total = 0.0;
	for (int i = 0; i < levels; ++i)
	{
		total += pieces[i];
	}
	return static_cast<unsigned long long>(fmin(ceil(total), 1.0e15));
}

unsigned long long FProceduralTreeParams::EstimateCards() const
{
	if (!bGenerateLeaves) return 0;
	const int levels = ClampInt(Levels, 1, MaxLevels);

	// 葉 / 房の枚数は分岐した片が分け合う (枝 1 本で枚数ぶん、ただし片ごとに最低 1 枚) ので、上界 = 枝の数 x 枚数 + クローンの片の数 (最低 1 枚ぶん)
	double pieces[MaxLevels] = {}, stems[MaxLevels] = {};
	CountPiecesPerLevel(*this, pieces, stems);
	auto shared = [](double Stems, double Pieces, int Count)
		{
			return (Count > 0) ? Stems * static_cast<double>(Count) + (Pieces - Stems) : 0.0;
		};

	// 房は ClustersPerTwig > 0 のときだけ: 枝の途中の房は葉と同じ取り分、枝先の房は切られていない片ごとに TerminalClusters 枚 (上界 = 片の数 x 枚数)
	double cards = shared(stems[levels - 1], pieces[levels - 1], LeavesPerBranch);
	if (ClustersPerTwig > 0)
	{
		cards += shared(stems[levels - 1], pieces[levels - 1], ClustersPerTwig)
			+ pieces[levels - 1] * static_cast<double>((TerminalClusters > 0) ? TerminalClusters : 0);
	}
	if (levels >= 2)
	{
		cards += shared(stems[levels - 2], pieces[levels - 2], LeavesOnParentLevel);
		// 側枝の胴の房 (ClustersOnParentLevel 枚、ClustersPerTwig > 0 のとき)
		if (ClustersPerTwig > 0)
		{
			cards += shared(stems[levels - 2], pieces[levels - 2], ClustersOnParentLevel);
		}
	}
	// 上位階層 (最終階層の 2 つ上まで) の枝先の葉 / 房: 片ごとに LeafTerminalCount 枚 + TerminalClusters 枚 (上界 = 切られた片も数える)
	for (int i = 0; i + 2 < levels; ++i)
	{
		const int tipLeaves = ClampInt(LeafTerminalCount, 0, 4);
		const int tipClusters = (ClustersPerTwig > 0 && TerminalClusters > 0) ? TerminalClusters : 0;
		cards += pieces[i] * static_cast<double>(tipLeaves + tipClusters);
	}
	return static_cast<unsigned long long>(ceil(fmax(cards, 0.0)));
}

void FProceduralTreeParams::Sanitize()
{
	Levels = ClampInt(Levels, 1, MaxLevels);
	TropismScale = Clamp(TropismScale, -5.0f, 5.0f);
	BarkUVTilesPerMeter = Clamp(BarkUVTilesPerMeter, 0.01f, 100.0f);
	BaseSplits = ClampInt(BaseSplits, 0, 3);

	CrownShape = static_cast<ECrownShape>(ClampInt(static_cast<int>(CrownShape), 0, static_cast<int>(ECrownShape::MAX) - 1));
	PruneRatio = Clamp(PruneRatio, 0.0f, 1.0f);
	PruneWidth = Clamp(PruneWidth, 0.1f, 1.5f);
	PruneWidthPeak = Clamp(PruneWidthPeak, 0.05f, 0.95f);
	PrunePowerLow = Clamp(PrunePowerLow, 0.1f, 3.0f);
	PrunePowerHigh = Clamp(PrunePowerHigh, 0.1f, 3.0f);

	TrunkFlare = Clamp(TrunkFlare, 0.0f, 2.0f);
	TrunkFlareHeight = Clamp(TrunkFlareHeight, 0.02f, 0.6f);
	GroundOffset = Clamp(GroundOffset, 0.0f, 0.5f);
	PipeExponent = Clamp(PipeExponent, 0.0f, 3.0f);
	if (PipeExponent > 0.0f && PipeExponent < 1.0f) PipeExponent = 1.0f;	// 0 = 無効、それ以外は 1 以上
	JunctionSink = Clamp(JunctionSink, 0.0f, 0.9f);
	JunctionCollar = Clamp(JunctionCollar, 0.0f, 1.0f);
	TwigRadiusThreshold = Clamp(TwigRadiusThreshold, 0.0f, 0.1f);
	BarkAOInner = Clamp(BarkAOInner, 0.0f, 1.0f);
	BarkMoss = Clamp(BarkMoss, 0.0f, 1.0f);

	LeavesPerBranch = ClampInt(LeavesPerBranch, 0, 64);
	LeavesOnParentLevel = ClampInt(LeavesOnParentLevel, 0, 32);
	LeafSize = Clamp(LeafSize, 0.01f, 5.0f);
	LeafSizeVariance = Clamp(LeafSizeVariance, 0.0f, 0.95f);
	LeafAspect = Clamp(LeafAspect, 0.1f, 3.0f);
	LeafAngle = Clamp(LeafAngle, 0.0f, 180.0f);
	LeafAngleVariance = Clamp(LeafAngleVariance, 0.0f, 90.0f);
	LeafTipBias = Clamp(LeafTipBias, 0.0f, 4.0f);
	LeafTerminalCount = ClampInt(LeafTerminalCount, 0, (LeavesPerBranch < 4) ? LeavesPerBranch : 4);
	LeafBareTwigFraction = Clamp(LeafBareTwigFraction, 0.0f, 0.5f);
	LeafPetioleLength = Clamp(LeafPetioleLength, 0.0f, 0.1f);
	LeafDroop = Clamp(LeafDroop, 0.0f, 55.0f);
	LeafCup = Clamp(LeafCup, 0.0f, 40.0f);

	ClustersPerTwig = ClampInt(ClustersPerTwig, 0, 8);
	TerminalClusters = ClampInt(TerminalClusters, 0, 3);
	ClusterSize = Clamp(ClusterSize, 0.05f, 2.0f);
	ClusterAspect = Clamp(ClusterAspect, 0.3f, 3.0f);

	LeafNormalBlend = Clamp(LeafNormalBlend, 0.0f, 0.85f);
	LeafNormalUpBias = Clamp(LeafNormalUpBias, 0.0f, 1.0f);
	LeafAOInner = Clamp(LeafAOInner, 0.1f, 1.0f);
	LeafAOBottom = Clamp(LeafAOBottom, 0.1f, 1.0f);
	LeafAOFalloff = Clamp(LeafAOFalloff, 0.0f, 3.0f);
	LeafHueVariance = Clamp(LeafHueVariance, 0.0f, 1.0f);
	LeafBranchVariance = Clamp(LeafBranchVariance, 0.0f, 1.0f);
	LeafDeadFraction = Clamp(LeafDeadFraction, 0.0f, 0.3f);
	LeafTintVariance = Clamp(LeafTintVariance, 0.0f, 1.0f);
	LeafErosion = Clamp(LeafErosion, 0.0f, 0.3f);
	LeafTint.x = Clamp(LeafTint.x, 0.0f, 2.0f);
	LeafTint.y = Clamp(LeafTint.y, 0.0f, 2.0f);
	LeafTint.z = Clamp(LeafTint.z, 0.0f, 2.0f);

	for (int i = 0; i < MaxLevels; ++i)
	{
		FProceduralTreeLevelParams& l = Level[i];
		l.Branches = ClampInt(l.Branches, 0, 64);
		l.Length = Clamp(l.Length, 0.01f, (i == 0) ? 200.0f : 3.0f);
		l.LengthVariance = Clamp(l.LengthVariance, 0.0f, 0.95f);
		l.LengthShape = Clamp(l.LengthShape, 0.05f, 2.0f);
		l.Radius = Clamp(l.Radius, (i == 0) ? MinStemRadius : 0.05f, (i == 0) ? 20.0f : 1.0f);
		l.Taper = Clamp(l.Taper, 0.0f, 1.0f);
		l.DownAngle = Clamp(l.DownAngle, 0.0f, 180.0f);
		l.DownAngleVariance = Clamp(l.DownAngleVariance, 0.0f, 90.0f);
		l.Rotate = Clamp(l.Rotate, -360.0f, 360.0f);
		l.RotateVariance = Clamp(l.RotateVariance, 0.0f, 180.0f);
		l.Curve = Clamp(l.Curve, -360.0f, 360.0f);
		l.CurveVariance = Clamp(l.CurveVariance, 0.0f, 360.0f);
		l.BaseOffset = Clamp(l.BaseOffset, 0.0f, 0.95f);
		l.Segments = ClampInt(l.Segments, 1, 32);
		l.RadialSegments = ClampInt(l.RadialSegments, 3, 32);

		l.Tropism = Clamp(l.Tropism, -90.0f, 90.0f);
		l.CurveBack = Clamp(l.CurveBack, -360.0f, 360.0f);
		l.DownAngleShape = Clamp(l.DownAngleShape, -60.0f, 60.0f);
		l.TipBias = Clamp(l.TipBias, 0.0f, 3.0f);
		l.RatioPower = Clamp(l.RatioPower, 0.5f, 1.5f);
		l.TipRound = Clamp(l.TipRound, 0.0f, 1.0f);
		l.SegSplits = Clamp(l.SegSplits, 0.0f, 1.0f);
		l.SplitAngle = Clamp(l.SplitAngle, 8.0f, 90.0f);
		l.SplitAngleVariance = Clamp(l.SplitAngleVariance, 0.0f, 45.0f);
		l.BarkUVScale = Clamp(l.BarkUVScale, 0.1f, 10.0f);
	}

	// ---- 枝の総数 (分岐の期待倍率込み) を上限内に収める ----
	// まず最深階層の SegSplits を 0 にし、それでも超えるなら最深階層から Branches を減らす
	while (EstimateStems() > FProceduralTreeBuilder::MaxTotalStems)
	{
		int splitLevel = -1;
		for (int i = Levels - 1; i >= 0; --i)
		{
			if (Level[i].SegSplits > 0.0f) { splitLevel = i; break; }
		}
		if (splitLevel >= 0)
		{
			Level[splitLevel].SegSplits = 0.0f;
			continue;
		}
		if (BaseSplits > 0)
		{
			--BaseSplits;
			continue;
		}
		int deepest = -1;
		for (int i = Levels - 1; i >= 1; --i)
		{
			if (Level[i].Branches > 1) { deepest = i; break; }
		}
		if (deepest < 0) break;
		--Level[deepest].Branches;
	}

	// ---- 葉カード数 (単葉 + 房) を上限内に収める ----
	while (EstimateCards() > FProceduralTreeBuilder::MaxLeafCards)
	{
		if (LeavesPerBranch > 2) { --LeavesPerBranch; continue; }
		if (ClustersPerTwig > 0) { --ClustersPerTwig; continue; }
		if (LeavesOnParentLevel > 0) { --LeavesOnParentLevel; continue; }
		int deepest = -1;
		for (int i = Levels - 1; i >= 1; --i)
		{
			if (Level[i].Branches > 1) { deepest = i; break; }
		}
		if (deepest < 0) break;
		--Level[deepest].Branches;
	}
	// ---- polish3: 三角形数 (上界) を MaxTreeTriangles 以内に収める。最深階層から周方向の分割 (4 まで) -> 長さ方向の分割 (3 まで) の順に減らし、
	//      それでも超えるなら幹の分割 (周 8 / 長さ 6 まで)、最後に最深階層の Branches を減らす (枝の数を変える前に細部を落とす) ----
	while (EstimateTrianglesBound(*this) > MaxTreeTriangles)
	{
		bool reduced = false;
		for (int i = Levels - 1; i >= 1 && !reduced; --i)
		{
			if (Level[i].RadialSegments > 4) { --Level[i].RadialSegments; reduced = true; }
		}
		for (int i = Levels - 1; i >= 1 && !reduced; --i)
		{
			if (Level[i].Segments > 3) { --Level[i].Segments; reduced = true; }
		}
		if (!reduced && Level[0].RadialSegments > 8) { --Level[0].RadialSegments; reduced = true; }
		if (!reduced && Level[0].Segments > 6) { --Level[0].Segments; reduced = true; }
		for (int i = Levels - 1; i >= 1 && !reduced; --i)
		{
			if (Level[i].Branches > 1) { --Level[i].Branches; reduced = true; }
		}
		if (!reduced) break;
	}
	LeafTerminalCount = ClampInt(LeafTerminalCount, 0, (LeavesPerBranch < 4) ? LeavesPerBranch : 4);
}

unsigned long long FProceduralTreeParams::ComputeHash() const
{
	// FNV-1a 64bit。使っている階層 (Levels 分) だけを混ぜる
	unsigned long long hash = 14695981039346656037ULL;
	auto Mix = [&hash](const void* Data, size_t Size)
		{
			const unsigned char* bytes = static_cast<const unsigned char*>(Data);
			for (size_t i = 0; i < Size; ++i)
			{
				hash ^= bytes[i];
				hash *= 1099511628211ULL;
			}
		};
	auto MixF = [&Mix](float V) { Mix(&V, sizeof(V)); };
	auto MixI = [&Mix](int V) { Mix(&V, sizeof(V)); };

	MixI(static_cast<int>(GeneratorVersion));
	MixI(GeneratorRevision);	// polish3: ヘッダの GeneratorVersion は固定なので、生成規則の改訂はここで混ぜる (古い SDF キャッシュと衝突しない)
	MixI(static_cast<int>(Seed));
	MixI(Levels);
	MixF(TropismScale);
	MixF(BarkUVTilesPerMeter);
	MixI(BaseSplits);

	MixI(static_cast<int>(CrownShape));
	MixF(PruneRatio);
	MixF(PruneWidth);
	MixF(PruneWidthPeak);
	MixF(PrunePowerLow);
	MixF(PrunePowerHigh);

	MixF(TrunkFlare);
	MixF(TrunkFlareHeight);
	MixF(GroundOffset);
	MixF(PipeExponent);
	MixF(JunctionSink);
	MixF(JunctionCollar);
	MixF(TwigRadiusThreshold);
	MixF(BarkAOInner);
	MixF(BarkMoss);

	MixI(bGenerateLeaves ? 1 : 0);
	MixI(LeavesPerBranch);
	MixI(LeavesOnParentLevel);
	MixF(LeafSize);
	MixF(LeafSizeVariance);
	MixF(LeafAspect);
	MixF(LeafAngle);
	MixF(LeafAngleVariance);
	MixF(LeafTipBias);
	MixI(LeafTerminalCount);
	MixF(LeafBareTwigFraction);
	MixF(LeafPetioleLength);
	MixF(LeafDroop);
	MixF(LeafCup);

	MixI(ClustersPerTwig);
	MixI(TerminalClusters);
	MixF(ClusterSize);
	MixF(ClusterAspect);

	MixF(LeafNormalBlend);
	MixF(LeafNormalUpBias);
	MixF(LeafAOInner);
	MixF(LeafAOBottom);
	MixF(LeafAOFalloff);
	MixF(LeafHueVariance);
	MixF(LeafBranchVariance);
	MixF(LeafDeadFraction);
	MixF(LeafTintVariance);
	MixF(LeafErosion);
	MixF(LeafTint.x);
	MixF(LeafTint.y);
	MixF(LeafTint.z);

	const int levels = ClampInt(Levels, 1, MaxLevels);
	for (int i = 0; i < levels; ++i)
	{
		const FProceduralTreeLevelParams& l = Level[i];
		MixI(l.Branches);
		MixF(l.Length);
		MixF(l.LengthVariance);
		MixF(l.LengthShape);
		MixF(l.Radius);
		MixF(l.Taper);
		MixF(l.DownAngle);
		MixF(l.DownAngleVariance);
		MixF(l.Rotate);
		MixF(l.RotateVariance);
		MixF(l.Curve);
		MixF(l.CurveVariance);
		MixF(l.BaseOffset);
		MixI(l.Segments);
		MixI(l.RadialSegments);

		MixF(l.Tropism);
		MixF(l.CurveBack);
		MixF(l.DownAngleShape);
		MixF(l.TipBias);
		MixI(l.bAlternate ? 1 : 0);
		MixF(l.RatioPower);
		MixF(l.TipRound);
		MixF(l.SegSplits);
		MixF(l.SplitAngle);
		MixF(l.SplitAngleVariance);
		MixF(l.BarkUVScale);
	}
	return hash;
}


// ============================================================
//  FProceduralTreeBuilder
// ============================================================

FProceduralMeshData FProceduralTreeBuilder::Build(const FProceduralTreeParams& Params, FProceduralTreeStats* OutStats)
{
	const auto t0 = std::chrono::steady_clock::now();

	FProceduralTreeParams sanitized = Params;
	sanitized.Sanitize();

	FTreeGenerator generator(sanitized);
	FProceduralMeshData data = generator.Run(OutStats);

	if (OutStats)
	{
		OutStats->GenerateMs = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - t0).count();
	}
	return data;
}
