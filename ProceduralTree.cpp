#include "Main.h"
#include "ProceduralTree.h"

#include <random>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

// ============================================================
//  内部ヘルパ
// ============================================================
namespace
{
	constexpr float PT_PI = 3.14159265358979323846f;
	constexpr float PT_TWO_PI = 6.28318530717958647692f;
	constexpr float PT_DEG2RAD = PT_PI / 180.0f;

	// 枝の最小半径 [m] (これ以下には細らせない)
	constexpr float MinStemRadius = 0.003f;

	// 枝の総数の上限 (Sanitize が Branches を減らして収める)
	constexpr unsigned int MaxTotalStems = 20000;

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
	};

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

	// 子枝の生成要求 (親の骨格から先に決めてから再帰する)
	struct FChildSpec
	{
		XMFLOAT3 Origin;
		XMFLOAT3 Direction;
		XMFLOAT3 Normal;
		float    Length;
		float    Radius;
	};

	// ============================================================
	//  FTreeGenerator
	// ============================================================
	class FTreeGenerator
	{
	private:
		const FProceduralTreeParams& m_Params;
		FTreeRandom                  m_Random;

		FProceduralMeshSection m_Bark;
		FProceduralMeshSection m_Leaves;
		unsigned int           m_NumStems = 0;
		unsigned int           m_NumLeaves = 0;

		// 頂点法線の向きに合わせて巻き順を正して三角形を追加する
		// (縮退三角形は追加しない)
		static void AddTriangle(FProceduralMeshSection& Section,
			unsigned int I0, unsigned int I1, unsigned int I2)
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

			const XMVECTOR reference = XMVectorAdd(XMVectorAdd(Load3(v0.Normal), Load3(v1.Normal)), Load3(v2.Normal));
			const bool bFlip = Dot3(geometric, reference) < 0.0f;

			Section.Indices.push_back(I0);
			Section.Indices.push_back(bFlip ? I2 : I1);
			Section.Indices.push_back(bFlip ? I1 : I2);
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

		// ---- 骨格 -> 樹皮チューブ ----
		void BuildBark(const FStem& Stem)
		{
			const FProceduralTreeLevelParams& lp = m_Params.Level[Stem.Level];
			const int radial = (lp.RadialSegments < 3) ? 3 : lp.RadialSegments;
			const int ringVerts = radial + 1;	// 継ぎ目の頂点を複製 (UV の連続性)
			const int pointCount = static_cast<int>(Stem.Points.size());

			// U は根元の円周で整数タイルになるようにする (継ぎ目でテクスチャが繋がる)
			const float tilesAround = fmaxf(1.0f,
				floorf(PT_TWO_PI * Stem.BaseRadius * m_Params.BarkUVTilesPerMeter + 0.5f));

			// 先細りの法線補正 (-dr/ds: 半径が減る向きで接線側へ倒す)
			const float slope = (Stem.BaseRadius - Stem.TipRadius) / fmaxf(Stem.Length, 1.0e-4f);

			const unsigned int base = static_cast<unsigned int>(m_Bark.Vertices.size());

			for (int i = 0; i < pointCount; ++i)
			{
				const FStemPoint& pt = Stem.Points[i];
				const XMVECTOR P = Load3(pt.Position);
				const XMVECTOR T = Load3(pt.Tangent);
				const XMVECTOR N = Load3(pt.Normal);
				const XMVECTOR B = SafeNormalize(XMVector3Cross(T, N), AnyPerpendicular(T));

				// 幹の根元は頂点カラーでわずかに暗くする (接地の陰)
				float shade = 1.0f;
				if (Stem.Level == 0 && pointCount > 1)
				{
					shade = Lerp(0.72f, 1.0f, static_cast<float>(i) / static_cast<float>(pointCount - 1));
				}
				const XMFLOAT4 color{ shade, shade, shade, 1.0f };

				for (int j = 0; j < ringVerts; ++j)
				{
					const float theta = static_cast<float>(j) / static_cast<float>(radial) * PT_TWO_PI;
					const float c = cosf(theta);
					const float s = sinf(theta);

					const XMVECTOR radialDir = XMVectorAdd(XMVectorScale(N, c), XMVectorScale(B, s));
					const XMVECTOR position = XMVectorAdd(P, XMVectorScale(radialDir, pt.Radius));
					const XMVECTOR normal = SafeNormalize(XMVectorAdd(radialDir, XMVectorScale(T, slope)), radialDir);
					// +U 方向 = 周方向 (cross(radial, dRadial/dTheta) = +T で dP/dV = cross(N, dP/dU) を満たす)
					const XMVECTOR tangent = XMVectorAdd(XMVectorScale(N, -s), XMVectorScale(B, c));

					const XMFLOAT2 uv{
						static_cast<float>(j) / static_cast<float>(radial) * tilesAround,
						pt.Distance * m_Params.BarkUVTilesPerMeter };

					m_Bark.Vertices.push_back(MakeVertex(position, normal, tangent, uv, color));
				}
			}

			// ---- 側面 ----
			for (int i = 0; i + 1 < pointCount; ++i)
			{
				for (int j = 0; j < radial; ++j)
				{
					const unsigned int a = base + static_cast<unsigned int>(i * ringVerts + j);
					const unsigned int b = a + 1;
					const unsigned int c = a + static_cast<unsigned int>(ringVerts);
					const unsigned int d = c + 1;
					AddTriangle(m_Bark, a, b, c);
					AddTriangle(m_Bark, b, d, c);
				}
			}

			// ---- キャップ (先端 / 根元)。各枝を閉じた面にする (SDF の内外判定のため) ----
			{
				const FStemPoint& tip = Stem.Points[pointCount - 1];
				const XMVECTOR T = Load3(tip.Tangent);
				const unsigned int tipIndex = static_cast<unsigned int>(m_Bark.Vertices.size());
				m_Bark.Vertices.push_back(MakeVertex(Load3(tip.Position), T, Load3(tip.Normal),
					{ tilesAround * 0.5f, tip.Distance * m_Params.BarkUVTilesPerMeter }, { 1.0f, 1.0f, 1.0f, 1.0f }));

				const unsigned int ring = base + static_cast<unsigned int>((pointCount - 1) * ringVerts);
				for (int j = 0; j < radial; ++j)
				{
					AddTriangle(m_Bark, ring + j, ring + j + 1, tipIndex);
				}
			}
			{
				const FStemPoint& root = Stem.Points[0];
				const XMVECTOR T = Load3(root.Tangent);
				const float shade = (Stem.Level == 0) ? 0.72f : 1.0f;
				const unsigned int rootIndex = static_cast<unsigned int>(m_Bark.Vertices.size());
				m_Bark.Vertices.push_back(MakeVertex(Load3(root.Position), XMVectorNegate(T), Load3(root.Normal),
					{ tilesAround * 0.5f, 0.0f }, { shade, shade, shade, 1.0f }));

				for (int j = 0; j < radial; ++j)
				{
					AddTriangle(m_Bark, base + j, base + j + 1, rootIndex);
				}
			}
		}

		// ---- 葉カード (最終階層の枝に貼る) ----
		void BuildLeaves(const FStem& Stem)
		{
			const int count = m_Params.LeavesPerBranch;
			if (count <= 0) return;

			const XMVECTOR up = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
			float phi = m_Random.Range(0.0f, PT_TWO_PI);

			for (int j = 0; j < count; ++j)
			{
				const float t = Clamp(0.15f + 0.85f * (static_cast<float>(j) + 0.5f + m_Random.Signed(0.3f)) / static_cast<float>(count), 0.1f, 1.0f);
				phi += (137.5f + m_Random.Signed(40.0f)) * PT_DEG2RAD;

				const FStemSample s = SampleStem(Stem, t);
				const XMVECTOR radialDir = RotateAxis(s.Normal, s.Tangent, phi);

				const float angle = (m_Params.LeafAngle + m_Random.Signed(m_Params.LeafAngleVariance)) * PT_DEG2RAD;
				const XMVECTOR leafDir = SafeNormalize(
					XMVectorAdd(XMVectorScale(s.Tangent, cosf(angle)), XMVectorScale(radialDir, sinf(angle))), s.Tangent);
				const XMVECTOR origin = XMVectorAdd(s.Position, XMVectorScale(radialDir, s.Radius * 0.8f));

				const float size = fmaxf(0.01f, m_Params.LeafSize * (1.0f + m_Random.Signed(m_Params.LeafSizeVariance)));
				const float halfWidth = size * m_Params.LeafAspect * 0.5f;

				// 葉面はおおむね上を向かせ、葉軸まわりにランダムに捻る
				XMVECTOR side = XMVector3Cross(up, leafDir);
				side = (LengthSq3(side) < 1.0e-4f) ? AnyPerpendicular(leafDir) : XMVector3Normalize(side);
				XMVECTOR normal = XMVector3Normalize(XMVector3Cross(leafDir, side));
				if (XMVectorGetY(normal) < 0.0f)
				{
					side = XMVectorNegate(side);
					normal = XMVectorNegate(normal);
				}
				const float roll = m_Random.Signed(35.0f) * PT_DEG2RAD;
				side = RotateAxis(side, leafDir, roll);
				normal = RotateAxis(normal, leafDir, roll);

				// 葉ごとの明度ばらつき (暗い葉ほどわずかに暖色へ)
				const float k = m_Params.LeafTintVariance * m_Random.Unit();
				const XMFLOAT4 color{ 1.0f - 0.5f * k, 1.0f - k, 1.0f - 0.9f * k, 1.0f };

				const XMVECTOR tipPos = XMVectorAdd(origin, XMVectorScale(leafDir, size));
				const XMVECTOR sideOffset = XMVectorScale(side, halfWidth);

				// V = 1 が付け根、V = 0 が葉先 (dP/dV = -leafDir = cross(N, side))
				const unsigned int b = static_cast<unsigned int>(m_Leaves.Vertices.size());
				m_Leaves.Vertices.push_back(MakeVertex(XMVectorSubtract(origin, sideOffset), normal, side, { 0.0f, 1.0f }, color));
				m_Leaves.Vertices.push_back(MakeVertex(XMVectorAdd(origin, sideOffset), normal, side, { 1.0f, 1.0f }, color));
				m_Leaves.Vertices.push_back(MakeVertex(XMVectorAdd(tipPos, sideOffset), normal, side, { 1.0f, 0.0f }, color));
				m_Leaves.Vertices.push_back(MakeVertex(XMVectorSubtract(tipPos, sideOffset), normal, side, { 0.0f, 0.0f }, color));

				AddTriangle(m_Leaves, b + 0, b + 1, b + 2);
				AddTriangle(m_Leaves, b + 0, b + 2, b + 3);

				++m_NumLeaves;
			}
		}

		// ---- 枝 1 本の骨格を作り、樹皮 / 葉 / 子枝を生成する ----
		void GenerateStem(int Level, FXMVECTOR Origin, FXMVECTOR Direction, FXMVECTOR InNormal,
			float Length, float BaseRadius)
		{
			const FProceduralTreeLevelParams& lp = m_Params.Level[Level];
			const int segments = (lp.Segments < 1) ? 1 : lp.Segments;
			const float segmentLength = Length / static_cast<float>(segments);
			const XMVECTOR up = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);

			FStem stem;
			stem.Level = Level;
			stem.Length = Length;
			stem.BaseRadius = fmaxf(BaseRadius, MinStemRadius);
			stem.TipRadius = stem.BaseRadius * Clamp(lp.Taper, 0.0f, 1.0f);

			// ---- セグメント列 (曲がり + 屈光性 + 回転最小フレーム) ----
			std::vector<XMVECTOR> positions(segments + 1);
			std::vector<XMVECTOR> normals(segments + 1);
			std::vector<XMVECTOR> segmentDirs(segments);

			XMVECTOR position = Origin;
			XMVECTOR tangent = SafeNormalize(Direction, up);
			XMVECTOR normal = Orthonormalize(InNormal, tangent);
			positions[0] = position;
			normals[0] = normal;

			const float curvePerSegment = lp.Curve / static_cast<float>(segments) * PT_DEG2RAD;

			for (int s = 0; s < segments; ++s)
			{
				XMVECTOR t = tangent;

				// 系統的な曲がり: 鉛直面内で上 (+) / 下 (-) へ
				if (fabsf(curvePerSegment) > 1.0e-6f)
				{
					XMVECTOR axis = XMVector3Cross(t, up);
					axis = (LengthSq3(axis) < 1.0e-6f) ? normal : XMVector3Normalize(axis);

					XMVECTOR candidate = RotateAxis(t, axis, curvePerSegment);
					// 正の Curve で上向きに曲がる方の回転を選ぶ
					if ((Dot3(candidate, up) - Dot3(t, up)) * curvePerSegment < 0.0f)
					{
						candidate = RotateAxis(t, axis, -curvePerSegment);
					}
					t = candidate;
				}

				// ランダムな曲がり: 接線に直交するランダム軸まわり
				{
					const float wobble = m_Random.Signed(lp.CurveVariance) / static_cast<float>(segments) * PT_DEG2RAD;
					const float azimuth = m_Random.Range(0.0f, PT_TWO_PI);
					const XMVECTOR randomAxis = Orthonormalize(RotateAxis(normal, tangent, azimuth), t);
					t = RotateAxis(t, randomAxis, wobble);
				}

				// 屈光性: 上方向へ引く
				t = SafeNormalize(XMVectorAdd(t, XMVectorScale(up, m_Params.Tropism / static_cast<float>(segments))), t);

				normal = TransportNormal(normal, tangent, t);
				tangent = t;

				segmentDirs[s] = t;
				position = XMVectorAdd(position, XMVectorScale(t, segmentLength));
				positions[s + 1] = position;
				normals[s + 1] = normal;
			}

			// ---- 骨格点 (接線は隣接セグメントの平均で滑らかにする) ----
			stem.Points.resize(segments + 1);
			for (int i = 0; i <= segments; ++i)
			{
				XMVECTOR pointTangent;
				if (i == 0)              pointTangent = segmentDirs[0];
				else if (i == segments)  pointTangent = segmentDirs[segments - 1];
				else                     pointTangent = SafeNormalize(XMVectorAdd(segmentDirs[i - 1], segmentDirs[i]), segmentDirs[i]);

				const float f = static_cast<float>(i) / static_cast<float>(segments);

				FStemPoint& pt = stem.Points[i];
				pt.Position = Store3(positions[i]);
				pt.Tangent = Store3(pointTangent);
				pt.Normal = Store3(Orthonormalize(normals[i], pointTangent));
				pt.Radius = Lerp(stem.BaseRadius, stem.TipRadius, f);
				pt.Distance = f * Length;
			}

			// ---- 子枝の仕様を先に決める (乱数の消費順を骨格 -> 子 -> 葉で固定) ----
			std::vector<FChildSpec> children;
			const int childLevel = Level + 1;
			if (childLevel < m_Params.Levels)
			{
				const FProceduralTreeLevelParams& cp = m_Params.Level[childLevel];
				const int count = cp.Branches;
				const float baseOffset = Clamp(lp.BaseOffset, 0.0f, 0.95f);
				float phi = m_Random.Range(0.0f, PT_TWO_PI);

				for (int k = 0; k < count; ++k)
				{
					const float t = Clamp(baseOffset + (1.0f - baseOffset)
						* (static_cast<float>(k) + 0.5f + m_Random.Signed(0.25f)) / static_cast<float>(count),
						baseOffset, 1.0f);
					phi += (cp.Rotate + m_Random.Signed(cp.RotateVariance)) * PT_DEG2RAD;

					const FStemSample s = SampleStem(stem, t);
					const XMVECTOR radialDir = RotateAxis(s.Normal, s.Tangent, phi);

					const float down = (cp.DownAngle + m_Random.Signed(cp.DownAngleVariance)) * PT_DEG2RAD;
					const XMVECTOR childDir = SafeNormalize(
						XMVectorAdd(XMVectorScale(s.Tangent, cosf(down)), XMVectorScale(radialDir, sinf(down))), s.Tangent);
					const XMVECTOR childNormal = Orthonormalize(radialDir, childDir);

					// 親の先端側ほど短く (樹冠の形)
					const float shape = Lerp(1.0f, cp.LengthShape, (t - baseOffset) / fmaxf(1.0f - baseOffset, 1.0e-3f));
					const float childLength = fmaxf(0.01f,
						Length * cp.Length * shape * (1.0f + m_Random.Signed(cp.LengthVariance)));
					const float childRadius = fmaxf(MinStemRadius, fminf(s.Radius * cp.Radius, s.Radius * 0.9f));

					FChildSpec spec;
					spec.Origin = Store3(s.Position);
					spec.Direction = Store3(childDir);
					spec.Normal = Store3(childNormal);
					spec.Length = childLength;
					spec.Radius = childRadius;
					children.push_back(spec);
				}
			}

			// ---- ジオメトリ ----
			BuildBark(stem);
			++m_NumStems;

			if (m_Params.bGenerateLeaves && Level == m_Params.Levels - 1)
			{
				BuildLeaves(stem);
			}

			// ---- 再帰 ----
			for (const FChildSpec& child : children)
			{
				GenerateStem(childLevel, Load3(child.Origin), Load3(child.Direction), Load3(child.Normal),
					child.Length, child.Radius);
			}
		}

	public:
		explicit FTreeGenerator(const FProceduralTreeParams& Params)
			: m_Params(Params)
			, m_Random(Params.Seed)
		{
			m_Bark.MaterialIndex = FProceduralTreeBuilder::BarkMaterialIndex;
			m_Leaves.MaterialIndex = FProceduralTreeBuilder::LeafMaterialIndex;
			m_Leaves.bAffectDistanceField = false;	// 開いた面は SDF の符号判定を乱す
		}

		FProceduralMeshData Run(FProceduralTreeStats* OutStats)
		{
			const FProceduralTreeLevelParams& trunk = m_Params.Level[0];
			const float length = fmaxf(0.05f, trunk.Length * (1.0f + m_Random.Signed(trunk.LengthVariance)));

			GenerateStem(0,
				XMVectorZero(),
				XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f),
				XMVectorSet(1.0f, 0.0f, 0.0f, 0.0f),
				length, trunk.Radius);

			FProceduralMeshData data;
			data.Sections.push_back(std::move(m_Bark));
			data.Sections.push_back(std::move(m_Leaves));

			if (OutStats)
			{
				OutStats->NumStems = m_NumStems;
				OutStats->NumLeaves = m_NumLeaves;
				OutStats->NumVertices = data.GetNumVertices();
				OutStats->NumTriangles = data.GetNumTriangles();
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
	// ---- Level 0: 幹 ----
	{
		FProceduralTreeLevelParams& l = Level[0];
		l.Branches = 0;
		l.Length = 4.0f;
		l.LengthVariance = 0.0f;
		l.LengthShape = 1.0f;
		l.Radius = 0.16f;
		l.Taper = 0.55f;
		l.DownAngle = 0.0f;
		l.DownAngleVariance = 0.0f;
		l.Rotate = 0.0f;
		l.RotateVariance = 0.0f;
		l.Curve = 0.0f;
		l.CurveVariance = 12.0f;
		l.BaseOffset = 0.35f;
		l.Segments = 10;
		l.RadialSegments = 14;
	}
	// ---- Level 1: 幹から出る主枝 ----
	{
		FProceduralTreeLevelParams& l = Level[1];
		l.Branches = 14;
		l.Length = 0.55f;
		l.LengthVariance = 0.15f;
		l.LengthShape = 0.45f;
		l.Radius = 0.6f;
		l.Taper = 0.3f;
		l.DownAngle = 60.0f;
		l.DownAngleVariance = 12.0f;
		l.Rotate = 137.5f;
		l.RotateVariance = 25.0f;
		l.Curve = 20.0f;
		l.CurveVariance = 35.0f;
		l.BaseOffset = 0.15f;
		l.Segments = 7;
		l.RadialSegments = 8;
	}
	// ---- Level 2: 側枝 ----
	{
		FProceduralTreeLevelParams& l = Level[2];
		l.Branches = 7;
		l.Length = 0.5f;
		l.LengthVariance = 0.2f;
		l.LengthShape = 0.7f;
		l.Radius = 0.55f;
		l.Taper = 0.25f;
		l.DownAngle = 50.0f;
		l.DownAngleVariance = 15.0f;
		l.Rotate = 137.5f;
		l.RotateVariance = 30.0f;
		l.Curve = 10.0f;
		l.CurveVariance = 50.0f;
		l.BaseOffset = 0.1f;
		l.Segments = 4;
		l.RadialSegments = 6;
	}
	// ---- Level 3: 小枝 (Levels = 4 のとき) ----
	{
		FProceduralTreeLevelParams& l = Level[3];
		l.Branches = 4;
		l.Length = 0.5f;
		l.LengthVariance = 0.2f;
		l.LengthShape = 0.8f;
		l.Radius = 0.55f;
		l.Taper = 0.2f;
		l.DownAngle = 45.0f;
		l.DownAngleVariance = 20.0f;
		l.Rotate = 137.5f;
		l.RotateVariance = 40.0f;
		l.Curve = 0.0f;
		l.CurveVariance = 60.0f;
		l.BaseOffset = 0.1f;
		l.Segments = 3;
		l.RadialSegments = 5;
	}
}

