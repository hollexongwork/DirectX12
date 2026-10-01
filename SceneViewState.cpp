#include "Main.h"
#include "SceneViewState.h"
#include "Halton.h"

#include <array>
#include <cmath>

// ============================================================
//  SceneViewState : FTAATexture / IsLargeCameraMovement /
//  ComputeClipToPrevClip / ComputeTemporalAASampleCount / ComputeTemporalAASample
// ============================================================

namespace
{
	// ---- ComputeClipToPrevClip 用の double 4x4 (行ベクトル規約, 転置前) ----
	using FMatrix44d = std::array<std::array<double, 4>, 4>;

	FMatrix44d IdentityD()
	{
		FMatrix44d M{};
		for (int i = 0; i < 4; ++i) M[i][i] = 1.0;
		return M;
	}

	// m[i][j] をそのまま double へ
	FMatrix44d ToDouble(const XMFLOAT4X4& M)
	{
		FMatrix44d R{};
		for (int i = 0; i < 4; ++i)
			for (int j = 0; j < 4; ++j)
				R[i][j] = (double)M.m[i][j];
		return R;
	}

	// A * B
	FMatrix44d Mul(const FMatrix44d& A, const FMatrix44d& B)
	{
		FMatrix44d R{};
		for (int i = 0; i < 4; ++i)
			for (int j = 0; j < 4; ++j)
			{
				double s = 0.0;
				for (int k = 0; k < 4; ++k) s += A[i][k] * B[k][j];
				R[i][j] = s;
			}
		return R;
	}

	// ガウス・ジョルダン (部分ピボット), double。特異 (ピボット 0) の場合は単位行列を返す
	FMatrix44d Inverse(const FMatrix44d& M)
	{
		FMatrix44d A = M;
		FMatrix44d Inv = IdentityD();
		for (int col = 0; col < 4; ++col)
		{
			// 部分ピボット: この列で絶対値最大の行を選ぶ
			int pivot = col;
			for (int r = col + 1; r < 4; ++r)
				if (std::fabs(A[r][col]) > std::fabs(A[pivot][col])) pivot = r;
			if (A[pivot][col] == 0.0)
				return IdentityD();
			std::swap(A[col], A[pivot]);
			std::swap(Inv[col], Inv[pivot]);

			const double invPivot = 1.0 / A[col][col];
			for (int j = 0; j < 4; ++j) { A[col][j] *= invPivot; Inv[col][j] *= invPivot; }

			for (int r = 0; r < 4; ++r)
			{
				if (r == col) continue;
				const double f = A[r][col];
				if (f == 0.0) continue;
				for (int j = 0; j < 4; ++j) { A[r][j] -= f * A[col][j]; Inv[r][j] -= f * Inv[col][j]; }
			}
		}
		return Inv;
	}

	// ViewMatrix の上 3x3 (回転) のみ。平行移動行 = 0, _44 = 1
	FMatrix44d RotationPart(const XMFLOAT4X4& View)
	{
		FMatrix44d R{};
		for (int i = 0; i < 3; ++i)
			for (int j = 0; j < 3; ++j)
				R[i][j] = (double)View.m[i][j];
		R[3][3] = 1.0;
		return R;
	}
}

void FTAATexture::Allocate(RenderManager* RHI, XMUINT2 E, DXGI_FORMAT F, const wchar_t* Name)
{
	// 旧テクスチャはデストラクタ経由で遅延解放キューへ (GPU が読み終わるまで生存)。
	// SRV / UAV / RTV は新しいヒープ枠に作られる (シェーダー可視デスクリプタの上書き無し)
	RT.reset();
	RT = RHI->CreateRenderTarget(E.x, E.y, F, 1, true);
	if (RT && RT->Resource && Name)
	{
		RT->Resource->SetName(Name);
	}
	Extent = E;
	Format = F;
	State = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
}


bool IsLargeCameraMovement(const FViewMatrices& Cur, const FViewMatrices& Prev, float RotationThresholdDeg, float TranslationThresholdM)
{
	// UE (4.26 / 5.x):
	//   bIsLargeMovement = FVector::DotProduct(Cur.GetColumn(j), Prev.GetColumn(j)) < cos(RotationThreshold) (j = 0..2)
	//                   || (Cur.ViewOrigin - Prev.ViewOrigin).SizeSquared() > TranslationThreshold^2
	// DirectXMath の行ベクトル規約では ViewMatrix (XMMatrixLookToLH) の上 3x3 の列 0/1/2 が
	// ワールド空間のカメラ右 / 上 / 前方向で、UE の GetColumn(j) と同じ意味になる
	const float c = std::cos(RotationThresholdDeg * (XM_PI / 180.0f));

	for (int j = 0; j < 3; ++j)
	{
		const float dot =
			Cur.ViewMatrix.m[0][j] * Prev.ViewMatrix.m[0][j] +
			Cur.ViewMatrix.m[1][j] * Prev.ViewMatrix.m[1][j] +
			Cur.ViewMatrix.m[2][j] * Prev.ViewMatrix.m[2][j];
		if (dot < c)
		{
			return true;
		}
	}

	const float dx = Cur.ViewOrigin.x - Prev.ViewOrigin.x;
	const float dy = Cur.ViewOrigin.y - Prev.ViewOrigin.y;
	const float dz = Cur.ViewOrigin.z - Prev.ViewOrigin.z;
	return (dx * dx + dy * dy + dz * dz) > TranslationThresholdM * TranslationThresholdM;
}


