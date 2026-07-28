#pragma once
#include <DirectXMath.h>
#include <vector>
#include "BoxSphereBounds.h"

using namespace DirectX;

// ============================================================
//  FConvexVolume
//  FConvexVolume に相当する凸ボリューム (平面集合)。
//  各平面は XMFLOAT4 (xyz = 内向き法線 [正規化済み], w = オフセット)
//  で保持し、
//    dot(Normal, Point) + w >= 0
//  を満たす点が内側。フラスタムは GetViewFrustumBounds が
//  ViewProjection 行列から Gribb / Hartmann 法で抽出する。
//
//  判定は SceneVisibility.cpp の FrustumCull と同じく
//    1. IntersectSphere : 外接球 vs 各平面の早期棄却
//    2. IntersectBox    : AABB の投影半径 (push-out) 判定
//  の 2 段構え。どちらも保守的 (交差の可能性があれば true) で、
//  フラスタム角の外側をまれに可視と誤判定するが描画結果は不変。
//
//  用途:
//    - FSceneRenderer::ComputeViewVisibility (カメラビュー)
//    - FShadowSceneRenderer::RenderShadowDepthMaps (シャドウビュー別
//      キャスターカリング。CSM / ポイント 6 面 / スポット / レクト)
// ============================================================

class FConvexVolume
{
public:
	std::vector<XMFLOAT4> Planes;

	void Init() { Planes.clear(); }

	// 球が全平面の内側 (交差含む) にあるか。
	// いずれかの平面から半径以上外側なら false (= カリング可能)。
	bool IntersectSphere(const XMFLOAT3& Origin, float Radius) const;

	// AABB (中心 + 半幅) が全平面の内側 (交差含む) にあるか。
	bool IntersectBox(const XMFLOAT3& Origin, const XMFLOAT3& Extent) const;

	// 球 -> ボックスの順で判定 (球で早期棄却)
	bool IntersectBounds(const FBoxSphereBounds& Bounds) const
	{
		return IntersectSphere(Bounds.Origin, Bounds.SphereRadius)
			&& IntersectBox(Bounds.Origin, Bounds.BoxExtent);
	}
};

// ViewProjection 行列 (転置前・行ベクトル規約 v' = v * M) から
// フラスタム 6 平面を抽出する (GetViewFrustumBounds 相当)。
// D3D クリップ空間 (-w <= x,y <= w, 0 <= z <= w) を前提とし、
// 透視 / 正射影のどちらでも動作する。
// bUseNearPlane / bUseFarPlane で近 / 遠平面を除外できる
// (無限遠射影などで縮退した平面は自動でスキップされる)。
void GetViewFrustumBounds(
	FConvexVolume& OutVolume,
	const XMMATRIX& ViewProjection,
	bool bUseNearPlane = true,
	bool bUseFarPlane = true);