void FProceduralTreeParams::Sanitize()
{
	Levels = ClampInt(Levels, 1, MaxLevels);
	BarkUVTilesPerMeter = Clamp(BarkUVTilesPerMeter, 0.01f, 100.0f);
	Tropism = Clamp(Tropism, -2.0f, 2.0f);

	LeavesPerBranch = ClampInt(LeavesPerBranch, 0, 64);
	LeafSize = Clamp(LeafSize, 0.01f, 5.0f);
	LeafSizeVariance = Clamp(LeafSizeVariance, 0.0f, 0.95f);
	LeafAspect = Clamp(LeafAspect, 0.1f, 3.0f);
	LeafAngle = Clamp(LeafAngle, 0.0f, 180.0f);
	LeafAngleVariance = Clamp(LeafAngleVariance, 0.0f, 90.0f);
	LeafTintVariance = Clamp(LeafTintVariance, 0.0f, 1.0f);

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
	}

	// 枝の総数 (1 + b1 + b1 b2 + ...) を上限内に収める: 最深階層から枝数を減らす
	auto EstimateStems = [this]() -> unsigned long long
		{
			unsigned long long total = 1;
			unsigned long long product = 1;
			for (int i = 1; i < Levels; ++i)
			{
				product *= static_cast<unsigned long long>(Level[i].Branches);
				total += product;
			}
			return total;
		};
	while (EstimateStems() > MaxTotalStems)
	{
		int deepest = -1;
		for (int i = Levels - 1; i >= 1; --i)
		{
			if (Level[i].Branches > 1) { deepest = i; break; }
		}
		if (deepest < 0) break;
		--Level[deepest].Branches;
	}
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
	MixI(static_cast<int>(Seed));
	MixI(Levels);
	MixF(Tropism);
	MixF(BarkUVTilesPerMeter);
	MixI(bGenerateLeaves ? 1 : 0);
	MixI(LeavesPerBranch);
	MixF(LeafSize);
	MixF(LeafSizeVariance);
	MixF(LeafAspect);
	MixF(LeafAngle);
	MixF(LeafAngleVariance);
	MixF(LeafTintVariance);

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
	}
	return hash;
}


