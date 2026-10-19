#include "Main.h"
#include "ProceduralMeshGenerator.h"

#include <random>
#include <unordered_map>
#include <cmath>

namespace
{
	constexpr float PM_PI = 3.14159265358979323846f;
	constexpr float PM_TWO_PI = 6.28318530717958647692f;

	inline XMVECTOR Load(const XMFLOAT3& V) { return XMLoadFloat3(&V); }

	inline XMFLOAT3 Store(FXMVECTOR V)
	{
		XMFLOAT3 out;
		XMStoreFloat3(&out, V);
		return out;
	}

	inline XMFLOAT3 Normalize(const XMFLOAT3& V, const XMFLOAT3& Fallback = { 0.0f, 1.0f, 0.0f })
	{
		XMVECTOR v = Load(V);
		if (XMVectorGetX(XMVector3LengthSq(v)) < 1.0e-20f)
		{
			return Fallback;
		}
		return Store(XMVector3Normalize(v));
	}

	// 巻き順 (v0, v1, v2) の幾何法線 (正規化なし、面積の 2 倍の長さ)
	inline XMVECTOR GeometricNormal(const XMFLOAT3& P0, const XMFLOAT3& P1, const XMFLOAT3& P2)
	{
		return XMVector3Cross(XMVectorSubtract(Load(P1), Load(P0)),
			XMVectorSubtract(Load(P2), Load(P0)));
	}

	inline VERTEX_3D MakeVertex(const XMFLOAT3& Position, const XMFLOAT3& Normal, const XMFLOAT2& UV)
	{
		VERTEX_3D v;
		v.Position = Position;
		v.Normal = Normal;
		v.Tangent = { 1.0f, 0.0f, 0.0f };
		v.TexCoord = UV;
		v.Color = { 1.0f, 1.0f, 1.0f, 1.0f };
		return v;
	}

	// 法線と直交する任意の接線 (BuildTBN のフォールバックと同じ規則)
	inline XMFLOAT3 FallbackTangent(const XMFLOAT3& N)
	{
		const XMFLOAT3 up = (fabsf(N.y) < 0.999f) ? XMFLOAT3{ 0.0f, 1.0f, 0.0f } : XMFLOAT3{ 1.0f, 0.0f, 0.0f };
		return Normalize(Store(XMVector3Cross(Load(up), Load(N))), { 1.0f, 0.0f, 0.0f });
	}
}


// ============================================================
//  FProceduralMeshData
// ============================================================

bool FProceduralMeshData::IsEmpty() const
{
	for (const FProceduralMeshSection& section : Sections)
	{
		if (!section.IsEmpty()) return false;
	}
	return true;
}

unsigned int FProceduralMeshData::GetNumVertices() const
{
	unsigned int count = 0;
	for (const FProceduralMeshSection& section : Sections)
	{
		count += static_cast<unsigned int>(section.Vertices.size());
	}
	return count;
}

unsigned int FProceduralMeshData::GetNumTriangles() const
{
	unsigned int count = 0;
	for (const FProceduralMeshSection& section : Sections)
	{
		count += section.GetNumTriangles();
	}
	return count;
}

FProceduralMeshData FProceduralMeshData::FromSection(FProceduralMeshSection Section)
{
	FProceduralMeshData data;
	data.Sections.emplace_back(std::move(Section));
	return data;
}

unsigned long long FProceduralMeshData::ComputeGeometryHash() const
{
	// FNV-1a 64bit
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

	const unsigned int sectionCount = static_cast<unsigned int>(Sections.size());
	Mix(&sectionCount, sizeof(sectionCount));

	for (const FProceduralMeshSection& section : Sections)
	{
		for (const VERTEX_3D& v : section.Vertices)
		{
			Mix(&v.Position, sizeof(v.Position));
		}
		if (!section.Indices.empty())
		{
			Mix(section.Indices.data(), section.Indices.size() * sizeof(unsigned int));
		}
	}
	return hash;
}


// ============================================================
//  FProceduralNoise (改良版 Perlin ノイズ)
// ============================================================

FProceduralNoise::FProceduralNoise(unsigned int Seed)
{
	unsigned char perm[256];
	for (int i = 0; i < 256; ++i)
	{
		perm[i] = static_cast<unsigned char>(i);
	}

	std::mt19937 rng(Seed);
	for (int i = 255; i > 0; --i)
	{
		std::uniform_int_distribution<int> dist(0, i);
		const int j = dist(rng);
		std::swap(perm[i], perm[j]);
	}

	for (int i = 0; i < 512; ++i)
	{
		m_Perm[i] = perm[i & 255];
	}
}

float FProceduralNoise::Grad(int hash, float x, float y, float z)
{
	// 12 本の勾配ベクトル (立方体の辺の中点方向) から選ぶ
	const int h = hash & 15;
	const float u = (h < 8) ? x : y;
	const float v = (h < 4) ? y : ((h == 12 || h == 14) ? x : z);
	return (((h & 1) == 0) ? u : -u) + (((h & 2) == 0) ? v : -v);
}

