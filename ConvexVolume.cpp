#include "Main.h"
#include "ConvexVolume.h"

// ============================================================
//  交差判定
//  平面規約: dot(Normal, Point) + w >= 0 が内側。
//  どちらも「完全に外側の平面が 1 つでもあれば不可視」の
//  保守的判定 (SceneVisibility.cpp の FrustumCull と同じ)。
// ============================================================

bool FConvexVolume::IntersectSphere(const XMFLOAT3& Origin, float Radius) const
{
	for (const XMFLOAT4& plane : Planes)
	{
		const float distance =
			plane.x * Origin.x + plane.y * Origin.y + plane.z * Origin.z + plane.w;

		if (distance < -Radius)
		{
			return false;	// この平面の完全外側 = カリング可能
		}
	}
	return true;
}

bool FConvexVolume::IntersectBox(const XMFLOAT3& Origin, const XMFLOAT3& Extent) const
{
	for (const XMFLOAT4& plane : Planes)
	{
		const float distance =
			plane.x * Origin.x + plane.y * Origin.y + plane.z * Origin.z + plane.w;

		// AABB の平面法線方向への投影半径 (push-out)
		const float pushOut =
			fabsf(plane.x) * Extent.x +
			fabsf(plane.y) * Extent.y +
			fabsf(plane.z) * Extent.z;

		if (distance < -pushOut)
		{
			return false;	// この平面の完全外側 = カリング可能
		}
	}
	return true;
}

// ============================================================
//  フラスタム平面抽出 (Gribb / Hartmann 法)
// ============================================================

// 平面 (a, b, c, d) を正規化して追加する。法線が縮退している場合
// (正射影の無限遠平面など) はスキップする。
static void AddNormalizedPlane(FConvexVolume& Volume, float a, float b, float c, float d)
{
	const float lengthSq = a * a + b * b + c * c;
	if (lengthSq < 1.0e-12f)
	{
		return;	// 縮退平面はカリングに寄与しない
	}

	const float invLength = 1.0f / sqrtf(lengthSq);
	Volume.Planes.push_back({ a * invLength, b * invLength, c * invLength, d * invLength });
}

void GetViewFrustumBounds(
	FConvexVolume& OutVolume,
	const XMMATRIX& ViewProjection,
	bool bUseNearPlane,
	bool bUseFarPlane)
{
	OutVolume.Init();

	XMFLOAT4X4 m;
	XMStoreFloat4x4(&m, ViewProjection);

	// 行ベクトル規約 (v' = v * M) では、クリップ成分は M の「列」との内積:
	//   x' = dot(v, col0), y' = dot(v, col1), z' = dot(v, col2), w' = dot(v, col3)
	// D3D クリップ空間の内側条件:
	//   -w' <= x' <= w',  -w' <= y' <= w',  0 <= z' <= w'
	// 各条件を dot(v, colA +- colB) >= 0 に展開して内向き平面を得る。

	// Left   : x' + w' >= 0  ->  col3 + col0
	AddNormalizedPlane(OutVolume,
		m._11 + m._14, m._21 + m._24, m._31 + m._34, m._41 + m._44);

	// Right  : w' - x' >= 0  ->  col3 - col0
	AddNormalizedPlane(OutVolume,
		m._14 - m._11, m._24 - m._21, m._34 - m._31, m._44 - m._41);

	// Bottom : y' + w' >= 0  ->  col3 + col1
	AddNormalizedPlane(OutVolume,
		m._12 + m._14, m._22 + m._24, m._32 + m._34, m._42 + m._44);

	// Top    : w' - y' >= 0  ->  col3 - col1
	AddNormalizedPlane(OutVolume,
		m._14 - m._12, m._24 - m._22, m._34 - m._32, m._44 - m._42);

	// Near   : z' >= 0       ->  col2
	if (bUseNearPlane)
	{
		AddNormalizedPlane(OutVolume,
			m._13, m._23, m._33, m._43);
	}

	// Far    : w' - z' >= 0  ->  col3 - col2
	if (bUseFarPlane)
	{
		AddNormalizedPlane(OutVolume,
			m._14 - m._13, m._24 - m._23, m._34 - m._33, m._44 - m._43);
	}
}