// ============================================================
//  FProceduralTreeBuilder
// ============================================================

FProceduralMeshData FProceduralTreeBuilder::Build(const FProceduralTreeParams& Params, FProceduralTreeStats* OutStats)
{
	FProceduralTreeParams sanitized = Params;
	sanitized.Sanitize();

	FTreeGenerator generator(sanitized);
	return generator.Run(OutStats);
}


// ============================================================
//  FProceduralTreeTextures
//  RGBA8 + ミップチェーンの DDS (DX10 ヘッダ) を直接書き出す。
// ============================================================
namespace
{
	struct FImageRGBA
	{
		unsigned int Width = 0;
		unsigned int Height = 0;
		std::vector<unsigned char> Pixels;	// RGBA8、行は詰めて並ぶ

		FImageRGBA(unsigned int W, unsigned int H) : Width(W), Height(H), Pixels(static_cast<size_t>(W) * H * 4, 0) {}

		void Set(unsigned int X, unsigned int Y, float R, float G, float B, float A)
		{
			unsigned char* p = &Pixels[(static_cast<size_t>(Y) * Width + X) * 4];
			p[0] = static_cast<unsigned char>(Clamp(R, 0.0f, 1.0f) * 255.0f + 0.5f);
			p[1] = static_cast<unsigned char>(Clamp(G, 0.0f, 1.0f) * 255.0f + 0.5f);
			p[2] = static_cast<unsigned char>(Clamp(B, 0.0f, 1.0f) * 255.0f + 0.5f);
			p[3] = static_cast<unsigned char>(Clamp(A, 0.0f, 1.0f) * 255.0f + 0.5f);
		}
	};