float FProceduralNoise::Noise3D(float x, float y, float z) const
{
	const float fx = floorf(x);
	const float fy = floorf(y);
	const float fz = floorf(z);

	const int X = static_cast<int>(fx) & 255;
	const int Y = static_cast<int>(fy) & 255;
	const int Z = static_cast<int>(fz) & 255;

	x -= fx;
	y -= fy;
	z -= fz;

	const float u = Fade(x);
	const float v = Fade(y);
	const float w = Fade(z);

	const int A = m_Perm[X] + Y;
	const int AA = m_Perm[A] + Z;
	const int AB = m_Perm[A + 1] + Z;
	const int B = m_Perm[X + 1] + Y;
	const int BA = m_Perm[B] + Z;
	const int BB = m_Perm[B + 1] + Z;

	return Lerp(
		Lerp(
			Lerp(Grad(m_Perm[AA], x, y, z), Grad(m_Perm[BA], x - 1.0f, y, z), u),
			Lerp(Grad(m_Perm[AB], x, y - 1.0f, z), Grad(m_Perm[BB], x - 1.0f, y - 1.0f, z), u),
			v),
		Lerp(
			Lerp(Grad(m_Perm[AA + 1], x, y, z - 1.0f), Grad(m_Perm[BA + 1], x - 1.0f, y, z - 1.0f), u),
			Lerp(Grad(m_Perm[AB + 1], x, y - 1.0f, z - 1.0f), Grad(m_Perm[BB + 1], x - 1.0f, y - 1.0f, z - 1.0f), u),
			v),
		w);
}

float FProceduralNoise::Noise2D(float x, float y) const
{
	// 3D ノイズの z = 定数スライス (格子点を避けるため半端な値)
	return Noise3D(x, y, 0.37f);
}

float FProceduralNoise::FBM2D(float x, float y, unsigned int Octaves, float Lacunarity, float Persistence) const
{
	float sum = 0.0f;
	float amplitude = 1.0f;
	float frequency = 1.0f;
	float norm = 0.0f;

	for (unsigned int i = 0; i < Octaves; ++i)
	{
		sum += Noise2D(x * frequency, y * frequency) * amplitude;
		norm += amplitude;
		amplitude *= Persistence;
		frequency *= Lacunarity;
	}
	return (norm > 0.0f) ? sum / norm : 0.0f;
}

float FProceduralNoise::FBM3D(float x, float y, float z, unsigned int Octaves, float Lacunarity, float Persistence) const
{
	float sum = 0.0f;
	float amplitude = 1.0f;
	float frequency = 1.0f;
	float norm = 0.0f;

	for (unsigned int i = 0; i < Octaves; ++i)
	{
		sum += Noise3D(x * frequency, y * frequency, z * frequency) * amplitude;
		norm += amplitude;
		amplitude *= Persistence;
		frequency *= Lacunarity;
	}
	return (norm > 0.0f) ? sum / norm : 0.0f;
}


// ============================================================
//  FProceduralMeshBuilder : 内部ヘルパ
// ============================================================

void FProceduralMeshBuilder::AddTriangleOriented(FProceduralMeshSection& Section,
	unsigned int I0, unsigned int I1, unsigned int I2)
{
	const VERTEX_3D& v0 = Section.Vertices[I0];
	const VERTEX_3D& v1 = Section.Vertices[I1];
	const VERTEX_3D& v2 = Section.Vertices[I2];

	const XMVECTOR geometric = GeometricNormal(v0.Position, v1.Position, v2.Position);

	// 縮退三角形 (極など) は追加しない
	if (XMVectorGetX(XMVector3LengthSq(geometric)) < 1.0e-16f)
	{
		return;
	}

	const XMVECTOR reference = XMVectorAdd(XMVectorAdd(Load(v0.Normal), Load(v1.Normal)), Load(v2.Normal));
	const bool bFlip = XMVectorGetX(XMVector3Dot(geometric, reference)) < 0.0f;

	Section.Indices.push_back(I0);
	Section.Indices.push_back(bFlip ? I2 : I1);
	Section.Indices.push_back(bFlip ? I1 : I2);
}


// ============================================================
//  パラメトリック曲面
// ============================================================

FProceduralMeshSection FProceduralMeshBuilder::CreateParametricSurface(
	const FSurfaceFunc& Surface, unsigned int ColumnsU, unsigned int RowsV, const XMFLOAT2& UVScale)
{
	FProceduralMeshSection section;

	const unsigned int cols = (ColumnsU < 1) ? 1 : ColumnsU;
	const unsigned int rows = (RowsV < 1) ? 1 : RowsV;

	section.Vertices.reserve((cols + 1) * (rows + 1));
	section.Indices.reserve(cols * rows * 6);

	for (unsigned int r = 0; r <= rows; ++r)
	{
		const float v = static_cast<float>(r) / static_cast<float>(rows);
		for (unsigned int c = 0; c <= cols; ++c)
		{
			const float u = static_cast<float>(c) / static_cast<float>(cols);

			XMFLOAT3 position{};
			XMFLOAT3 normal{ 0.0f, 1.0f, 0.0f };
			Surface(u, v, position, normal);

			section.Vertices.push_back(MakeVertex(position, Normalize(normal),
				{ u * UVScale.x, v * UVScale.y }));
		}
	}

	// 巻き順は頂点法線を基準に AddTriangleOriented が決める
	const unsigned int stride = cols + 1;
	for (unsigned int r = 0; r < rows; ++r)
	{
		for (unsigned int c = 0; c < cols; ++c)
		{
			const unsigned int i0 = r * stride + c;
			const unsigned int i1 = i0 + 1;
			const unsigned int i2 = i0 + stride;
			const unsigned int i3 = i2 + 1;

			AddTriangleOriented(section, i0, i1, i2);
			AddTriangleOriented(section, i1, i3, i2);
		}
	}

	ComputeTangents(section);
	return section;
}


