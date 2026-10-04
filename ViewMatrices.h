#pragma once
#include <cassert>
#include <DirectXMath.h>

// ============================================================
//  FViewMatrices
//  FViewMatrices 相当 (ヘッダオンリー)。1 ビュー分の行列一式を
//  ジッタ込み / ジッタ無し (NoAA) の 2 系統で保持する。
//
//  規約 (§0.2): 行列はすべて転置前 (DirectXMath の行ベクトル: clip = v * M)。
//  b0 などシェーダーへ渡すときだけ XMMatrixTranspose して詰める。
//
//  TAA のジッタは射影行列の _31 / _32 (NDC オフセット) に加算する
//  (HackAddTemporalAAProjectionJitter)。深度行 / _11 / _22 は不変なので
//  全深度で画像がちょうど +J_ndc だけずれる (Appendix A.2)。
//  NoAA 系はカリング / ClipToPrevClip / Volumetric Fog の履歴に使う。
// ============================================================

inline constexpr DirectX::XMFLOAT4X4 kIdentity4x4{ 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };

struct FViewMatrices                               // すべて転置前 (row-vector)。未初期化読みを防ぐため全行列を単位行列で初期化
{
	DirectX::XMFLOAT4X4 ViewMatrix = kIdentity4x4;
	DirectX::XMFLOAT4X4 ProjectionNoAAMatrix = kIdentity4x4;         // ジッタ無し
	DirectX::XMFLOAT4X4 ProjectionMatrix = kIdentity4x4;             // = NoAA + (_31,_32) += TemporalAAProjectionJitter
	DirectX::XMFLOAT4X4 ViewProjectionMatrix = kIdentity4x4;         // View * Projection (ジッタ込み)
	DirectX::XMFLOAT4X4 ViewProjectionNoAAMatrix = kIdentity4x4;
	DirectX::XMFLOAT4X4 InvViewProjectionMatrix = kIdentity4x4;      // ジッタ込み
	DirectX::XMFLOAT4X4 InvViewProjectionNoAAMatrix = kIdentity4x4;
	DirectX::XMFLOAT3   ViewOrigin{};
	DirectX::XMFLOAT2   TemporalAAProjectionJitter{ 0.0f, 0.0f };   // NDC (GetTemporalAAJitter)

	// ジッタ 0 で全派生行列を計算する
	void Init(const DirectX::XMFLOAT4X4& View, const DirectX::XMFLOAT4X4& ProjNoAA, const DirectX::XMFLOAT3& Origin)
	{
		ViewMatrix = View;
		ProjectionNoAAMatrix = ProjNoAA;
		ProjectionMatrix = ProjNoAA;
		ViewOrigin = Origin;
		TemporalAAProjectionJitter = { 0.0f, 0.0f };
		RecomputeDerivedMatrices();
	}

	// HackAddTemporalAAProjectionJitter: 射影の _31 / _32 に NDC ジッタを足して派生行列を再計算
	void HackAddTemporalAAProjectionJitter(DirectX::XMFLOAT2 J)
	{
		assert(TemporalAAProjectionJitter.x == 0.0f && TemporalAAProjectionJitter.y == 0.0f && "jitter already applied");
		ProjectionMatrix._31 += J.x;
		ProjectionMatrix._32 += J.y;
		TemporalAAProjectionJitter = J;
		RecomputeDerivedMatrices();
	}

	// HackRemoveTemporalAAProjectionJitter: ジッタを取り除いて再計算。
	// 浮動小数の (a + j) - j は a に戻らないことがあるので、ジッタ無しの
	// 射影行列の _31 / _32 を書き戻す (= ジッタの厳密な減算)
	void HackRemoveTemporalAAProjectionJitter()
	{
		ProjectionMatrix._31 = ProjectionNoAAMatrix._31;
		ProjectionMatrix._32 = ProjectionNoAAMatrix._32;
		TemporalAAProjectionJitter = { 0.0f, 0.0f };
		RecomputeDerivedMatrices();
	}

	// VP / InvVP (ジッタ込み + NoAA の両系統)。
	// 旧 RenderBasePass と同じ呼び出し順 (XMMatrixMultiply(View, Proj) -> XMMatrixInverse) なので、
	// ジッタ 0 では b0 の行列が基準とビット一致する
	void RecomputeDerivedMatrices()
	{
		using namespace DirectX;
		const XMMATRIX view = XMLoadFloat4x4(&ViewMatrix);
		const XMMATRIX viewProjection = XMMatrixMultiply(view, XMLoadFloat4x4(&ProjectionMatrix));
		const XMMATRIX viewProjectionNoAA = XMMatrixMultiply(view, XMLoadFloat4x4(&ProjectionNoAAMatrix));
		XMStoreFloat4x4(&ViewProjectionMatrix, viewProjection);
		XMStoreFloat4x4(&ViewProjectionNoAAMatrix, viewProjectionNoAA);
		XMStoreFloat4x4(&InvViewProjectionMatrix, XMMatrixInverse(nullptr, viewProjection));
		XMStoreFloat4x4(&InvViewProjectionNoAAMatrix, XMMatrixInverse(nullptr, viewProjectionNoAA));
	}

	DirectX::XMFLOAT2 GetTemporalAAJitter() const { return TemporalAAProjectionJitter; }
};