	// 2x2 ボックスフィルタで 1 段小さいミップを作る
	FImageRGBA Downsample(const FImageRGBA& Src)
	{
		const unsigned int w = (Src.Width > 1) ? Src.Width / 2 : 1;
		const unsigned int h = (Src.Height > 1) ? Src.Height / 2 : 1;
		FImageRGBA dst(w, h);

		for (unsigned int y = 0; y < h; ++y)
		{
			for (unsigned int x = 0; x < w; ++x)
			{
				const unsigned int x0 = (x * 2 < Src.Width) ? x * 2 : Src.Width - 1;
				const unsigned int y0 = (y * 2 < Src.Height) ? y * 2 : Src.Height - 1;
				const unsigned int x1 = (x0 + 1 < Src.Width) ? x0 + 1 : x0;
				const unsigned int y1 = (y0 + 1 < Src.Height) ? y0 + 1 : y0;

				for (unsigned int c = 0; c < 4; ++c)
				{
					const unsigned int sum =
						Src.Pixels[(static_cast<size_t>(y0) * Src.Width + x0) * 4 + c] +
						Src.Pixels[(static_cast<size_t>(y0) * Src.Width + x1) * 4 + c] +
						Src.Pixels[(static_cast<size_t>(y1) * Src.Width + x0) * 4 + c] +
						Src.Pixels[(static_cast<size_t>(y1) * Src.Width + x1) * 4 + c];
					dst.Pixels[(static_cast<size_t>(y) * w + x) * 4 + c] = static_cast<unsigned char>((sum + 2) / 4);
				}
			}
		}
		return dst;
	}