// ============================================================
//  基本形状
// ============================================================

FProceduralMeshSection FProceduralMeshBuilder::CreateBox(const XMFLOAT3& HalfExtent, const XMFLOAT2& UVScale)
{
	FProceduralMeshSection section;
	section.Vertices.reserve(24);
	section.Indices.reserve(36);

	// 面ごとの (法線 N, 接線 T)。従法線 B = cross(N, T) が
	// テクスチャの +V 方向 (側面は -Y = 下向き) になるよう T を選ぶ
	struct FFace { XMFLOAT3 N; XMFLOAT3 T; };
	const FFace faces[6] =
	{
		{ {  0.0f,  0.0f,  1.0f }, { -1.0f,  0.0f,  0.0f } },	// +Z
		{ {  0.0f,  0.0f, -1.0f }, {  1.0f,  0.0f,  0.0f } },	// -Z
		{ {  1.0f,  0.0f,  0.0f }, {  0.0f,  0.0f,  1.0f } },	// +X
		{ { -1.0f,  0.0f,  0.0f }, {  0.0f,  0.0f, -1.0f } },	// -X
		{ {  0.0f,  1.0f,  0.0f }, {  1.0f,  0.0f,  0.0f } },	// +Y
		{ {  0.0f, -1.0f,  0.0f }, {  1.0f,  0.0f,  0.0f } },	// -Y
	};

	const XMVECTOR halfExtent = Load(HalfExtent);

	for (const FFace& face : faces)
	{
		const XMVECTOR n = Load(face.N);
		const XMVECTOR t = Load(face.T);
		const XMVECTOR b = XMVector3Cross(n, t);

		// 各軸方向の半幅 (軸の絶対値と HalfExtent の内積)
		const float en = XMVectorGetX(XMVector3Dot(XMVectorAbs(n), halfExtent));
		const float et = XMVectorGetX(XMVector3Dot(XMVectorAbs(t), halfExtent));
		const float eb = XMVectorGetX(XMVector3Dot(XMVectorAbs(b), halfExtent));

		const unsigned int base = static_cast<unsigned int>(section.Vertices.size());

		for (unsigned int iv = 0; iv < 2; ++iv)
		{
			for (unsigned int iu = 0; iu < 2; ++iu)
			{
				const float su = (iu == 0) ? -1.0f : 1.0f;
				const float sv = (iv == 0) ? -1.0f : 1.0f;

				XMVECTOR p = XMVectorScale(n, en);
				p = XMVectorAdd(p, XMVectorScale(t, su * et));
				p = XMVectorAdd(p, XMVectorScale(b, sv * eb));

				VERTEX_3D vertex = MakeVertex(Store(p), face.N,
					{ static_cast<float>(iu) * UVScale.x, static_cast<float>(iv) * UVScale.y });
				vertex.Tangent = face.T;
				section.Vertices.push_back(vertex);
			}
		}

		AddTriangleOriented(section, base + 0, base + 1, base + 2);
		AddTriangleOriented(section, base + 1, base + 3, base + 2);
	}

	return section;
}

FProceduralMeshSection FProceduralMeshBuilder::CreatePlane(float Width, float Depth,
	unsigned int SubdivisionsX, unsigned int SubdivisionsZ, const XMFLOAT2& UVScale)
{
	// U = +X, V = -Z (AField の地面クアッドと同じ向き)
	return CreateParametricSurface(
		[=](float u, float v, XMFLOAT3& OutPosition, XMFLOAT3& OutNormal)
		{
			OutPosition = { -Width * 0.5f + u * Width, 0.0f, Depth * 0.5f - v * Depth };
			OutNormal = { 0.0f, 1.0f, 0.0f };
		},
		SubdivisionsX, SubdivisionsZ, UVScale);
}

FProceduralMeshSection FProceduralMeshBuilder::CreateSphere(float Radius, unsigned int Slices, unsigned int Stacks)
{
	// u = 経度 (phi / 2pi), v = 緯度 (theta / pi、北極 = 0)
	return CreateParametricSurface(
		[=](float u, float v, XMFLOAT3& OutPosition, XMFLOAT3& OutNormal)
		{
			const float phi = u * PM_TWO_PI;
			const float theta = v * PM_PI;
			const float sinTheta = sinf(theta);

			OutNormal = { sinTheta * cosf(phi), cosf(theta), sinTheta * sinf(phi) };
			OutPosition = { OutNormal.x * Radius, OutNormal.y * Radius, OutNormal.z * Radius };
		},
		(Slices < 3) ? 3 : Slices, (Stacks < 2) ? 2 : Stacks);
}