XMFLOAT4X4 ComputeClipToPrevClip(const FViewMatrices& Cur, const FViewMatrices& Prev)
{
	// UE: ClipToPrevClip = InvTranslatedViewProj(cur) * Translation(PreViewTranslation 差) * TranslatedViewProj(prev)
	// XMMatrixLookToLH の ViewMatrix は Translation(-O) * Rot (行ベクトル) なので、
	// InvVP_NoAA(cur) * VP_NoAA(prev) = InvProj(cur) * InvRot(cur) * Translation(O_cur - O_prev) * Rot(prev) * Proj(prev)。
	// 絶対座標を行列に入れず原点差分のみを使う -> 静止カメラで厳密に単位行列 (float 逆行列の |O| 比例誤差を除去)
	FMatrix44d T = IdentityD();
	T[3][0] = (double)Cur.ViewOrigin.x - (double)Prev.ViewOrigin.x;
	T[3][1] = (double)Cur.ViewOrigin.y - (double)Prev.ViewOrigin.y;
	T[3][2] = (double)Cur.ViewOrigin.z - (double)Prev.ViewOrigin.z;

	const FMatrix44d C2P =
		Mul(Mul(Mul(Mul(Inverse(ToDouble(Cur.ProjectionNoAAMatrix)), Inverse(RotationPart(Cur.ViewMatrix))), T),
			RotationPart(Prev.ViewMatrix)), ToDouble(Prev.ProjectionNoAAMatrix));

	XMFLOAT4X4 Out;
	for (int i = 0; i < 4; ++i)
		for (int j = 0; j < 4; ++j)
			Out.m[i][j] = (float)C2P[i][j];
	return Out;	// 転置前 (b0 / TAA CB へは従来どおり転置してアップロード)
}


int ComputeTemporalAASampleCount(bool bTemporalUpsampling, int SamplesCVar, float ResolutionFraction)
{
	int N = SamplesCVar;
	if (bTemporalUpsampling)
	{
		// 出力画素あたりのサンプル密度を一定に保つ (UE: float を int32 へ代入 = 切り捨て。四捨五入しない)
		const float f = ResolutionFraction;
		N = (int)((float)N * (std::max)(1.0f, 1.0f / (f * f)));
	}
	else if (SamplesCVar == 5)
	{
		N = 4;	// 圧縮プラス 4 サンプル
	}
	return std::clamp(N, 1, 255);
}


XMFLOAT2 ComputeTemporalAASample(bool bTemporalUpsampling, int SamplesCVar, int SequenceLength, int Index, float FilterSize)
{
	if (SequenceLength == 1)
		return { 0.0f, 0.0f };	// [PORT] UE は Gaussian #0 の定数オフセット。0 にして AA Off と厳密比較可能にする

	if (Index < 0)
		Index = 0;

	if (bTemporalUpsampling)
	{
		// 一様分布 (入出力画素の整列が無いため)。パターン分岐より先に判定する (UE 4.26 / 5.x の分岐順)
		return { Halton((uint32_t)Index + 1, 2) - 0.5f, Halton((uint32_t)Index + 1, 3) - 0.5f };
	}

	// UE 4.26: CVarTemporalAASamplesValue で分岐 (添字は % 長さで保護)
	switch (SamplesCVar)
	{
	case 2:
	{
		static const float X[] = { -4.0f / 16.0f, 4.0f / 16.0f };
		static const float Y[] = { -4.0f / 16.0f, 4.0f / 16.0f };
		return { X[Index % 2], Y[Index % 2] };
	}
	case 3:
	{
		// 3xMSAA 風
		static const float X[] = { -2.0f / 3.0f, 2.0f / 3.0f, 0.0f };
		static const float Y[] = { -2.0f / 3.0f, 0.0f, 2.0f / 3.0f };
		return { X[Index % 3], Y[Index % 3] };
	}
	case 4:
	{
		// 回転グリッド 4 サンプル
		static const float X[] = { -2.0f / 16.0f, 6.0f / 16.0f, 2.0f / 16.0f, -6.0f / 16.0f };
		static const float Y[] = { -6.0f / 16.0f, -2.0f / 16.0f, 6.0f / 16.0f, 2.0f / 16.0f };
		return { X[Index % 4], Y[Index % 4] };
	}
	case 5:
	{
		// 圧縮プラス (N = 4)
		static const float X[] = { 0.0f, 1.0f / 2.0f, 0.0f, -1.0f / 2.0f };
		static const float Y[] = { -1.0f / 2.0f, 0.0f, 1.0f / 2.0f, 0.0f };
		return { X[Index % 4], Y[Index % 4] };
	}
	default:
		break;
	}

	// 窓付きガウス (Box-Muller)。sigma = 0.47 * FilterSize, 半径 0.5 で窓掛け
	const float u1 = Halton((uint32_t)Index + 1, 2);
	const float u2 = Halton((uint32_t)Index + 1, 3);
	const float Sigma = 0.47f * (std::max)(FilterSize, 0.01f);
	const float InWindow = std::exp(-0.5f * (0.5f / Sigma) * (0.5f / Sigma));
	const float Theta = 2.0f * XM_PI * u2;
	const float r = Sigma * std::sqrt(-2.0f * std::log((1.0f - u1) * InWindow + u1));
	return { r * std::cos(Theta), r * std::sin(Theta) };
}
