#pragma once
#include <DirectXMath.h>

using namespace DirectX;

// ============================================================
//  FBoxSphereBounds
//  FBoxSphereBounds に相当する境界表現。
//  中心 (Origin) + AABB 半幅 (BoxExtent) + 外接球半径 (SphereRadius)
//  を併せ持ち、フラスタムカリングは球で早期棄却した後に
//  ボックスで精密判定する (FConvexVolume::IntersectBounds)。
//
//  データフロー (一方向):
//    USceneComponent::CalcBounds (ローカル境界 -> ワールド変換)
//      -> UPrimitiveComponent::UpdateBounds (m_Bounds 更新)
//      -> SendRenderTransform (FPrimitiveSceneProxy::SetTransform)
//      -> FSceneRenderer::ComputeViewVisibility (フラスタム判定)
// ============================================================

// 「無限」境界に使う半ワールドサイズ (HALF_WORLD_MAX 相当)。
// 2D オーバーレイなどカリング対象外のプリミティブが使う。
constexpr float HALF_WORLD_MAX = 1.0e8f;

struct FBoxSphereBounds
{
	XMFLOAT3 Origin       = { 0.0f, 0.0f, 0.0f };	// ワールド中心
	XMFLOAT3 BoxExtent    = { 0.0f, 0.0f, 0.0f };	// AABB 半幅 (各軸の全長の半分)
	float    SphereRadius = 0.0f;					// 外接球半径

	FBoxSphereBounds() = default;

	FBoxSphereBounds(const XMFLOAT3& InOrigin, const XMFLOAT3& InBoxExtent, float InSphereRadius)
		: Origin(InOrigin)
		, BoxExtent(InBoxExtent)
		, SphereRadius(InSphereRadius)
	{
	}

	// ローカル AABB (Min/Max) から構築 (FBox -> FBoxSphereBounds 変換相当)
	static FBoxSphereBounds FromMinMax(const XMFLOAT3& Min, const XMFLOAT3& Max)
	{
		FBoxSphereBounds bounds;
		bounds.Origin = {
			(Min.x + Max.x) * 0.5f,
			(Min.y + Max.y) * 0.5f,
			(Min.z + Max.z) * 0.5f };
		bounds.BoxExtent = {
			(Max.x - Min.x) * 0.5f,
			(Max.y - Min.y) * 0.5f,
			(Max.z - Min.z) * 0.5f };
		bounds.SphereRadius = sqrtf(
			bounds.BoxExtent.x * bounds.BoxExtent.x +
			bounds.BoxExtent.y * bounds.BoxExtent.y +
			bounds.BoxExtent.z * bounds.BoxExtent.z);
		return bounds;
	}

	// 行列でワールドへ変換した境界を返す (FBoxSphereBounds::TransformBy 相当)。
	// AABB は行列の回転 + スケール成分の絶対値を半幅に掛けて再フィットし、
	// 球半径は「最大軸スケール x 半径」と「新半幅の対角長」の小さい方を採る。
	FBoxSphereBounds TransformBy(const XMMATRIX& M) const
	{
		FBoxSphereBounds result;

		XMVECTOR origin = XMVectorSet(Origin.x, Origin.y, Origin.z, 1.0f);
		XMVECTOR extent = XMVectorSet(BoxExtent.x, BoxExtent.y, BoxExtent.z, 0.0f);

		// 新しい中心
		XMVECTOR newOrigin = XMVector3TransformCoord(origin, M);
		XMStoreFloat3(&result.Origin, newOrigin);

		// 新しい半幅: |M| (回転 + スケールの絶対値) を半幅に掛ける
		// (行ベクトル規約: extent'_j = Σ_i |M[i][j]| * extent_i)
		XMVECTOR newExtent =
			XMVectorAbs(M.r[0]) * XMVectorSplatX(extent) +
			XMVectorAbs(M.r[1]) * XMVectorSplatY(extent) +
			XMVectorAbs(M.r[2]) * XMVectorSplatZ(extent);
		XMStoreFloat3(&result.BoxExtent, newExtent);

		// 新しい球半径 (保守的に小さい方を採用)
		const float scaleXSq = XMVectorGetX(XMVector3LengthSq(M.r[0]));
		const float scaleYSq = XMVectorGetX(XMVector3LengthSq(M.r[1]));
		const float scaleZSq = XMVectorGetX(XMVector3LengthSq(M.r[2]));
		const float maxScale = sqrtf(fmaxf(scaleXSq, fmaxf(scaleYSq, scaleZSq)));
		const float extentLength = XMVectorGetX(XMVector3Length(newExtent));

		result.SphereRadius = fminf(SphereRadius * maxScale, extentLength);
		return result;
	}
};