FProceduralMeshSection FProceduralMeshBuilder::CreateIcoSphere(float Radius, unsigned int Subdivisions)
{
	// ---- 正二十面体 ----
	const float t = (1.0f + sqrtf(5.0f)) * 0.5f;

	std::vector<XMFLOAT3> positions =
	{
		{ -1.0f,  t, 0.0f }, {  1.0f,  t, 0.0f }, { -1.0f, -t, 0.0f }, {  1.0f, -t, 0.0f },
		{ 0.0f, -1.0f,  t }, { 0.0f,  1.0f,  t }, { 0.0f, -1.0f, -t }, { 0.0f,  1.0f, -t },
		{  t, 0.0f, -1.0f }, {  t, 0.0f,  1.0f }, { -t, 0.0f, -1.0f }, { -t, 0.0f,  1.0f },
	};
	for (XMFLOAT3& p : positions)
	{
		p = Normalize(p);
	}

	std::vector<unsigned int> faces =
	{
		0, 11, 5,   0, 5, 1,    0, 1, 7,    0, 7, 10,   0, 10, 11,
		1, 5, 9,    5, 11, 4,   11, 10, 2,  10, 7, 6,   7, 1, 8,
		3, 9, 4,    3, 4, 2,    3, 2, 6,    3, 6, 8,    3, 8, 9,
		4, 9, 5,    2, 4, 11,   6, 2, 10,   8, 6, 7,    9, 8, 1,
	};

	// ---- 中点分割 ----
	for (unsigned int level = 0; level < Subdivisions; ++level)
	{
		std::unordered_map<unsigned long long, unsigned int> midpointCache;

		auto GetMidpoint = [&](unsigned int a, unsigned int b) -> unsigned int
			{
				const unsigned long long key =
					(static_cast<unsigned long long>((a < b) ? a : b) << 32) | static_cast<unsigned long long>((a < b) ? b : a);

				auto it = midpointCache.find(key);
				if (it != midpointCache.end())
				{
					return it->second;
				}

				const XMFLOAT3& pa = positions[a];
				const XMFLOAT3& pb = positions[b];
				XMFLOAT3 mid{ (pa.x + pb.x) * 0.5f, (pa.y + pb.y) * 0.5f, (pa.z + pb.z) * 0.5f };
				positions.push_back(Normalize(mid));

				const unsigned int index = static_cast<unsigned int>(positions.size() - 1);
				midpointCache.emplace(key, index);
				return index;
			};

		std::vector<unsigned int> refined;
		refined.reserve(faces.size() * 4);

		for (size_t i = 0; i + 2 < faces.size(); i += 3)
		{
			const unsigned int a = faces[i];
			const unsigned int b = faces[i + 1];
			const unsigned int c = faces[i + 2];
			const unsigned int ab = GetMidpoint(a, b);
			const unsigned int bc = GetMidpoint(b, c);
			const unsigned int ca = GetMidpoint(c, a);

			const unsigned int quad[12] = { a, ab, ca,  b, bc, ab,  c, ca, bc,  ab, bc, ca };
			refined.insert(refined.end(), quad, quad + 12);
		}
		faces.swap(refined);
	}

	// ---- 頂点 (法線 = 位置方向、UV = 球面マッピング) ----
	FProceduralMeshSection section;
	section.Vertices.reserve(positions.size());

	auto SphericalUV = [](const XMFLOAT3& N) -> XMFLOAT2
		{
			float u = 0.5f + atan2f(N.z, N.x) / PM_TWO_PI;
			if (u < 0.0f) u += 1.0f;
			const float y = (N.y > 1.0f) ? 1.0f : ((N.y < -1.0f) ? -1.0f : N.y);
			const float v = acosf(y) / PM_PI;
			return { u, v };
		};

	for (const XMFLOAT3& n : positions)
	{
		section.Vertices.push_back(MakeVertex({ n.x * Radius, n.y * Radius, n.z * Radius }, n, SphericalUV(n)));
	}

	// ---- UV 継ぎ目の修正 ----
	// u が 1 -> 0 へ巻き戻る三角形は、u の小さい側の頂点を u + 1 で複製する
	const size_t sharedVertexCount = section.Vertices.size();
	for (size_t i = 0; i + 2 < faces.size(); i += 3)
	{
		unsigned int* tri = &faces[i];

		// 極 (u が不定) は継ぎ目判定から除く
		float uMin = 2.0f;
		float uMax = -1.0f;
		for (int k = 0; k < 3; ++k)
		{
			const VERTEX_3D& v = section.Vertices[tri[k]];
			if (fabsf(v.Normal.y) > 0.9999f) continue;
			uMin = fminf(uMin, v.TexCoord.x);
			uMax = fmaxf(uMax, v.TexCoord.x);
		}

		if (uMax - uMin > 0.5f)
		{
			for (int k = 0; k < 3; ++k)
			{
				VERTEX_3D& v = section.Vertices[tri[k]];
				if (fabsf(v.Normal.y) > 0.9999f) continue;
				if (v.TexCoord.x < 0.5f)
				{
					VERTEX_3D duplicated = v;
					duplicated.TexCoord.x += 1.0f;
					section.Vertices.push_back(duplicated);
					tri[k] = static_cast<unsigned int>(section.Vertices.size() - 1);
				}
			}
		}
	}

	// ---- 極の修正 ----
	// 極の頂点は u が不定なので、隣接三角形ごとに複製して残り 2 頂点の
	// u の平均を与える (接線のハンドネスが三角形ごとに一貫する)
	for (size_t i = 0; i + 2 < faces.size(); i += 3)
	{
		unsigned int* tri = &faces[i];
		for (int k = 0; k < 3; ++k)
		{
			if (tri[k] >= sharedVertexCount) continue;	// 既に複製済み
			const VERTEX_3D& pole = section.Vertices[tri[k]];
			if (fabsf(pole.Normal.y) <= 0.9999f) continue;

			const float uA = section.Vertices[tri[(k + 1) % 3]].TexCoord.x;
			const float uB = section.Vertices[tri[(k + 2) % 3]].TexCoord.x;

			VERTEX_3D duplicated = pole;
			duplicated.TexCoord.x = (uA + uB) * 0.5f;
			section.Vertices.push_back(duplicated);
			tri[k] = static_cast<unsigned int>(section.Vertices.size() - 1);
		}
	}

	section.Indices.reserve(faces.size());
	for (size_t i = 0; i + 2 < faces.size(); i += 3)
	{
		AddTriangleOriented(section, faces[i], faces[i + 1], faces[i + 2]);
	}

	ComputeTangents(section);
	return section;
}