	bool WriteDDS(const std::string& Path, const FImageRGBA& Image)
	{
		// ---- ミップチェーン ----
		std::vector<FImageRGBA> mips;
		mips.push_back(Image);
		while (mips.back().Width > 1 || mips.back().Height > 1)
		{
			mips.push_back(Downsample(mips.back()));
		}

		// ---- DDS ヘッダ (DDS_HEADER 124 bytes + DDS_HEADER_DXT10 20 bytes) ----
		unsigned int header[31] = {};
		header[0] = 124;								// dwSize
		header[1] = 0x1 | 0x2 | 0x4 | 0x8 | 0x1000 | 0x20000;	// CAPS | HEIGHT | WIDTH | PITCH | PIXELFORMAT | MIPMAPCOUNT
		header[2] = Image.Height;
		header[3] = Image.Width;
		header[4] = Image.Width * 4;					// dwPitchOrLinearSize
		header[5] = 0;									// dwDepth
		header[6] = static_cast<unsigned int>(mips.size());
		// header[7..17] reserved1
		header[18] = 32;								// ddspf.dwSize
		header[19] = 0x4;								// DDPF_FOURCC
		header[20] = 0x30315844;						// "DX10"
		// header[21..25] = 0 (bit counts / masks)
		header[26] = 0x1000 | 0x8 | 0x400000;			// DDSCAPS_TEXTURE | COMPLEX | MIPMAP
		// header[27..30] caps2..4 / reserved2

		unsigned int dx10[5] = {};
		dx10[0] = 28;	// DXGI_FORMAT_R8G8B8A8_UNORM (BaseColor は DDS_LOADER_FORCE_SRGB で sRGB 化される)
		dx10[1] = 3;	// D3D10_RESOURCE_DIMENSION_TEXTURE2D
		dx10[2] = 0;	// miscFlag
		dx10[3] = 1;	// arraySize
		dx10[4] = 0;	// miscFlags2 (alpha mode unknown)

		std::ofstream file(Path, std::ios_base::out | std::ios_base::binary);
		if (!file) return false;

		file.write("DDS ", 4);
		file.write(reinterpret_cast<const char*>(header), sizeof(header));
		file.write(reinterpret_cast<const char*>(dx10), sizeof(dx10));
		for (const FImageRGBA& mip : mips)
		{
			file.write(reinterpret_cast<const char*>(mip.Pixels.data()), mip.Pixels.size());
		}
		return file.good();
	}

