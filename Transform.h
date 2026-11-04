#pragma once
#include <DirectXMath.h>
#include <cmath>

using namespace DirectX;

// ============================================================
//  FTransform
//  FTransform (Math/TransformVectorized.h) に相当する
//  平行移動 / 回転 (四元数) / スケールの 3 成分表現。
//  ボーン空間 / コンポーネント空間のポーズ (FCompactPose /
//  ComponentSpaceTransforms) はこの型で受け渡し、行列への変換は
//  ReferenceToLocal の計算 (SendRenderDynamicData_Concurrent) で行う。
//
//  規約:
//    - 行列は行ベクトル規約 (v' = v * M)。ToMatrixWithScale は
//      S * R * T (USceneComponent::GetLocalMatrix と同じ順)
//    - A * B は「A を適用してから B」(UE と同じ)。行列では
//      A.ToMatrix() * B.ToMatrix() に一致する
//      (B のスケールが非一様かつ回転付きの場合は UE と同様に近似)
//    - 四元数は XMQuaternion* の規約 (x, y, z, w)。
//      XMQuaternionMultiply(Q1, Q2) は「Q1 を適用してから Q2」
// ============================================================

struct FTransform
{
	XMFLOAT4 Rotation = { 0.0f, 0.0f, 0.0f, 1.0f };	// 四元数 (x, y, z, w)
	XMFLOAT3 Translation = { 0.0f, 0.0f, 0.0f };
	XMFLOAT3 Scale3D = { 1.0f, 1.0f, 1.0f };

	FTransform() = default;

	FTransform(const XMFLOAT4& InRotation, const XMFLOAT3& InTranslation, const XMFLOAT3& InScale3D = { 1.0f, 1.0f, 1.0f })
		: Rotation(InRotation)
		, Translation(InTranslation)
		, Scale3D(InScale3D)
	{
	}

	explicit FTransform(const XMFLOAT3& InTranslation)
		: Translation(InTranslation)
	{
	}

	static const FTransform& Identity()
	{
		static const FTransform identity;
		return identity;
	}

	// ---- XMVECTOR アクセサ ----
	XMVECTOR GetRotation() const { return XMLoadFloat4(&Rotation); }
	XMVECTOR GetTranslation() const { return XMLoadFloat3(&Translation); }
	XMVECTOR GetScale3D() const { return XMLoadFloat3(&Scale3D); }

	void SetRotation(FXMVECTOR Q) { XMStoreFloat4(&Rotation, Q); }
	void SetTranslation(FXMVECTOR T) { XMStoreFloat3(&Translation, T); }
	void SetScale3D(FXMVECTOR S) { XMStoreFloat3(&Scale3D, S); }

	void SetIdentity()
	{
		Rotation = { 0.0f, 0.0f, 0.0f, 1.0f };
		Translation = { 0.0f, 0.0f, 0.0f };
		Scale3D = { 1.0f, 1.0f, 1.0f };
	}

	// ---- 行列変換 (行ベクトル規約: S * R * T) ----
	XMMATRIX ToMatrixWithScale() const
	{
		return XMMatrixScalingFromVector(GetScale3D())
			* XMMatrixRotationQuaternion(GetRotation())
			* XMMatrixTranslationFromVector(GetTranslation());
	}

	XMMATRIX ToMatrixNoScale() const
	{
		return XMMatrixRotationQuaternion(GetRotation())
			* XMMatrixTranslationFromVector(GetTranslation());
	}

	// 行列からの構築 (XMMatrixDecompose。分解できない行列は単位変換)
	static FTransform FromMatrix(FXMMATRIX M)
	{
		XMVECTOR s, r, t;
		FTransform result;
		if (XMMatrixDecompose(&s, &r, &t, M))
		{
			result.SetScale3D(s);
			result.SetRotation(r);
			result.SetTranslation(t);
		}
		return result;
	}