FProceduralMeshSection FProceduralMeshBuilder::CreateCylinder(float BottomRadius, float TopRadius, float Height,
	unsigned int Slices, unsigned int Stacks, bool bCaps)
{
	const unsigned int slices = (Slices < 3) ? 3 : Slices;
	const float halfHeight = Height * 0.5f;

	// ---- 側面 (u = 周方向, v = 上 -> 下) ----
	// 円錐台の法線は (cos, (rb - rt) / h, sin) を正規化したもの
	FProceduralMeshSection section = CreateParametricSurface(
		[=](float u, float v, XMFLOAT3& OutPosition, XMFLOAT3& OutNormal)
		{
			const float phi = u * PM_TWO_PI;
			const float c = cosf(phi);
			const float s = sinf(phi);
			const float radius = TopRadius + (BottomRadius - TopRadius) * v;

			OutPosition = { radius * c, halfHeight - v * Height, radius * s };
			OutNormal = { c, (Height > 0.0f) ? (BottomRadius - TopRadius) / Height : 0.0f, s };
		},
		slices, (Stacks < 1) ? 1 : Stacks);

	if (!bCaps)
	{
		return section;
	}

	// ---- キャップ (扇形) ----
	// 上面: U = +X, V = -Z / 下面: U = +X, V = +Z (B = cross(N, T) が +V になる向き)
	auto AddCap = [&](float Radius, float Y, bool bTop)
		{
			if (Radius <= 0.0f) return;

			FProceduralMeshSection cap;
			const XMFLOAT3 normal{ 0.0f, bTop ? 1.0f : -1.0f, 0.0f };
			const float vSign = bTop ? -1.0f : 1.0f;

			cap.Vertices.push_back(MakeVertex({ 0.0f, Y, 0.0f }, normal, { 0.5f, 0.5f }));
			for (unsigned int i = 0; i <= slices; ++i)
			{
				const float phi = static_cast<float>(i) / static_cast<float>(slices) * PM_TWO_PI;
				const float c = cosf(phi);
				const float s = sinf(phi);
				cap.Vertices.push_back(MakeVertex({ Radius * c, Y, Radius * s }, normal,
					{ 0.5f + 0.5f * c, 0.5f + 0.5f * vSign * s }));
			}
			for (unsigned int i = 0; i < slices; ++i)
			{
				AddTriangleOriented(cap, 0, 1 + i, 2 + i);
			}

			ComputeTangents(cap);
			Append(section, cap);
		};

	AddCap(TopRadius, halfHeight, true);
	AddCap(BottomRadius, -halfHeight, false);

	return section;
}

FProceduralMeshSection FProceduralMeshBuilder::CreateCone(float Radius, float Height, unsigned int Slices, bool bCap)
{
	return CreateCylinder(Radius, 0.0f, Height, Slices, 1, bCap);
}