	// ---- タイル可能な格子ノイズ (値ノイズ, smoothstep 補間) ----
	inline float LatticeHash(int X, int Y, unsigned int Seed)
	{
		unsigned int n = static_cast<unsigned int>(X) * 374761393u
			+ static_cast<unsigned int>(Y) * 668265263u
			+ Seed * 1013904223u;
		n = (n ^ (n >> 13)) * 1274126177u;
		n = n ^ (n >> 16);
		return static_cast<float>(n) * (1.0f / 4294967296.0f);
	}

	inline int WrapInt(int V, int Period)
	{
		const int m = V % Period;
		return (m < 0) ? m + Period : m;
	}

	// U / V (0..1) をそれぞれ PeriodX / PeriodY 個の格子で評価する。両方向にタイルする
	float TileableValueNoise(float U, float V, int PeriodX, int PeriodY, unsigned int Seed)
	{
		const float x = U * static_cast<float>(PeriodX);
		const float y = V * static_cast<float>(PeriodY);
		const int ix = static_cast<int>(floorf(x));
		const int iy = static_cast<int>(floorf(y));
		const float fx = x - static_cast<float>(ix);
		const float fy = y - static_cast<float>(iy);
		const float sx = fx * fx * (3.0f - 2.0f * fx);
		const float sy = fy * fy * (3.0f - 2.0f * fy);

		const float n00 = LatticeHash(WrapInt(ix, PeriodX), WrapInt(iy, PeriodY), Seed);
		const float n10 = LatticeHash(WrapInt(ix + 1, PeriodX), WrapInt(iy, PeriodY), Seed);
		const float n01 = LatticeHash(WrapInt(ix, PeriodX), WrapInt(iy + 1, PeriodY), Seed);
		const float n11 = LatticeHash(WrapInt(ix + 1, PeriodX), WrapInt(iy + 1, PeriodY), Seed);

		return Lerp(Lerp(n00, n10, sx), Lerp(n01, n11, sx), sy);
	}