	// ---- 合成 (A * B = A を適用してから B。FTransform::Multiply) ----
	// Rot   = A.Rot then B.Rot
	// Scale = A.Scale * B.Scale
	// Trans = B.Rot.Rotate(B.Scale * A.Trans) + B.Trans
	FTransform operator*(const FTransform& B) const
	{
		const XMVECTOR qA = GetRotation();
		const XMVECTOR qB = B.GetRotation();

		FTransform result;
		result.SetRotation(XMQuaternionNormalize(XMQuaternionMultiply(qA, qB)));
		result.SetScale3D(XMVectorMultiply(GetScale3D(), B.GetScale3D()));

		const XMVECTOR scaledTranslation = XMVectorMultiply(B.GetScale3D(), GetTranslation());
		const XMVECTOR rotatedTranslation = XMVector3Rotate(scaledTranslation, qB);
		result.SetTranslation(XMVectorAdd(rotatedTranslation, B.GetTranslation()));
		return result;
	}

	// 逆変換 (スケール 0 の軸は 0 のまま)
	FTransform Inverse() const
	{
		const XMVECTOR invRotation = XMQuaternionInverse(GetRotation());
		const XMVECTOR scale = GetScale3D();
		const XMVECTOR invScale = XMVectorSelect(
			XMVectorReciprocal(scale), XMVectorZero(),
			XMVectorNearEqual(scale, XMVectorZero(), XMVectorReplicate(1.0e-8f)));

		FTransform result;
		result.SetRotation(invRotation);
		result.SetScale3D(invScale);
		// T' = -(InvRot.Rotate(T) * InvScale)
		const XMVECTOR t = XMVector3Rotate(GetTranslation(), invRotation);
		result.SetTranslation(XMVectorNegate(XMVectorMultiply(t, invScale)));
		return result;
	}

	// ---- 点 / ベクトルの変換 ----
	XMVECTOR TransformPosition(FXMVECTOR V) const
	{
		return XMVectorAdd(XMVector3Rotate(XMVectorMultiply(V, GetScale3D()), GetRotation()), GetTranslation());
	}

	XMVECTOR TransformVector(FXMVECTOR V) const
	{
		return XMVector3Rotate(XMVectorMultiply(V, GetScale3D()), GetRotation());
	}

	// ---- ブレンド (FTransform::Blend: 平行移動 / スケール線形, 回転は最短経路の nlerp) ----
	static FTransform Blend(const FTransform& A, const FTransform& B, float Alpha)
	{
		if (Alpha <= 0.0f) return A;
		if (Alpha >= 1.0f) return B;

		XMVECTOR qA = A.GetRotation();
		XMVECTOR qB = B.GetRotation();
		// 最短経路 (内積が負なら B を反転)
		if (XMVectorGetX(XMQuaternionDot(qA, qB)) < 0.0f)
		{
			qB = XMVectorNegate(qB);
		}

		FTransform result;
		result.SetRotation(XMQuaternionNormalize(XMVectorLerp(qA, qB, Alpha)));
		result.SetTranslation(XMVectorLerp(A.GetTranslation(), B.GetTranslation(), Alpha));
		result.SetScale3D(XMVectorLerp(A.GetScale3D(), B.GetScale3D(), Alpha));
		return result;
	}

	// 累積ブレンド (FTransform::BlendWith 相当)。this = Blend(this, Other, Alpha)
	void BlendWith(const FTransform& Other, float Alpha)
	{
		*this = Blend(*this, Other, Alpha);
	}

	// 回転の正規化 (累積誤差の除去)
	void NormalizeRotation()
	{
		SetRotation(XMQuaternionNormalize(GetRotation()));
	}

	bool Equals(const FTransform& Other, float Tolerance = 1.0e-4f) const
	{
		const XMVECTOR eps = XMVectorReplicate(Tolerance);
		const bool bRot = XMVector4NearEqual(GetRotation(), Other.GetRotation(), eps)
			|| XMVector4NearEqual(GetRotation(), XMVectorNegate(Other.GetRotation()), eps);
		return bRot
			&& XMVector3NearEqual(GetTranslation(), Other.GetTranslation(), eps)
			&& XMVector3NearEqual(GetScale3D(), Other.GetScale3D(), eps);
	}
};