FProceduralMeshSection FProceduralMeshBuilder::CreateTorus(float MajorRadius, float MinorRadius,
	unsigned int MajorSegments, unsigned int MinorSegments)
{
	// u = リング周方向, v = 管周方向 (dP/dV = cross(N, dP/dU) になるよう負方向に回す)
	return CreateParametricSurface(
		[=](float u, float v, XMFLOAT3& OutPosition, XMFLOAT3& OutNormal)
		{
			const float a = u * PM_TWO_PI;
			const float b = -v * PM_TWO_PI;
			const float ca = cosf(a);
			const float sa = sinf(a);
			const float cb = cosf(b);
			const float sb = sinf(b);

			OutNormal = { ca * cb, sb, sa * cb };
			OutPosition = {
				MajorRadius * ca + MinorRadius * OutNormal.x,
				MinorRadius * OutNormal.y,
				MajorRadius * sa + MinorRadius * OutNormal.z };
		},
		(MajorSegments < 3) ? 3 : MajorSegments, (MinorSegments < 3) ? 3 : MinorSegments);
}

FProceduralMeshSection FProceduralMeshBuilder::CreateCapsule(float Radius, float HalfHeight,
	unsigned int Slices, unsigned int HemisphereStacks)
{
	const unsigned int slices = (Slices < 3) ? 3 : Slices;
	const unsigned int stacks = (HemisphereStacks < 1) ? 1 : HemisphereStacks;

	// V は弧長比で割り当てる (上半球 -> 円柱 -> 下半球)
	const float arcLength = PM_PI * Radius * 0.5f;
	const float totalLength = arcLength * 2.0f + HalfHeight * 2.0f;
	const float vHemisphere = (totalLength > 0.0f) ? arcLength / totalLength : 0.25f;
	const float vCylinder = 1.0f - vHemisphere * 2.0f;

	auto Hemisphere = [=](float YOffset, float ThetaStart) -> FSurfaceFunc
		{
			return [=](float u, float v, XMFLOAT3& OutPosition, XMFLOAT3& OutNormal)
				{
					const float phi = u * PM_TWO_PI;
					const float theta = ThetaStart + v * PM_PI * 0.5f;
					const float sinTheta = sinf(theta);
					OutNormal = { sinTheta * cosf(phi), cosf(theta), sinTheta * sinf(phi) };
					OutPosition = { OutNormal.x * Radius, YOffset + OutNormal.y * Radius, OutNormal.z * Radius };
				};
		};

	// 上半球 (theta 0 -> pi/2)
	FProceduralMeshSection section = CreateParametricSurface(Hemisphere(HalfHeight, 0.0f),
		slices, stacks, { 1.0f, vHemisphere });

	// 円柱部
	if (HalfHeight > 0.0f)
	{
		FProceduralMeshSection side = CreateParametricSurface(
			[=](float u, float v, XMFLOAT3& OutPosition, XMFLOAT3& OutNormal)
			{
				const float phi = u * PM_TWO_PI;
				const float c = cosf(phi);
				const float s = sinf(phi);
				OutNormal = { c, 0.0f, s };
				OutPosition = { Radius * c, HalfHeight - v * HalfHeight * 2.0f, Radius * s };
			},
			slices, 1, { 1.0f, vCylinder });
		for (VERTEX_3D& v : side.Vertices) v.TexCoord.y += vHemisphere;
		Append(section, side);
	}

	// 下半球 (theta pi/2 -> pi)
	FProceduralMeshSection bottom = CreateParametricSurface(Hemisphere(-HalfHeight, PM_PI * 0.5f),
		slices, stacks, { 1.0f, vHemisphere });
	for (VERTEX_3D& v : bottom.Vertices) v.TexCoord.y += vHemisphere + vCylinder;
	Append(section, bottom);

	return section;
}


// ============================================================
//  ノイズベース
// ============================================================

FProceduralMeshSection FProceduralMeshBuilder::CreateTerrain(const FProceduralTerrainParams& Params)
{
	const FProceduralNoise noise(Params.Seed);

	// 位置のみ評価し、法線は後で面法線の平均から求める
	FProceduralMeshSection section = CreateParametricSurface(
		[&](float u, float v, XMFLOAT3& OutPosition, XMFLOAT3& OutNormal)
		{
			const float x = -Params.Width * 0.5f + u * Params.Width;
			const float z = Params.Depth * 0.5f - v * Params.Depth;
			const float h = noise.FBM2D(x * Params.Frequency, z * Params.Frequency,
				Params.Octaves, Params.Lacunarity, Params.Persistence);

			OutPosition = { x, h * Params.HeightScale, z };
			OutNormal = { 0.0f, 1.0f, 0.0f };
		},
		Params.SubdivisionsX, Params.SubdivisionsZ, Params.UVTiling);

	ComputeSmoothNormals(section);
	ComputeTangents(section);

	if (Params.bColorByHeight)
	{
		float minY = 1.0e30f;
		float maxY = -1.0e30f;
		for (const VERTEX_3D& v : section.Vertices)
		{
			minY = fminf(minY, v.Position.y);
			maxY = fmaxf(maxY, v.Position.y);
		}
		const float range = fmaxf(maxY - minY, 1.0e-6f);

		const XMVECTOR low = XMLoadFloat4(&Params.LowColor);
		const XMVECTOR high = XMLoadFloat4(&Params.HighColor);
		for (VERTEX_3D& v : section.Vertices)
		{
			const float t = (v.Position.y - minY) / range;
			XMStoreFloat4(&v.Color, XMVectorLerp(low, high, t));
		}
	}

	return section;
}