	// オクターブごとに周期を 2 倍にして重ねる (0..1 に正規化)
	float TileableFBM(float U, float V, int PeriodX, int PeriodY, int Octaves, unsigned int Seed)
	{
		float sum = 0.0f;
		float amplitude = 1.0f;
		float total = 0.0f;
		int px = PeriodX;
		int py = PeriodY;
		for (int o = 0; o < Octaves; ++o)
		{
			sum += TileableValueNoise(U, V, px, py, Seed + static_cast<unsigned int>(o) * 7919u) * amplitude;
			total += amplitude;
			amplitude *= 0.5f;
			px *= 2;
			py *= 2;
		}
		return (total > 0.0f) ? sum / total : 0.0f;
	}

	// ---- 樹皮 (BaseColor / Normal / ARM を同じハイトフィールドから作る) ----
	bool GenerateBarkTextures(const std::string& DiffPath, const std::string& NormalPath, const std::string& ARMPath)
	{
		const unsigned int size = 512;
		FImageRGBA diff(size, size);
		FImageRGBA normal(size, size);
		FImageRGBA arm(size, size);
		std::vector<float> height(static_cast<size_t>(size) * size);

		for (unsigned int y = 0; y < size; ++y)
		{
			for (unsigned int x = 0; x < size; ++x)
			{
				const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(size);
				const float v = (static_cast<float>(y) + 0.5f) / static_cast<float>(size);

				// 縦に伸びた畝 (U 方向に細かく、V 方向に粗く) + 細部
				const float ridges = TileableFBM(u, v, 12, 3, 5, 11u);
				const float fine = TileableFBM(u, v, 48, 48, 3, 23u);
				const float h = Clamp(0.65f * ridges + 0.35f * fine, 0.0f, 1.0f);
				height[static_cast<size_t>(y) * size + x] = h;

				const float hv = SmoothStep(0.35f, 0.62f, h);

				// 溝 = 暗い焦げ茶、畝 = 明るい茶。低周波で苔のような緑を混ぜる
				float r = Lerp(0.16f, 0.46f, hv);
				float g = Lerp(0.10f, 0.33f, hv);
				float b = Lerp(0.06f, 0.21f, hv);
				const float moss = SmoothStep(0.62f, 0.80f, TileableFBM(u, v, 4, 4, 3, 5u)) * 0.55f;
				r = Lerp(r, 0.22f, moss);
				g = Lerp(g, 0.30f, moss);
				b = Lerp(b, 0.12f, moss);

				diff.Set(x, y, r, g, b, 1.0f);

				// AO は溝で暗く、ラフネスは畝でわずかに低く。メタリック 0
				arm.Set(x, y, 0.55f + 0.45f * hv, 0.90f - 0.15f * hv, 0.0f, 1.0f);
			}
		}

		// ---- 法線 (ハイトフィールドの差分、タイル境界はラップ) ----
		const float strength = 10.0f;
		for (unsigned int y = 0; y < size; ++y)
		{
			for (unsigned int x = 0; x < size; ++x)
			{
				const unsigned int xm = (x + size - 1) % size;
				const unsigned int xp = (x + 1) % size;
				const unsigned int ym = (y + size - 1) % size;
				const unsigned int yp = (y + 1) % size;

				const float dhdu = (height[static_cast<size_t>(y) * size + xp] - height[static_cast<size_t>(y) * size + xm]) * 0.5f;
				const float dhdv = (height[static_cast<size_t>(yp) * size + x] - height[static_cast<size_t>(ym) * size + x]) * 0.5f;

				XMFLOAT3 n{ -dhdu * strength, -dhdv * strength, 1.0f };
				const float len = sqrtf(n.x * n.x + n.y * n.y + n.z * n.z);
				n.x /= len; n.y /= len; n.z /= len;

				// 接線空間 (T = +U, B = cross(N, T) = +V, N) を 0..1 に符号化 (既存アセットの nor_dx と同じ規約)
				normal.Set(x, y, 0.5f + 0.5f * n.x, 0.5f + 0.5f * n.y, 0.5f + 0.5f * n.z, 1.0f);
			}
		}

		return WriteDDS(DiffPath, diff) && WriteDDS(NormalPath, normal) && WriteDDS(ARMPath, arm);
	}