FProceduralMeshSection FProceduralMeshBuilder::CreateRock(const FProceduralRockParams& Params)
{
	const FProceduralNoise noise(Params.Seed);

	FProceduralMeshSection section = CreateIcoSphere(1.0f, Params.Subdivisions);

	// 単位球の各頂点を法線方向に fBm で変位し、Stretch で非等方スケール
	for (VERTEX_3D& v : section.Vertices)
	{
		const XMFLOAT3 n = Normalize(v.Position);
		const float d = noise.FBM3D(n.x * Params.Frequency, n.y * Params.Frequency, n.z * Params.Frequency,
			Params.Octaves, Params.Lacunarity, Params.Persistence);
		const float r = Params.Radius * (1.0f + Params.Displacement * d);

		v.Position = { n.x * r * Params.Stretch.x, n.y * r * Params.Stretch.y, n.z * r * Params.Stretch.z };
	}

	if (Params.bFlatShading)
	{
		ComputeFlatNormals(section);
	}
	else
	{
		ComputeSmoothNormals(section);
	}
	ComputeTangents(section);

	return section;
}


// ============================================================
//  後処理
// ============================================================

void FProceduralMeshBuilder::ComputeSmoothNormals(FProceduralMeshSection& Section)
{
	std::vector<XMFLOAT3> accum(Section.Vertices.size(), XMFLOAT3{ 0.0f, 0.0f, 0.0f });

	for (size_t i = 0; i + 2 < Section.Indices.size(); i += 3)
	{
		const unsigned int i0 = Section.Indices[i];
		const unsigned int i1 = Section.Indices[i + 1];
		const unsigned int i2 = Section.Indices[i + 2];

		// 面積加重 (正規化しない外積)
		const XMFLOAT3 n = Store(GeometricNormal(
			Section.Vertices[i0].Position, Section.Vertices[i1].Position, Section.Vertices[i2].Position));

		for (unsigned int idx : { i0, i1, i2 })
		{
			accum[idx].x += n.x;
			accum[idx].y += n.y;
			accum[idx].z += n.z;
		}
	}

	for (size_t i = 0; i < Section.Vertices.size(); ++i)
	{
		Section.Vertices[i].Normal = Normalize(accum[i]);
	}
}

void FProceduralMeshBuilder::ComputeFlatNormals(FProceduralMeshSection& Section)
{
	std::vector<VERTEX_3D> vertices;
	std::vector<unsigned int> indices;
	vertices.reserve(Section.Indices.size());
	indices.reserve(Section.Indices.size());

	for (size_t i = 0; i + 2 < Section.Indices.size(); i += 3)
	{
		VERTEX_3D v0 = Section.Vertices[Section.Indices[i]];
		VERTEX_3D v1 = Section.Vertices[Section.Indices[i + 1]];
		VERTEX_3D v2 = Section.Vertices[Section.Indices[i + 2]];

		const XMFLOAT3 n = Normalize(Store(GeometricNormal(v0.Position, v1.Position, v2.Position)));
		v0.Normal = n;
		v1.Normal = n;
		v2.Normal = n;

		const unsigned int base = static_cast<unsigned int>(vertices.size());
		vertices.push_back(v0);
		vertices.push_back(v1);
		vertices.push_back(v2);
		indices.push_back(base);
		indices.push_back(base + 1);
		indices.push_back(base + 2);
	}

	Section.Vertices.swap(vertices);
	Section.Indices.swap(indices);
}