	// ---- 葉 (α = 葉の形、V = 0 が葉先 / V = 1 が付け根) ----
	bool GenerateLeafTextures(const std::string& DiffPath, const std::string& ARMPath)
	{
		const unsigned int size = 256;
		FImageRGBA diff(size, size);

		const float texel = 1.0f / static_cast<float>(size);

		for (unsigned int y = 0; y < size; ++y)
		{
			for (unsigned int x = 0; x < size; ++x)
			{
				const float u = (static_cast<float>(x) + 0.5f) * texel;
				const float v = (static_cast<float>(y) + 0.5f) * texel;
				const float du = fabsf(u - 0.5f);

				// 半幅のプロファイル: 葉先で 0、やや付け根寄りで最大、付け根で葉柄へ絞る
				const float vp = powf(v, 0.8f);
				float halfWidth = 0.46f * powf(fmaxf(sinf(PT_PI * vp), 0.0f), 0.6f);
				halfWidth += 0.012f * (TileableValueNoise(0.0f, v, 1, 24, 3u) - 0.5f);	// 縁のゆらぎ
				const float edge = du - halfWidth;

				// 葉身 (1 テクセル幅でアンチエイリアス)
				float alpha = Clamp(0.5f - edge / texel, 0.0f, 1.0f);
				// 葉柄 (付け根側の細い茎)
				if (v > 0.88f)
				{
					alpha = fmaxf(alpha, Clamp(0.5f - (du - 0.018f) / texel, 0.0f, 1.0f));
				}

				// 色: 中央 / 葉先寄りを明るく、縁を濃く
				const float center = 1.0f - Clamp(du / fmaxf(halfWidth, 0.02f), 0.0f, 1.0f);
				float r = Lerp(0.24f, 0.42f, center * 0.6f);
				float g = Lerp(0.50f, 0.70f, center * 0.6f);
				float b = Lerp(0.13f, 0.22f, center * 0.6f);

				// まだら模様
				const float mottle = 0.88f + 0.24f * TileableFBM(u, v, 6, 6, 3, 31u);
				r *= mottle; g *= mottle; b *= mottle;

				// 主脈 / 側脈
				if (du < 0.012f && v < 0.95f)
				{
					r *= 0.70f; g *= 0.72f; b *= 0.70f;
				}
				else
				{
					const float vein = fmodf(v * 10.0f + du * 6.0f, 1.0f);
					if (vein < 0.07f)
					{
						r *= 0.84f; g *= 0.86f; b *= 0.84f;
					}
				}

				diff.Set(x, y, r, g, b, alpha);
			}
		}

		// ARM: AO 1 / ラフネス 0.6 / メタリック 0 (定数)
		FImageRGBA arm(4, 4);
		for (unsigned int y = 0; y < 4; ++y)
			for (unsigned int x = 0; x < 4; ++x)
				arm.Set(x, y, 1.0f, 0.6f, 0.0f, 1.0f);

		return WriteDDS(DiffPath, diff) && WriteDDS(ARMPath, arm);
	}

	bool GenerateFlatNormal(const std::string& Path)
	{
		FImageRGBA flat(4, 4);
		for (unsigned int y = 0; y < 4; ++y)
			for (unsigned int x = 0; x < 4; ++x)
				flat.Set(x, y, 0.5f, 0.5f, 1.0f, 1.0f);
		return WriteDDS(Path, flat);
	}

	std::string TexturePath(const char* Name)
	{
		char buffer[128];
		sprintf_s(buffer, "Saved/ProceduralTexture/Tree%s_v%u.dds", Name, FProceduralTreeTextures::Version);
		return buffer;
	}
}

std::string FProceduralTreeTextures::GetDirectory() { return "Saved/ProceduralTexture"; }
std::string FProceduralTreeTextures::GetBarkBaseColorPath() { return TexturePath("Bark_diff"); }
std::string FProceduralTreeTextures::GetBarkNormalPath() { return TexturePath("Bark_nor_dx"); }
std::string FProceduralTreeTextures::GetBarkARMPath() { return TexturePath("Bark_arm"); }
std::string FProceduralTreeTextures::GetLeafBaseColorPath() { return TexturePath("Leaf_diff"); }
std::string FProceduralTreeTextures::GetLeafARMPath() { return TexturePath("Leaf_arm"); }
std::string FProceduralTreeTextures::GetFlatNormalPath() { return TexturePath("Flat_nor_dx"); }

bool FProceduralTreeTextures::EnsureGenerated()
{
	std::error_code ec;
	fs::create_directories(GetDirectory(), ec);
	if (ec)
	{
		OutputDebugStringA("[ProceduralTree] failed to create Saved/ProceduralTexture\n");
		return false;
	}

	auto Exists = [](const std::string& Path) { return fs::exists(Path); };

	bool ok = true;

	if (!Exists(GetBarkBaseColorPath()) || !Exists(GetBarkNormalPath()) || !Exists(GetBarkARMPath()))
	{
		ok &= GenerateBarkTextures(GetBarkBaseColorPath(), GetBarkNormalPath(), GetBarkARMPath());
	}
	if (!Exists(GetLeafBaseColorPath()) || !Exists(GetLeafARMPath()))
	{
		ok &= GenerateLeafTextures(GetLeafBaseColorPath(), GetLeafARMPath());
	}
	if (!Exists(GetFlatNormalPath()))
	{
		ok &= GenerateFlatNormal(GetFlatNormalPath());
	}

	if (!ok)
	{
		OutputDebugStringA("[ProceduralTree] texture generation failed\n");
	}
	return ok;
}