unsigned int FProceduralMeshBuilder::ComputeTangents(FProceduralMeshSection& Section)
{
	const size_t vertexCount = Section.Vertices.size();
	std::vector<XMFLOAT3> tan1(vertexCount, XMFLOAT3{ 0.0f, 0.0f, 0.0f });
	std::vector<XMFLOAT3> tan2(vertexCount, XMFLOAT3{ 0.0f, 0.0f, 0.0f });

	for (size_t i = 0; i + 2 < Section.Indices.size(); i += 3)
	{
		const unsigned int i0 = Section.Indices[i];
		const unsigned int i1 = Section.Indices[i + 1];
		const unsigned int i2 = Section.Indices[i + 2];

		const VERTEX_3D& v0 = Section.Vertices[i0];
		const VERTEX_3D& v1 = Section.Vertices[i1];
		const VERTEX_3D& v2 = Section.Vertices[i2];

		const XMFLOAT3 e1 = { v1.Position.x - v0.Position.x, v1.Position.y - v0.Position.y, v1.Position.z - v0.Position.z };
		const XMFLOAT3 e2 = { v2.Position.x - v0.Position.x, v2.Position.y - v0.Position.y, v2.Position.z - v0.Position.z };

		const float du1 = v1.TexCoord.x - v0.TexCoord.x;
		const float dv1 = v1.TexCoord.y - v0.TexCoord.y;
		const float du2 = v2.TexCoord.x - v0.TexCoord.x;
		const float dv2 = v2.TexCoord.y - v0.TexCoord.y;

		const float det = du1 * dv2 - du2 * dv1;
		if (fabsf(det) < 1.0e-12f)
		{
			continue;	// UV が縮退 (極など)
		}
		const float r = 1.0f / det;

		// sdir = dP/dU, tdir = dP/dV
		const XMFLOAT3 sdir = {
			(e1.x * dv2 - e2.x * dv1) * r,
			(e1.y * dv2 - e2.y * dv1) * r,
			(e1.z * dv2 - e2.z * dv1) * r };
		const XMFLOAT3 tdir = {
			(e2.x * du1 - e1.x * du2) * r,
			(e2.y * du1 - e1.y * du2) * r,
			(e2.z * du1 - e1.z * du2) * r };

		for (unsigned int idx : { i0, i1, i2 })
		{
			tan1[idx].x += sdir.x; tan1[idx].y += sdir.y; tan1[idx].z += sdir.z;
			tan2[idx].x += tdir.x; tan2[idx].y += tdir.y; tan2[idx].z += tdir.z;
		}
	}

	unsigned int negativeHandedness = 0;

	for (size_t i = 0; i < vertexCount; ++i)
	{
		VERTEX_3D& v = Section.Vertices[i];
		const XMVECTOR n = Load(v.Normal);
		const XMVECTOR t = Load(tan1[i]);

		// Gram-Schmidt で法線と直交化
		XMVECTOR tangent = XMVectorSubtract(t, XMVectorScale(n, XMVectorGetX(XMVector3Dot(n, t))));
		if (XMVectorGetX(XMVector3LengthSq(tangent)) < 1.0e-12f)
		{
			v.Tangent = FallbackTangent(v.Normal);
			continue;
		}
		tangent = XMVector3Normalize(tangent);
		v.Tangent = Store(tangent);

		// ハンドネス: cross(N, T) (シェーダが再構成する従法線) と dP/dV の向き
		if (XMVectorGetX(XMVector3Dot(XMVector3Cross(n, tangent), Load(tan2[i]))) < 0.0f)
		{
			++negativeHandedness;
		}
	}

	return negativeHandedness;
}

void FProceduralMeshBuilder::Transform(FProceduralMeshSection& Section, const XMMATRIX& Matrix)
{
	for (VERTEX_3D& v : Section.Vertices)
	{
		XMStoreFloat3(&v.Position, XMVector3TransformCoord(Load(v.Position), Matrix));
		v.Normal = Normalize(Store(XMVector3TransformNormal(Load(v.Normal), Matrix)));
		v.Tangent = Normalize(Store(XMVector3TransformNormal(Load(v.Tangent), Matrix)), { 1.0f, 0.0f, 0.0f });
	}
}

void FProceduralMeshBuilder::FlipWinding(FProceduralMeshSection& Section)
{
	for (size_t i = 0; i + 2 < Section.Indices.size(); i += 3)
	{
		std::swap(Section.Indices[i + 1], Section.Indices[i + 2]);
	}
}

void FProceduralMeshBuilder::SetVertexColor(FProceduralMeshSection& Section, const XMFLOAT4& Color)
{
	for (VERTEX_3D& v : Section.Vertices)
	{
		v.Color = Color;
	}
}

void FProceduralMeshBuilder::Append(FProceduralMeshSection& Destination, const FProceduralMeshSection& Source)
{
	const unsigned int offset = static_cast<unsigned int>(Destination.Vertices.size());

	Destination.Vertices.insert(Destination.Vertices.end(), Source.Vertices.begin(), Source.Vertices.end());
	Destination.Indices.reserve(Destination.Indices.size() + Source.Indices.size());
	for (unsigned int index : Source.Indices)
	{
		Destination.Indices.push_back(index + offset);
	}
}

unsigned int FProceduralMeshBuilder::CountInvertedTriangles(const FProceduralMeshSection& Section)
{
	unsigned int inverted = 0;

	for (size_t i = 0; i + 2 < Section.Indices.size(); i += 3)
	{
		const VERTEX_3D& v0 = Section.Vertices[Section.Indices[i]];
		const VERTEX_3D& v1 = Section.Vertices[Section.Indices[i + 1]];
		const VERTEX_3D& v2 = Section.Vertices[Section.Indices[i + 2]];

		const XMVECTOR geometric = GeometricNormal(v0.Position, v1.Position, v2.Position);
		if (XMVectorGetX(XMVector3LengthSq(geometric)) < 1.0e-16f)
		{
			continue;
		}

		const XMVECTOR reference = XMVectorAdd(XMVectorAdd(Load(v0.Normal), Load(v1.Normal)), Load(v2.Normal));
		if (XMVectorGetX(XMVector3Dot(geometric, reference)) < 0.0f)
		{
			++inverted;
		}
	}

	return inverted;
}
