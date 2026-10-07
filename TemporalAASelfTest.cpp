#include "Main.h"
#include "TemporalAASelfTest.h"

#include "SceneRenderer.h"
#include "ScreenPercentage.h"
#include "SceneViewState.h"
#include "ViewMatrices.h"
#include "Halton.h"
#include "SceneVelocityData.h"
#include "VelocityRendering.h"
#include "TemporalAA.h"

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <string>

// ============================================================
//  TemporalAASelfTest : CPU テストベクタ (Appendix A)
//  ジッタ / ビューファミリ : T1 / T2 / T3 / T12 (N = ジッタサンプル数を含む)
//  再投影                  : T4 / T4b / T5 / T13 / T15
//  ベロシティ              : T6 (エンコード) / T16 (FSceneVelocityData のライフサイクル)
//  Main カーネル           : T7 (Main のサンプル重み) / T14 (YCoCg / HDR 重みの CPU 鏡像)
//  TAAU カーネル           : T8 (TAAU 空間重み曲線) / T9 (TAAU の入力写像 K / dKO / FTW) /
//                            T10 (Catmull-Rom 1D 重み) / T11 (Mitchell-Netravali のカーネルと正規化タップ)
//  GPU パリティ            : TemporalAASelfTest_CS
// ============================================================

namespace
{
	// ------------------------------------------------------------
	//  レポート: テスト (ID) 単位で PASS / FAIL を 1 行、失敗した検査は個別に 1 行ずつ
	// ------------------------------------------------------------
	class FSelfTestReport
	{
	public:
		explicit FSelfTestReport(FSceneRenderer& Renderer) : m_Renderer(Renderer) {}

		void Line(const std::string& Text)
		{
			const std::string line = "[TAA SelfTest] " + Text;
			OutputDebugStringA((line + "\n").c_str());
			m_Renderer.AddSelfTestLogLine(line);
		}

		void BeginTest(const char* Id)
		{
			m_Id = Id;
			m_NumChecks = 0;
			m_NumFailedChecks = 0;
			m_WorstRatio = -1.0;
			m_WorstText.clear();
		}

		// |Got - Exp| <= Tol (両方有限) なら合格。Tol = 0 は厳密一致
		bool Check(const std::string& What, double Got, double Exp, double Tol)
		{
			++m_NumChecks;
			const double err = std::fabs(Got - Exp);
			const bool bFinite = std::isfinite(Got) && std::isfinite(Exp);
			const bool bPass = bFinite && err <= Tol;

			char text[320];
			sprintf_s(text, "got=%.9g exp=%.9g (%s, |err|=%.3g tol=%.3g)", Got, Exp, What.c_str(), err, Tol);

			// 合格行には最も許容誤差に近い検査を載せる
			const double ratio = !bFinite ? 1e30 : (Tol > 0.0 ? err / Tol : (err > 0.0 ? 1e30 : 0.0));
			if (ratio > m_WorstRatio)
			{
				m_WorstRatio = ratio;
				m_WorstText = text;
			}

			if (!bPass)
			{
				++m_NumFailedChecks;
				Line(std::string(m_Id) + " FAIL " + text);
			}
			return bPass;
		}

		bool CheckBool(const std::string& What, bool Got, bool Exp)
		{
			return Check(What, Got ? 1.0 : 0.0, Exp ? 1.0 : 0.0, 0.0);
		}

		void EndTest()
		{
			++m_NumTests;
			char summary[64];
			sprintf_s(summary, " [%d checks]", m_NumChecks);
			if (m_NumFailedChecks == 0 && m_NumChecks > 0)
			{
				Line(std::string(m_Id) + " PASS " + m_WorstText + summary);
			}
			else
			{
				++m_NumFailedTests;
				char failed[64];
				sprintf_s(failed, " [%d/%d checks failed]", m_NumFailedChecks, m_NumChecks);
				Line(std::string(m_Id) + " FAIL" + failed);
			}
		}

		int GetNumTests() const { return m_NumTests; }
		int GetNumFailedTests() const { return m_NumFailedTests; }

	private:
		FSceneRenderer& m_Renderer;
		const char* m_Id = "";
		int m_NumChecks = 0;
		int m_NumFailedChecks = 0;
		double m_WorstRatio = -1.0;
		std::string m_WorstText;
		int m_NumTests = 0;
		int m_NumFailedTests = 0;
	};

	std::string Fmt(const char* Format, ...)
	{
		char buffer[256];
		va_list args;
		va_start(args, Format);
		vsprintf_s(buffer, Format, args);
		va_end(args);
		return buffer;
	}

	// ---- テスト共通の射影 (Appendix A: FOV 45 度, 16:9, n = 0.1, f = 500) ----
	constexpr float kNear = 0.1f;
	constexpr float kFar = 500.0f;
	constexpr unsigned kOutW = 1920;
	constexpr unsigned kOutH = 1080;

	XMFLOAT4X4 MakeTestProjection()
	{
		XMFLOAT4X4 p;
		XMStoreFloat4x4(&p, XMMatrixPerspectiveFovLH(XMConvertToRadians(45.0f), 16.0f / 9.0f, kNear, kFar));
		return p;
	}

	XMFLOAT4X4 MakeLookTo(const XMFLOAT3& Position, const XMFLOAT3& Forward)
	{
		XMFLOAT4X4 v;
		XMStoreFloat4x4(&v, XMMatrixLookToLH(XMLoadFloat3(&Position), XMVector3Normalize(XMLoadFloat3(&Forward)), XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f)));
		return v;
	}

	// 行ベクトル (x, y, z, 1) * M -> ビューポート画素 (+y 下)。除算以降は double
	void ProjectToPixel(const XMFLOAT4X4& ViewProjection, const XMFLOAT3& Point, unsigned W, unsigned H, double& OutX, double& OutY)
	{
		XMFLOAT4 clip;
		XMStoreFloat4(&clip, XMVector4Transform(XMVectorSet(Point.x, Point.y, Point.z, 1.0f), XMLoadFloat4x4(&ViewProjection)));
		const double ndcX = (double)clip.x / (double)clip.w;
		const double ndcY = (double)clip.y / (double)clip.w;
		OutX = (0.5 * ndcX + 0.5) * (double)W;
		OutY = (0.5 - 0.5 * ndcY) * (double)H;
	}

	// |InvVP * VP - I| の最大要素
	double MaxInverseError(const XMFLOAT4X4& ViewProjection, const XMFLOAT4X4& InvViewProjection)
	{
		XMFLOAT4X4 product;
		XMStoreFloat4x4(&product, XMMatrixMultiply(XMLoadFloat4x4(&InvViewProjection), XMLoadFloat4x4(&ViewProjection)));
		double worst = 0.0;
		for (int i = 0; i < 4; ++i)
			for (int j = 0; j < 4; ++j)
				worst = (std::max)(worst, std::fabs((double)product.m[i][j] - (i == j ? 1.0 : 0.0)));
		return worst;
	}

	// ---- LinearDepth <-> デバイス Z (CPU 参照。§0.2 / Appendix A.5) ----
	// LinearDepthPS: z = n f / (f - d (f - n))
	double DeviceZToViewZ(double DeviceZ, double Near, double Far)
	{
		return Near * Far / (Far - DeviceZ * (Far - Near));
	}
	// 逆変換: d = Q - Q n / z (Q = f / (f - n))。遠方画素 (z >= 0.999 f) は無限遠 d = Q (回転のみの再投影)
	double ViewZToDeviceZ(double ViewZ, double Near, double Far)
	{
		const double Q = Far / (Far - Near);
		if (ViewZ >= 0.999 * Far)
			return Q;
		return Q - Q * Near / ViewZ;
	}

	// ClipToPrevClip による再投影 (A.5): PrevClip = (s.x, s.y, d, 1) * C2P (行ベクトル, 転置前)。
	// 前フレームの ScreenPos (= NDC.xy) を double で返す
	void ReprojectScreenPos(const XMFLOAT4X4& C2P, double SX, double SY, double DeviceZ, double& OutX, double& OutY)
	{
		const double v[4] = { SX, SY, DeviceZ, 1.0 };
		double o[4] = { 0.0, 0.0, 0.0, 0.0 };
		for (int j = 0; j < 4; ++j)
			for (int k = 0; k < 4; ++k)
				o[j] += v[k] * (double)C2P.m[k][j];
		OutX = o[0] / o[3];
		OutY = o[1] / o[3];
	}

	// ワールド Y 軸回りのヨー [度] (ピッチ 0) の前方ベクトル: (sin yaw, 0, cos yaw)
	XMFLOAT3 YawForward(float YawDeg)
	{
		const float yaw = XMConvertToRadians(YawDeg);
		return { std::sin(yaw), 0.0f, std::cos(yaw) };
	}

	FViewMatrices MakeViewMatrices(const XMFLOAT3& Position, const XMFLOAT3& Forward, const XMFLOAT4X4& Projection)
	{
		FViewMatrices vm;
		vm.Init(MakeLookTo(Position, Forward), Projection, Position);
		return vm;
	}


	// ============================================================
	//  T1: Halton(i, 2 / 3), i = 1..8 (A.3)
	// ============================================================
	void TestT1(FSelfTestReport& R)
	{
		static const double kBase2[8] = { 0.5, 0.25, 0.75, 0.125, 0.625, 0.375, 0.875, 0.0625 };
		static const double kBase3[8] = { 1.0 / 3.0, 2.0 / 3.0, 1.0 / 9.0, 4.0 / 9.0, 7.0 / 9.0, 2.0 / 9.0, 5.0 / 9.0, 8.0 / 9.0 };

		R.BeginTest("T1");
		for (uint32_t i = 1; i <= 8; ++i)
		{
			R.Check(Fmt("Halton(%u,2)", i), Halton(i, 2), kBase2[i - 1], 1e-6);
			R.Check(Fmt("Halton(%u,3)", i), Halton(i, 3), kBase3[i - 1], 1e-6);
		}
		R.EndTest();
	}


	// ============================================================
	//  T2: ジッタのサンプル列 (A.3)
	//   一様 (TAAU) / 窓付きガウス (FilterSize 1) の Index 0..7、CVar 2/3/4/5 の固定パターンを
	//   Index = 7 (% 長さ) で、CVar 5 -> N = 4 (TAAU 以外)、N = 1 -> (0, 0)、
	//   TAAU では固定パターンより一様分布が優先 (分岐順)
	// ============================================================
	void TestT2(FSelfTestReport& R)
	{
		R.BeginTest("T2");

		// ---- 一様 (TemporalUpscale): Halton(Index + 1, 2 / 3) - 0.5。Index 0..15 (8..15 は追加検査) ----
		{
			static const double kUniform[16][2] =
			{
				{  0.0,     -1.0 / 6.0  }, { -0.25,    1.0 / 6.0  }, {  0.25,   -7.0 / 18.0 }, { -0.375,  -1.0 / 18.0 },
				{  0.125,    5.0 / 18.0 }, { -0.125,  -5.0 / 18.0 }, {  0.375,   1.0 / 18.0 }, { -0.4375,  7.0 / 18.0 },
				{  0.0625, -25.0 / 54.0 }, { -0.1875, -7.0 / 54.0 }, {  0.3125, 11.0 / 54.0 }, { -0.3125, -19.0 / 54.0 },
				{  0.1875,  -1.0 / 54.0 }, { -0.0625, 17.0 / 54.0 }, {  0.4375, -13.0 / 54.0 }, { -0.46875,  5.0 / 54.0 },
			};
			const int N = ComputeTemporalAASampleCount(true, 8, 1.0f);
			R.Check("uniform N (CVar 8, f = 1)", N, 8, 0.0);
			for (int i = 0; i < 16; ++i)
			{
				// Index 8..15 は N = 32 (SP 50 %) の列の続き
				const XMFLOAT2 s = ComputeTemporalAASample(true, 8, (i < 8) ? 8 : 32, i, 1.0f);
				R.Check(Fmt("uniform[%d].x", i), s.x, kUniform[i][0], 1e-4);
				R.Check(Fmt("uniform[%d].y", i), s.y, kUniform[i][1], 1e-4);
			}
		}

		// ---- 窓付きガウス (FilterSize 1, sigma 0.47, 窓 0.5678676) ----
		{
			static const double kGaussian[8][2] =
			{
				{ -0.1640,  0.2840 }, { -0.2080, -0.3603 }, {  0.1722,  0.1445 }, { -0.4305,  0.1567 },
				{  0.0485, -0.2752 }, {  0.0648,  0.3673 }, { -0.1472, -0.0536 }, {  0.3670, -0.3079 },
			};
			const int N = ComputeTemporalAASampleCount(false, 8, 1.0f);
			R.Check("gaussian N (CVar 8)", N, 8, 0.0);
			for (int i = 0; i < 8; ++i)
			{
				const XMFLOAT2 s = ComputeTemporalAASample(false, 8, N, i, 1.0f);
				R.Check(Fmt("gaussian[%d].x", i), s.x, kGaussian[i][0], 1e-4);
				R.Check(Fmt("gaussian[%d].y", i), s.y, kGaussian[i][1], 1e-4);
				// 半径 0.5 の窓の内側 (|s| <= 0.5 は窓の定義から常に成り立つ)
				R.CheckBool(Fmt("gaussian[%d] |s| <= 0.5", i), std::sqrt((double)s.x * s.x + (double)s.y * s.y) <= 0.5 + 1e-6, true);
			}
		}

		// ---- 固定パターン (TAAU 以外, CVar で分岐) を Index = 7 で (% 長さの保護) ----
		{
			struct FPattern { int CVar; int ExpectedN; double X, Y; };
			static const FPattern kPatterns[] =
			{
				{ 2, 2,  4.0 / 16.0, 4.0 / 16.0 },
				{ 3, 3,  2.0 / 3.0,  0.0        },
				{ 4, 4, -6.0 / 16.0, 2.0 / 16.0 },
				{ 5, 4, -1.0 / 2.0,  0.0        },	// 圧縮プラス: N = 4
			};
			for (const FPattern& pat : kPatterns)
			{
				const int N = ComputeTemporalAASampleCount(false, pat.CVar, 1.0f);
				R.Check(Fmt("CVar %d N", pat.CVar), N, pat.ExpectedN, 0.0);
				const XMFLOAT2 s = ComputeTemporalAASample(false, pat.CVar, N, 7, 1.0f);
				R.Check(Fmt("CVar %d [7].x", pat.CVar), s.x, pat.X, 1e-4);
				R.Check(Fmt("CVar %d [7].y", pat.CVar), s.y, pat.Y, 1e-4);
			}
		}

		// ---- N の規則 (§4.4): TAAU は int(CVar * max(1, 1/f^2)) (切り捨て)、CVar 5 は TAAU では 5 のまま ----
		R.Check("TAAU CVar 5 N (f = 1)", ComputeTemporalAASampleCount(true, 5, 1.0f), 5, 0.0);
		R.Check("TAAU CVar 8 N (f = 0.5)", ComputeTemporalAASampleCount(true, 8, 0.5f), 32, 0.0);
		R.Check("TAAU CVar 8 N (f = 1287/1920)", ComputeTemporalAASampleCount(true, 8, 1287.0f / 1920.0f), 17, 0.0);
		R.Check("TAAU CVar 64 N (f = 0.5) clamp 255", ComputeTemporalAASampleCount(true, 64, 0.5f), 255, 0.0);
		R.Check("CVar 1 N", ComputeTemporalAASampleCount(false, 1, 1.0f), 1, 0.0);

		// ---- TAAU ではパターン分岐より一様分布が先 (CVar 4, Index 1 -> 一様 #1) ----
		{
			const XMFLOAT2 s = ComputeTemporalAASample(true, 4, 4, 1, 1.0f);
			R.Check("TAAU CVar 4 [1].x (uniform)", s.x, -0.25, 1e-4);
			R.Check("TAAU CVar 4 [1].y (uniform)", s.y, 1.0 / 6.0, 1e-4);
		}

		// ---- N = 1 -> (0, 0) (一様 / ガウス / パターンのいずれでも) ----
		{
			const XMFLOAT2 a = ComputeTemporalAASample(false, 1, 1, 0, 1.0f);
			const XMFLOAT2 b = ComputeTemporalAASample(true, 1, 1, 0, 1.0f);
			R.Check("N = 1 gaussian x", a.x, 0.0, 0.0);
			R.Check("N = 1 gaussian y", a.y, 0.0, 0.0);
			R.Check("N = 1 uniform x", b.x, 0.0, 0.0);
			R.Check("N = 1 uniform y", b.y, 0.0, 0.0);
		}

		R.EndTest();
	}


	// ============================================================
	//  T3: HackAddTemporalAAProjectionJitter (A.2)
	//   点 (0.5, 0.3, 7) (ビュー空間) が R = 1920x1080 でちょうど (0, -1/6) px 動く。
	//   RecomputeDerivedMatrices 後に InvVP * VP = I。HackRemove で厳密に戻る
	// ============================================================
	void TestT3(FSelfTestReport& R)
	{
		R.BeginTest("T3");

		const XMFLOAT4X4 proj = MakeTestProjection();
		const XMFLOAT3 point = { 0.5f, 0.3f, 7.0f };

		// ---- (a) ビュー = 単位行列: 画素位置とジッタによる移動量 ----
		{
			FViewMatrices vm;
			vm.Init(kIdentity4x4, proj, XMFLOAT3{ 0.0f, 0.0f, 0.0f });

			double ux, uy;
			ProjectToPixel(vm.ViewProjectionNoAAMatrix, point, kOutW, kOutH, ux, uy);

			// J_px = (0, -1/6) (一様サンプル #0) -> J_ndc = (2 sx / R.x, -2 sy / R.y)
			const float sx = 0.0f, sy = -1.0f / 6.0f;
			const XMFLOAT2 jndc = { sx * 2.0f / (float)kOutW, sy * -2.0f / (float)kOutH };
			vm.HackAddTemporalAAProjectionJitter(jndc);

			double jx, jy;
			ProjectToPixel(vm.ViewProjectionMatrix, point, kOutW, kOutH, jx, jy);

			R.Check("unjittered px.x", ux, 1053.119666, 1e-3);
			R.Check("unjittered px.y", uy, 484.128200, 1e-3);
			R.Check("jittered px.y", jy, 483.961534, 1e-3);
			R.Check("delta px.x", jx - ux, 0.0, 1e-4);
			R.Check("delta px.y", jy - uy, -1.0 / 6.0, 1e-4);
			R.Check("J_ndc.y", jndc.y, 0.000308642, 1e-9);
			R.Check("stored jitter.y", vm.GetTemporalAAJitter().y, jndc.y, 0.0);
			R.Check("NoAA _32 untouched", vm.ProjectionNoAAMatrix._32, proj._32, 0.0);

			// RecomputeDerivedMatrices 後に InvVP * VP = I (ジッタ込み / NoAA の両系統)
			R.Check("max|InvVP*VP - I| (jittered)", MaxInverseError(vm.ViewProjectionMatrix, vm.InvViewProjectionMatrix), 0.0, 1e-5);
			R.Check("max|InvVP*VP - I| (NoAA)", MaxInverseError(vm.ViewProjectionNoAAMatrix, vm.InvViewProjectionNoAAMatrix), 0.0, 1e-5);

			// NoAA 系の VP はジッタの影響を受けない
			double nx, ny;
			ProjectToPixel(vm.ViewProjectionNoAAMatrix, point, kOutW, kOutH, nx, ny);
			R.Check("NoAA px.y after jitter", ny, uy, 0.0);

			// ジッタ除去で射影行列が NoAA とビット一致に戻る
			vm.HackRemoveTemporalAAProjectionJitter();
			int mismatches = 0;
			for (int i = 0; i < 4; ++i)
				for (int j = 0; j < 4; ++j)
					mismatches += (vm.ProjectionMatrix.m[i][j] != vm.ProjectionNoAAMatrix.m[i][j]) ? 1 : 0;
			R.Check("HackRemove: Projection != NoAA entries", mismatches, 0.0, 0.0);
			R.Check("HackRemove: jitter.y", vm.GetTemporalAAJitter().y, 0.0, 0.0);
		}

		// ---- (b) 追加検査: 実際のカメラ姿勢 (既定位置 (0, 7, -15), ピッチ 20 度) でも InvVP * VP = I ----
		// ワールド絶対座標の VP を float で逆行列にするので、誤差は |ViewOrigin| (約 16.6 m) に比例して
		// 増える (実測 3e-5 程度)。仕様 (1e-5) は (a) のビュー空間で検査し、ここは float 精度相応の 1e-4 とする
		{
			const float pitch = XMConvertToRadians(20.0f);
			const XMFLOAT3 forward = { 0.0f, -std::sin(pitch), std::cos(pitch) };
			FViewMatrices vm;
			vm.Init(MakeLookTo({ 0.0f, 7.0f, -15.0f }, forward), proj, XMFLOAT3{ 0.0f, 7.0f, -15.0f });
			vm.HackAddTemporalAAProjectionJitter({ 0.25f * 2.0f / (float)kOutW, (-1.0f / 6.0f) * -2.0f / (float)kOutH });

			R.Check("camera pose max|InvVP*VP - I| (jittered)", MaxInverseError(vm.ViewProjectionMatrix, vm.InvViewProjectionMatrix), 0.0, 1e-4);
			R.Check("camera pose max|InvVP*VP - I| (NoAA)", MaxInverseError(vm.ViewProjectionNoAAMatrix, vm.InvViewProjectionNoAAMatrix), 0.0, 1e-4);
		}

		R.EndTest();
	}


	// ============================================================
	//  T4: ComputeClipToPrevClip, 原点で +0.1 m (x) の平行移動 (A.5)
	//   C2P = | 1 0 0 0 | 0 1 0 0 | -1.35772353 0 1 0 | 1.35799513 0 0 1 |
	//   点 (0, 0, 10): 今フレーム (s, d) = (-0.01357995, 0, 0.99019804) -> 前フレーム (0, 0)
	//   遠方画素 (0.3, 0.2) を d = Q (無限遠) で -> BackN = s_cur - s_prev = 0 (回転のみの再投影)
	// ============================================================
	void TestT4(FSelfTestReport& R)
	{
		R.BeginTest("T4");

		const XMFLOAT4X4 proj = MakeTestProjection();
		const XMFLOAT3 forward = { 0.0f, 0.0f, 1.0f };
		const FViewMatrices prev = MakeViewMatrices({ 0.0f, 0.0f, 0.0f }, forward, proj);
		const FViewMatrices cur = MakeViewMatrices({ 0.1f, 0.0f, 0.0f }, forward, proj);
		const XMFLOAT4X4 c2p = ComputeClipToPrevClip(cur, prev);

		static const double kExpected[4][4] =
		{
			{  1.0,        0.0, 0.0, 0.0 },
			{  0.0,        1.0, 0.0, 0.0 },
			{ -1.35772353, 0.0, 1.0, 0.0 },
			{  1.35799513, 0.0, 0.0, 1.0 },
		};
		for (int i = 0; i < 4; ++i)
			for (int j = 0; j < 4; ++j)
				R.Check(Fmt("C2P(%d,%d)", i, j), c2p.m[i][j], kExpected[i][j], 1e-5);

		// 点 (0, 0, 10) は前フレームの画面中央 (BackN = -0.01357995 = -13.0368 px @1920)
		const double d10 = ViewZToDeviceZ(10.0, kNear, kFar);
		R.Check("d(z=10)", d10, 0.99019804, 1e-7);
		double px, py;
		ReprojectScreenPos(c2p, -0.01357995, 0.0, d10, px, py);
		R.Check("(0,0,10) prev s.x", px, 0.0, 1e-5);
		R.Check("(0,0,10) prev s.y", py, 0.0, 1e-5);

		// 遠方画素: d = Q (無限遠) は平行移動の影響を受けない -> BackN = 0
		const double Q = (double)kFar / ((double)kFar - (double)kNear);
		ReprojectScreenPos(c2p, 0.3, 0.2, Q, px, py);
		R.Check("far (0.3,0.2,Q) BackN.x", 0.3 - px, 0.0, 1e-5);
		R.Check("far (0.3,0.2,Q) BackN.y", 0.2 - py, 0.0, 1e-5);

		// 参考 (A.5): 遠平面 d = 1 (500 m) では 0.1 m 移動で BackN.x = -0.00027160 (-0.2607 px)
		ReprojectScreenPos(c2p, 0.3, 0.2, 1.0, px, py);
		R.Check("far plane (0.3,0.2,1) BackN.x", 0.3 - px, -0.00027160, 1e-6);

		// 静止 (Prev = Cur) は厳密に単位行列 (カット / 大移動で Prev = Cur とした場合も同じ)
		const XMFLOAT4X4 same = ComputeClipToPrevClip(cur, cur);
		double worst = 0.0;
		for (int i = 0; i < 4; ++i)
			for (int j = 0; j < 4; ++j)
				worst = (std::max)(worst, std::fabs((double)same.m[i][j] - (i == j ? 1.0 : 0.0)));
		R.Check("static max|C2P - I|", worst, 0.0, 1e-6);

		R.EndTest();
	}


	// ============================================================
	//  T4b: ComputeClipToPrevClip を原点から離れた位置で (A.5, calc/c2p_relative.py)
	//   ViewMatrix = XMMatrixLookToLH (float), 前方 = (sin 17, 0, cos 17)
	//   (a) 静止カメラ (0,7,-15) / (150,7,-15) / (1000,7,-15): 画素 (0.3, 0.2, d(z=10)) で |BackN| < 1e-6
	//   (b) 前フレーム (150,7,-15)、今フレームはカメラ右方向へ 0.1 m:
	//       (-0.01357995, 0, 0.99019804) -> (0, 0) (1e-5)、C2P(2,0) / (3,0) が A.5 の値の 1e-4 以内
	//       (許容誤差は 150 m 付近の位置の float 丸め分)
	// ============================================================
	void TestT4b(FSelfTestReport& R)
	{
		R.BeginTest("T4b");

		const XMFLOAT4X4 proj = MakeTestProjection();
		const XMFLOAT3 forward = YawForward(17.0f);
		const double d10 = ViewZToDeviceZ(10.0, kNear, kFar);

		// ---- (a) 静止カメラ ----
		static const XMFLOAT3 kPositions[] = { { 0.0f, 7.0f, -15.0f }, { 150.0f, 7.0f, -15.0f }, { 1000.0f, 7.0f, -15.0f } };
		for (const XMFLOAT3& pos : kPositions)
		{
			const FViewMatrices vm = MakeViewMatrices(pos, forward, proj);
			const XMFLOAT4X4 c2p = ComputeClipToPrevClip(vm, vm);
			double px, py;
			ReprojectScreenPos(c2p, 0.3, 0.2, d10, px, py);
			const std::string at = Fmt("static (%g,%g,%g)", pos.x, pos.y, pos.z);
			R.Check(at + " BackN.x", 0.3 - px, 0.0, 1e-6);
			R.Check(at + " BackN.y", 0.2 - py, 0.0, 1e-6);

			// 参考: 旧方式 (ワールド絶対座標の InvVP_NoAA * VP_NoAA を float で) の残差 [px @1920]。検査はしない
			XMFLOAT4X4 old;
			XMStoreFloat4x4(&old, XMMatrixMultiply(XMLoadFloat4x4(&vm.InvViewProjectionNoAAMatrix), XMLoadFloat4x4(&vm.ViewProjectionNoAAMatrix)));
			double ox, oy;
			ReprojectScreenPos(old, 0.3, 0.2, d10, ox, oy);
			R.Line(Fmt("T4b info: %s old world-space float product |BackN| = (%.4f, %.4f) px, camera-relative = (%.2g, %.2g) px",
				at.c_str(), std::fabs(0.3 - ox) * kOutW * 0.5, std::fabs(0.2 - oy) * kOutH * 0.5,
				std::fabs(0.3 - px) * kOutW * 0.5, std::fabs(0.2 - py) * kOutH * 0.5));
		}

		// ---- (b) (150, 7, -15) からカメラ右方向へ 0.1 m ----
		{
			const XMFLOAT3 prevPos = { 150.0f, 7.0f, -15.0f };
			const FViewMatrices prev = MakeViewMatrices(prevPos, forward, proj);
			// ViewMatrix 上 3x3 の列 0 = ワールド空間のカメラ右方向
			const XMFLOAT3 right = { prev.ViewMatrix._11, prev.ViewMatrix._21, prev.ViewMatrix._31 };
			const XMFLOAT3 curPos = { prevPos.x + 0.1f * right.x, prevPos.y + 0.1f * right.y, prevPos.z + 0.1f * right.z };
			const FViewMatrices cur = MakeViewMatrices(curPos, forward, proj);
			const XMFLOAT4X4 c2p = ComputeClipToPrevClip(cur, prev);

			double px, py;
			ReprojectScreenPos(c2p, -0.01357995, 0.0, d10, px, py);
			R.Check("move 0.1 m @150: (-0.01357995,0,d10) prev s.x", px, 0.0, 1e-5);
			R.Check("move 0.1 m @150: (-0.01357995,0,d10) prev s.y", py, 0.0, 1e-5);
			R.Check("move 0.1 m @150: C2P(2,0)", c2p._31, -1.35772353, 1e-4);
			R.Check("move 0.1 m @150: C2P(3,0)", c2p._41, 1.35799513, 1e-4);
		}

		R.EndTest();
	}


	// ============================================================
	//  T5: ヨー 1 度の回転: 再投影後の ndc.x はデバイス Z に依らない (A.5)
	//   画面中央 (0, 0) を d = 0.5 / 0.99 / 1.0 / Q で -> ndc.x = tan(1 度) * P11 = 0.02370389, ndc.y = 0
	//   (原点 ヨー 0 -> 1 度、追加で (150, 7, -15) ヨー 17 -> 18 度)
	// ============================================================
	void TestT5(FSelfTestReport& R)
	{
		R.BeginTest("T5");

		const XMFLOAT4X4 proj = MakeTestProjection();
		const double Q = (double)kFar / ((double)kFar - (double)kNear);
		static const double kDepths[] = { 0.5, 0.99, 1.0, 0.0 /* = Q */ };

		struct FPose { XMFLOAT3 Position; float PrevYaw; };
		static const FPose kPoses[] = { { { 0.0f, 0.0f, 0.0f }, 0.0f }, { { 150.0f, 7.0f, -15.0f }, 17.0f } };
		for (const FPose& pose : kPoses)
		{
			const FViewMatrices prev = MakeViewMatrices(pose.Position, YawForward(pose.PrevYaw), proj);
			const FViewMatrices cur = MakeViewMatrices(pose.Position, YawForward(pose.PrevYaw + 1.0f), proj);
			const XMFLOAT4X4 c2p = ComputeClipToPrevClip(cur, prev);
			for (double d : kDepths)
			{
				const double dd = (d == 0.0) ? Q : d;
				double px, py;
				ReprojectScreenPos(c2p, 0.0, 0.0, dd, px, py);
				const std::string at = Fmt("yaw %g->%g d=%s", pose.PrevYaw, pose.PrevYaw + 1.0f, (d == 0.0) ? "Q" : Fmt("%g", d).c_str());
				R.Check(at + " ndc.x", px, 0.02370389, 1e-6);
				R.Check(at + " ndc.y", py, 0.0, 1e-6);
			}
		}

		R.EndTest();
	}


	// ============================================================
	//  T6: ベロシティのエンコード (A.4, VelocityCommon.hlsl の CPU 鏡像)
	//   V = -0.01357995 -> E = 0.49660417 -> UNORM16 32545 -> 復号 -0.0135772
	//   V = 0 -> 32767, V = +2 -> 65469, V = -2 -> 65, V = 3 -> ±2 クランプで 65469 (0 = 未書き込みと衝突しない)
	//   (2, 2) の復号は 2.0000021 (HSP = s - V <= -1 が画面外 = 背面ガードの履歴棄却)
	// ============================================================
	unsigned QuantizeUNorm16(float E)
	{
		// D3D の float -> UNORM 変換 (飽和 + 最近接丸め)
		const float e = (std::min)((std::max)(E, 0.0f), 1.0f);
		return (unsigned)std::floor((double)e * 65535.0 + 0.5);
	}

	void TestT6(FSelfTestReport& R)
	{
		R.BeginTest("T6");

		struct FVector { float V; unsigned UNorm; };
		static const FVector kVectors[] =
		{
			{ -0.01357995f, 32545u },
			{  0.0f,        32767u },
			{  2.0f,        65469u },
			{ -2.0f,           65u },
			{  3.0f,        65469u },	// ±2 クランプ
			{ -3.0f,           65u },
		};
		for (const FVector& v : kVectors)
		{
			// x / y の両成分で同じ結果 (成分ごとの式)
			const XMFLOAT2 ex = EncodeVelocityToTextureCPU({ v.V, 0.0f });
			const XMFLOAT2 ey = EncodeVelocityToTextureCPU({ 0.0f, v.V });
			R.Check(Fmt("UNORM16(E(%g)).x", v.V), QuantizeUNorm16(ex.x), v.UNorm, 0.0);
			R.Check(Fmt("UNORM16(E(%g)).y", v.V), QuantizeUNorm16(ey.y), v.UNorm, 0.0);
			R.CheckBool(Fmt("E(%g) written (> 0)", v.V), QuantizeUNorm16(ex.x) > 0u, true);
		}

		R.Check("E(-0.01357995).x", EncodeVelocityToTextureCPU({ -0.01357995f, 0.0f }).x, 0.49660417, 1e-6);
		R.Check("E(0).x", EncodeVelocityToTextureCPU({ 0.0f, 0.0f }).x, 0.49999237, 1e-6);
		R.Check("E(3).x (clamped)", EncodeVelocityToTextureCPU({ 3.0f, 0.0f }).x, 0.99899237, 1e-6);

		const XMFLOAT2 d = DecodeVelocityFromTextureCPU({ 32545.0f / 65535.0f, 32767.0f / 65535.0f });
		R.Check("D(32545/65535)", d.x, -0.0135772, 1e-6);
		R.Check("D(32767/65535)", d.y, 0.0, 1e-6);
		const XMFLOAT2 d2 = DecodeVelocityFromTextureCPU({ 65469.0f / 65535.0f, 65469.0f / 65535.0f });
		R.Check("D(65469/65535) (2,2)", d2.x, 2.0000021, 1e-6);

		// 往復誤差: 1 LSB = 6.1158e-5 NDC (0.0587 px @1920) の半分以内
		const XMFLOAT2 rt = DecodeVelocityFromTextureCPU({ QuantizeUNorm16(EncodeVelocityToTextureCPU({ 0.01357995f, 0.0f }).x) / 65535.0f, 0.0f });
		R.Check("round trip +0.01357995", rt.x, 0.01357995, 0.5 * 6.1158e-5);

		R.EndTest();
	}


	// ============================================================
	//  T16: FSceneVelocityData のライフサイクル (§9.2)
	//   1. Register (T0) -> プッシュ T1 のフレーム: テレポート扱いで速度無し
	//   2. T1 -> T2 のプッシュ: 速度あり (Prev = T1)
	//   3. プッシュ無しのフレーム: 速度無し (Prev = Current = T2)
	//   4. MarkTeleported -> 次のプッシュ: 速度無し (その次のプッシュからは速度あり)
	//   5. Remove -> Find は null
	// ============================================================
	void TestT16(FSelfTestReport& R)
	{
		R.BeginTest("T16");

		// キーとしてだけ使う (参照しない) ダミーのコンポーネントアドレス
		const UPrimitiveComponent* comp = reinterpret_cast<const UPrimitiveComponent*>(static_cast<uintptr_t>(0x1000));
		const UPrimitiveComponent* other = reinterpret_cast<const UPrimitiveComponent*>(static_cast<uintptr_t>(0x2000));
		auto translation = [](float X)
			{
				XMFLOAT4X4 m;
				XMStoreFloat4x4(&m, XMMatrixTranslation(X, 0.0f, 0.0f));
				return m;
			};
		const XMFLOAT4X4 T0 = translation(0.0f), T1 = translation(1.0f), T2 = translation(1.5f);
		const XMFLOAT4X4 T3 = translation(10.0f), T4 = translation(10.5f);

		// ゲーム側 1 フレーム分 (FScene::UpdateAllPrimitiveSceneInfos と同じ順)
		FSceneVelocityData vd;
		auto frame = [&](const XMFLOAT4X4* Push)
			{
				vd.StartFrame();
				if (Push)
					vd.UpdateTransform(comp, *Push);
				vd.EndFrameUpdates();
			};
		auto hasVelocity = [&]() { const FComponentVelocityData* d = vd.Find(comp); return d != nullptr && d->HasVelocity(); };
		auto prevX = [&]() { const FComponentVelocityData* d = vd.Find(comp); return d ? (double)d->PreviousLocalToWorld._41 : -1.0; };

		// 1. 登録 (スポーン) 直後の最初のプッシュはテレポート
		vd.Register(comp, T0);
		R.Check("Num after Register", (double)vd.Num(), 1.0, 0.0);
		R.CheckBool("Register: no velocity", hasVelocity(), false);
		frame(&T1);
		R.CheckBool("1. first push T1 (teleport): no velocity", hasVelocity(), false);
		R.Check("1. Prev = T1", prevX(), 1.0, 0.0);

		// 2. 通常のプッシュ
		frame(&T2);
		R.CheckBool("2. push T1 -> T2: velocity", hasVelocity(), true);
		R.Check("2. Prev = T1", prevX(), 1.0, 0.0);
		R.Check("2. Current = T2", (double)vd.Find(comp)->LocalToWorld._41, 1.5, 0.0);

		// 3. プッシュ無し (動きが止まった) フレーム
		frame(nullptr);
		R.CheckBool("3. no push: no velocity", hasVelocity(), false);
		R.Check("3. Prev = T2", prevX(), 1.5, 0.0);

		// 許容誤差 (FMatrix::Equals 1e-4) 未満の変化は速度無し
		{
			XMFLOAT4X4 tiny = T2;
			tiny._41 += 5.0e-5f;
			frame(&tiny);
			R.CheckBool("3b. change < 1e-4: no velocity", hasVelocity(), false);
			frame(&T2);
		}

		// 4. テレポート (Reset Actor / INI 適用) -> 次のプッシュは速度無し、その次から速度あり
		frame(nullptr);
		vd.MarkTeleported(comp);
		vd.MarkTeleported(other);			// 未登録は何もしない
		R.Check("4. MarkTeleported(unknown): Num", (double)vd.Num(), 1.0, 0.0);
		frame(&T3);
		R.CheckBool("4. push after MarkTeleported: no velocity", hasVelocity(), false);
		R.Check("4. Prev = T3", prevX(), 10.0, 0.0);
		frame(&T4);
		R.CheckBool("4b. next push T3 -> T4: velocity", hasVelocity(), true);

		// 未登録のコンポーネントへの UpdateTransform は Register 扱い (テレポート)
		vd.StartFrame();
		vd.UpdateTransform(other, T1);
		vd.EndFrameUpdates();
		const FComponentVelocityData* o = vd.Find(other);
		R.CheckBool("UpdateTransform(unknown) registers", o != nullptr, true);
		R.CheckBool("UpdateTransform(unknown): no velocity", o != nullptr && o->HasVelocity(), false);

		// 5. 削除
		vd.Remove(comp);
		R.CheckBool("5. Remove -> Find == null", vd.Find(comp) == nullptr, true);
		R.Check("5. Num after Remove", (double)vd.Num(), 1.0, 0.0);

		R.EndTest();
	}


	// ============================================================
	//  T12: ComputeViewFamilyInfo が §4.2 の表を再現する
	//   R / S / H / P / パス構成 / UF = H.x / R.x / N (ジッタサンプル数) / ミップバイアス / SAMPLE_DISTANCE 閾値
	// ============================================================
	void TestT12(FSelfTestReport& R)
	{
		struct FRow
		{
			const char* Name;
			int   Method;         // AntiAliasingMethod
			bool  bUpsampling;    // bTemporalAAUpsampling
			float SP, HSP;
			unsigned Rx, Ry, Sx, Sy, Hx, Hy, Px, Py;
			int   Pass;           // -1 = TAA 無し, それ以外 ETAAPassConfig
			double UF;            // H.x / R.x (TAA 無しは照合しない)
			double Bias;
			double Threshold;     // < 0 = 該当なし (MainUpsampling 以外)
			bool  bSpatialUpscale;
			int   N;              // ジッタサンプル数 (TemporalAASamples = 8)。0 = 該当なし (TAA 無し)
		};
		static const FRow kRows[] =
		{
			{ "None 100",          0, true, 100.0f, 100.0f, 1920, 1080, 1920, 1080, 1920, 1080, 1920, 1080, -1, 0.0,    0.0,     -1.0,   false,  0 },
			{ "None 50",           0, true,  50.0f, 100.0f,  960,  540,  960,  540,  960,  540,  960,  540, -1, 0.0,    0.0,     -1.0,   true,   0 },
			{ "TAA 100/100",       2, false, 100.0f, 100.0f, 1920, 1080, 1920, 1080, 1920, 1080, 1920, 1080, 0, 1.0,    0.0,     -1.0,   false,  8 },
			{ "TAA 71/100",        2, false,  71.0f, 100.0f, 1364,  767, 1364,  767, 1364,  767, 1364,  767, 0, 1.0,    0.0,     -1.0,   true,   8 },
			{ "TAA 100/200",       2, false, 100.0f, 200.0f, 1920, 1080, 1920, 1080, 3840, 2160, 1920, 1080, 2, 2.0,    0.0,     -1.0,   false,  8 },
			{ "TAAU 100/100",      2, true,  100.0f, 100.0f, 1920, 1080, 1920, 1080, 1920, 1080, 1920, 1080, 1, 1.0,   -0.30,    1.5100, false,  8 },
			{ "TAAU 50/100",       2, true,   50.0f, 100.0f,  960,  540, 1920, 1080, 1920, 1080, 1920, 1080, 1, 2.0,   -1.30,    1.3000, false, 32 },
			{ "TAAU 67/100",       2, true,   67.0f, 100.0f, 1287,  724, 1920, 1080, 1920, 1080, 1920, 1080, 1, 1.4918, -0.8771,  1.4067, false, 17 },
			{ "TAAU 71/100",       2, true,   71.0f, 100.0f, 1364,  767, 1920, 1080, 1920, 1080, 1920, 1080, 1, 1.4076, -0.7933,  1.4244, false, 15 },
			{ "TAAU 75/100",       2, true,   75.0f, 100.0f, 1440,  810, 1920, 1080, 1920, 1080, 1920, 1080, 1, 1.3333, -0.7150,  1.4400, false, 14 },
			{ "TAAU 150/100",      2, true,  150.0f, 100.0f, 2880, 1620, 1920, 1080, 1920, 1080, 1920, 1080, 1, 0.6667, -0.30,    1.5800, false,  8 },
			{ "TAAU 200/100",      2, true,  200.0f, 100.0f, 3840, 2160, 1920, 1080, 1920, 1080, 1920, 1080, 1, 0.5,   -0.30,    1.6150, false,  8 },
			{ "TAAU 100/200",      2, true,  100.0f, 200.0f, 1920, 1080, 1920, 1080, 3840, 2160, 1920, 1080, 2, 2.0,   -0.30,   -1.0,   false,  8 },
			{ "TAAU 50/150",       2, true,   50.0f, 150.0f,  960,  540, 1920, 1080, 2880, 1620, 1920, 1080, 2, 3.0,   -1.30,   -1.0,   false, 32 },
			{ "TAAU 30->50/100",   2, true,   30.0f, 100.0f,  960,  540, 1920, 1080, 1920, 1080, 1920, 1080, 1, 2.0,   -1.30,    1.3000, false, 32 },
			{ "None 300->200",     0, true,  300.0f, 100.0f, 3840, 2160, 3840, 2160, 3840, 2160, 3840, 2160, -1, 0.0,   0.0,     -1.0,   true,   0 },
		};

		const XMUINT2 O = { kOutW, kOutH };

		R.BeginTest("T12");
		for (const FRow& row : kRows)
		{
			FAntiAliasingParams p;
			p.AntiAliasingMethod = row.Method;
			p.bTemporalAAUpsampling = row.bUpsampling;
			p.ScreenPercentage = row.SP;
			p.TemporalAAHistoryScreenPercentage = row.HSP;

			const FViewFamilyInfo F = ComputeViewFamilyInfo(p, O, false);
			const std::string n = row.Name;

			R.Check(n + " R.x", F.RenderExtent.x, row.Rx, 0.0);
			R.Check(n + " R.y", F.RenderExtent.y, row.Ry, 0.0);
			R.Check(n + " S.x", F.SecondaryExtent.x, row.Sx, 0.0);
			R.Check(n + " S.y", F.SecondaryExtent.y, row.Sy, 0.0);
			R.Check(n + " H.x", F.HistoryExtent.x, row.Hx, 0.0);
			R.Check(n + " H.y", F.HistoryExtent.y, row.Hy, 0.0);
			R.Check(n + " P.x", F.PostProcessExtent.x, row.Px, 0.0);
			R.Check(n + " P.y", F.PostProcessExtent.y, row.Py, 0.0);
			R.CheckBool(n + " bTemporalAA", F.bTemporalAA, row.Pass >= 0);
			R.CheckBool(n + " bSpatialUpscale", F.bSpatialUpscale, row.bSpatialUpscale);
			if (row.Pass >= 0)
			{
				R.Check(n + " pass", (double)(int)F.TAAPass, row.Pass, 0.0);
				// ジッタサンプル数 N (§4.4 と同じ規則: TemporalUpscale なら int(8 * max(1, 1/f^2)))
				const bool bTAAU = (F.PrimaryScreenPercentageMethod == EPrimaryScreenPercentageMethod::TemporalUpscale);
				R.Check(n + " N", ComputeTemporalAASampleCount(bTAAU, p.TemporalAASamples, F.EffectivePrimaryResolutionFraction), row.N, 0.0);
				const double uf = (double)F.HistoryExtent.x / (double)F.RenderExtent.x;
				R.Check(n + " UF", uf, row.UF, 1e-4);
				if (row.Threshold >= 0.0)
				{
					// SAMPLE_DISTANCE の閾値 (MainUpsampling のみ): lerp(1.51, 1.3, UF - 1) を外挿も含めて (クランプ無し)
					const double threshold = 1.51 + (1.3 - 1.51) * (uf - 1.0);
					R.Check(n + " SampleDistanceThreshold", threshold, row.Threshold, 1e-4);
				}
			}
			R.Check(n + " mip bias", ComputeViewTextureMipBias(F, p), row.Bias, 1e-4);
		}
		R.EndTest();
	}


	// ============================================================
	//  T13: LinearDepth <-> デバイス Z の往復 (z = 0.1, 1, 10, 100, 499) と遠方規則 (499.5, 500)
	// ============================================================
	void TestT13(FSelfTestReport& R)
	{
		struct FVector { double ViewZ; double DeviceZ; };
		static const FVector kVectors[] =
		{
			{ 0.1,   0.00000000 },
			{ 1.0,   0.90018004 },
			{ 10.0,  0.99019804 },
			{ 100.0, 0.99919984 },
			{ 499.0, 0.99999960 },
			{ 499.5, 1.00020004 },	// 遠方 (z >= 0.999 f): 無限遠 d = Q
			{ 500.0, 1.00020004 },
		};

		R.BeginTest("T13");
		for (const FVector& v : kVectors)
		{
			const double d = ViewZToDeviceZ(v.ViewZ, kNear, kFar);
			R.Check(Fmt("d(z=%g)", v.ViewZ), d, v.DeviceZ, 1e-6 * (std::max)(1.0, std::fabs(v.DeviceZ)));

			if (v.ViewZ < 0.999 * kFar)
			{
				// 往復: z -> d -> z (相対 1e-6)
				const double z = DeviceZToViewZ(d, kNear, kFar);
				R.Check(Fmt("z(d(z=%g))", v.ViewZ), z, v.ViewZ, 1e-6 * v.ViewZ);
			}
		}

		// float の LinearDepthPS はデバイス Z = 1 (遠平面) で 499.969 を返し、遠方閾値 499.5 を超える (A.5)
		const float n = kNear, f = kFar;
		const float zFar = n * f / (f - 1.0f * (f - n));
		R.Check("float LinearDepth(d=1)", zFar, 499.969, 1e-3);
		R.CheckBool("float LinearDepth(d=1) >= 0.999 f", zFar >= 0.999f * kFar, true);
		R.EndTest();
	}


	// ============================================================
	//  T15: IsLargeCameraMovement (閾値 45 度 / 100 m)
	//   ヨー 44 度 -> false, 46 度 -> true; 移動 99 m -> false, 101 m -> true
	// ============================================================
	void TestT15(FSelfTestReport& R)
	{
		const XMFLOAT4X4 proj = MakeTestProjection();
		const XMFLOAT3 origin = { 0.0f, 7.0f, -15.0f };
		const float pitch = XMConvertToRadians(20.0f);

		auto makeView = [&](float YawDeg, const XMFLOAT3& Position)
			{
				const float yaw = XMConvertToRadians(YawDeg);
				// ピッチ 20 度 (下向き) の前方ベクトルをワールド Y 軸回りにヨー回転
				const XMFLOAT3 forward = { std::cos(pitch) * std::sin(yaw), -std::sin(pitch), std::cos(pitch) * std::cos(yaw) };
				FViewMatrices vm;
				vm.Init(MakeLookTo(Position, forward), proj, Position);
				return vm;
			};

		const FViewMatrices prev = makeView(0.0f, origin);
		const float rotTh = 45.0f, transTh = 100.0f;

		R.BeginTest("T15");
		R.CheckBool("yaw 0 (static)", IsLargeCameraMovement(makeView(0.0f, origin), prev, rotTh, transTh), false);
		R.CheckBool("yaw 44", IsLargeCameraMovement(makeView(44.0f, origin), prev, rotTh, transTh), false);
		R.CheckBool("yaw 46", IsLargeCameraMovement(makeView(46.0f, origin), prev, rotTh, transTh), true);
		R.CheckBool("yaw -46", IsLargeCameraMovement(makeView(-46.0f, origin), prev, rotTh, transTh), true);
		R.CheckBool("move 99 m", IsLargeCameraMovement(makeView(0.0f, { origin.x + 99.0f, origin.y, origin.z }), prev, rotTh, transTh), false);
		R.CheckBool("move 101 m", IsLargeCameraMovement(makeView(0.0f, { origin.x + 101.0f, origin.y, origin.z }), prev, rotTh, transTh), true);
		R.EndTest();
	}


	// ============================================================
	//  T7: ComputeTemporalAASampleWeights (§4.8.6, A.7)
	//   ガウス (FS 1): J = (-0.164, 0.284) / (0, 0) / (0, -1/6)
	//   Catmull-Rom: FS 1 / FS 0.5 (正規化のみ, ガード無し) /
	//                FS 0.1 (総和 0 -> 中心の one-hot) / FS 0.4, J = (-0.35, 0.35) (Σw/Σ|w| = 0.121 < 0.25 -> 中心) /
	//                FS 0.1, J = (-2/3, 0) (CVar 3 のジッタ -> 最近傍 (-1, 0) の one-hot)
	//   期待値は同じ式を double で計算した値。全出力が有限であること
	// ============================================================
	void TestT7(FSelfTestReport& R)
	{
		struct FCase
		{
			const char* Name;
			float Jx, Jy, FS;
			bool  bCatmullRom;
			double Sample[9];
			double Plus[5];			// Plus[0] < -9 = プラスは照合しない
		};
		static const FCase kCases[] =
		{
			{ "gauss J(-0.164,0.284)", -0.164f, 0.284f, 1.0f, false,
				{ 0.0033504, 0.0156107, 0.0007459, 0.1214860, 0.5660479, 0.0270465, 0.0451739, 0.2104817, 0.0100571 },
				{ 0.0165953, 0.1291479, 0.6017479, 0.0287523, 0.2237565 } },
			{ "gauss J(0,0)", 0.0f, 0.0f, 1.0f, false,
				{ 0.0070915, 0.0700280, 0.0070915, 0.0700280, 0.6915221, 0.0700280, 0.0070915, 0.0700280, 0.0070915 },
				{ 0.0720724, 0.0720724, 0.7117104, 0.0720724, 0.0720724 } },
			{ "gauss J(0,-1/6)", 0.0f, -1.0f / 6.0f, 1.0f, false,
				{ 0.0144691, 0.1428812, 0.0144691, 0.0665984, 0.6576547, 0.0665984, 0.0031435, 0.0310422, 0.0031435 },
				{ 0.1480980, 0.0690300, 0.6816666, 0.0690300, 0.0321756 } },
			{ "CR FS1 J(-0.164,0.284)", -0.164f, 0.284f, 1.0f, true,
				{ -0.0090381, -0.0657258, 0.0040098, 0.1033859, 0.7518314, -0.0458677, 0.0333917, 0.2428273, -0.0148144 },
				{ -0.0666286, 0.1048059, 0.7621579, -0.0464977, 0.2461625 } },
			{ "CR FS0.5 J(-0.164,0.284)", -0.164f, 0.284f, 0.5f, true,
				{ 0.0, 0.0, 0.0, -0.0567882, 1.2316044, 0.0, 0.0084503, -0.1832665, 0.0 },
				{ 0.0, -0.0572722, 1.2421005, 0.0, -0.1848283 } },
			{ "CR FS0.1 J(-0.164,0.284) sum0", -0.164f, 0.284f, 0.1f, true,
				{ 0, 0, 0, 0, 1, 0, 0, 0, 0 },
				{ 0, 0, 1, 0, 0 } },
			{ "CR FS0.4 J(-0.35,0.35) ill-conditioned", -0.35f, 0.35f, 0.4f, true,
				{ 0, 0, 0, 0, 1, 0, 0, 0, 0 },
				{ 0, 0, 1, 0, 0 } },
			{ "CR FS0.1 J(-2/3,0) nearest(-1,0)", -2.0f / 3.0f, 0.0f, 0.1f, true,
				{ 0, 0, 0, 1, 0, 0, 0, 0, 0 },
				{ 0, 1, 0, 0, 0 } },
		};

		R.BeginTest("T7");
		for (const FCase& c : kCases)
		{
			float sw[9], pw[5];
			ComputeTemporalAASampleWeights({ c.Jx, c.Jy }, c.FS, c.bCatmullRom, sw, pw);
			bool bFinite = true;
			for (int i = 0; i < 9; ++i)
			{
				R.Check(Fmt("%s S[%d]", c.Name, i), sw[i], c.Sample[i], 1e-4);
				bFinite = bFinite && std::isfinite(sw[i]);
			}
			for (int i = 0; i < 5; ++i)
			{
				R.Check(Fmt("%s P[%d]", c.Name, i), pw[i], c.Plus[i], 1e-4);
				bFinite = bFinite && std::isfinite(pw[i]);
			}
			R.CheckBool(Fmt("%s finite", c.Name), bFinite, true);
		}

		// 追加: 実際のガウスジッタ列 (Index 0..7, FS 1) とガード領域 (FS 0.1..2, CR) で全出力が有限、
		// 正規化された重みの和が 1 (one-hot も含む)
		double worstSum = 0.0;
		bool bAllFinite = true;
		for (int idx = 0; idx < 8; ++idx)
		{
			const XMFLOAT2 j = ComputeTemporalAASample(false, 8, 8, idx, 1.0f);
			for (int fsStep = 1; fsStep <= 20; ++fsStep)
			{
				const float fs = 0.1f * (float)fsStep;
				for (int cr = 0; cr < 2; ++cr)
				{
					float sw[9], pw[5];
					ComputeTemporalAASampleWeights(j, fs, cr != 0, sw, pw);
					double s9 = 0.0, s5 = 0.0;
					for (int i = 0; i < 9; ++i) { s9 += sw[i]; bAllFinite = bAllFinite && std::isfinite(sw[i]); }
					for (int i = 0; i < 5; ++i) { s5 += pw[i]; bAllFinite = bAllFinite && std::isfinite(pw[i]); }
					worstSum = (std::max)(worstSum, (std::max)(std::fabs(s9 - 1.0), std::fabs(s5 - 1.0)));
				}
			}
		}
		R.CheckBool("scan: all finite", bAllFinite, true);
		R.Check("scan: max |sum - 1|", worstSum, 0.0, 1e-5);
		R.EndTest();
	}


	// ============================================================
	//  T8: TAAU の空間重み曲線 ComputeSampleWeigth (A.6)
	//   w(x^2) = (0.905 x^2 - 1.9) x^2 + 1, x^2 = saturate(UF^2 |d|^2)
	//   x^2 = 0 / .25 / .5 / .75 / 1 -> 1 / .5815625 / .27625 / .0840625 / .005 (x^2 > 1 は飽和して .005)
	// ============================================================
	void TestT8(FSelfTestReport& R)
	{
		struct FCase { float Dx, Dy, UF; double X2; double Expected; };
		static const FCase kCases[] =
		{
			{ 0.0f,  0.0f,        1.0f, 0.0,  1.0       },
			{ 0.5f,  0.0f,        1.0f, 0.25, 0.5815625 },
			{ 0.5f,  0.5f,        1.0f, 0.5,  0.27625   },
			{ 0.5f,  0.70710678f, 1.0f, 0.75, 0.0840625 },
			{ 1.0f,  0.0f,        1.0f, 1.0,  0.005     },
			{ 0.0f, -0.5f,        2.0f, 1.0,  0.005     },	// UF = 2 では半分の距離で同じ重み
			{ 0.25f, 0.0f,        2.0f, 0.25, 0.5815625 },
			{ 2.0f,  0.0f,        1.0f, 4.0,  0.005     },	// 飽和 (x^2 > 1)
			{ -1.0f, -1.0f,       0.5f, 0.5,  0.27625   },	// UF = 0.5 (SP 200 %) では 2 倍の距離で同じ重み
		};

		R.BeginTest("T8");
		for (const FCase& c : kCases)
		{
			R.Check(Fmt("w(d=(%g,%g), UF=%g) [x^2=%g]", c.Dx, c.Dy, c.UF, c.X2), ComputeSampleWeigthCPU({ c.Dx, c.Dy }, c.UF), c.Expected, 1e-6);
		}
		R.EndTest();
	}


	// ============================================================
	//  T9: TAAU の入力写像 (A.6。TemporalAA.hlsl 手順 1-2 と FilterCurrentFrame の FTW)
	//   O = 1920x1080, R = 960x540 (UF = 2), J = (0.25, -1/6):
	//     p = (100,57)    -> K (50,28),  dKO (0, 1/12),    中心 0.947920525, 他 0.005, FTW 0.987920525
	//     p = (101,57)    -> K (51,28),  dKO (-0.5, 1/12), 全 0.005, FTW 0.045
	//     p = (0,0)       -> K (0,0),    dKO (0, -5/12),   中心 0.116994599, 他 0.005, FTW 0.156994599
	//     p = (1919,1079) -> K (960,539) -> クランプ (959,539), dKO (-0.5, 1/12), FTW 0.045
	//   UF = 1 (R = O), p = (10,10): K (10,10), dKO (0.25, -1/6), 中心 0.835848042, (1,0) 0.193799431,
	//     (0,-1) 0.080338783, FTW 1.139986256
	//   FTW = 9 サンプル (High) の非正規化空間重みの総和 (FTW モード 0)
	// ============================================================
	void TestT9(FSelfTestReport& R)
	{
		static const int kOffsets[9][2] = { {-1,-1},{0,-1},{1,-1},{-1,0},{0,0},{1,0},{-1,1},{0,1},{1,1} };
		struct FCase
		{
			unsigned Px, Py, Rx, Ry;
			int Kx, Ky;
			double DKOx, DKOy;
			double Centre, FTW;
		};
		static const FCase kCases[] =
		{
			{  100,   57,  960,  540,  50,  28,  0.0,   1.0 / 12.0, 0.947920525, 0.987920525 },
			{  101,   57,  960,  540,  51,  28, -0.5,   1.0 / 12.0, 0.005,       0.045       },
			{    0,    0,  960,  540,   0,   0,  0.0,  -5.0 / 12.0, 0.116994599, 0.156994599 },
			{ 1919, 1079,  960,  540, 959, 539, -0.5,   1.0 / 12.0, 0.005,       0.045       },
			{   10,   10, 1920, 1080,  10,  10,  0.25, -1.0 / 6.0,  0.835848042, 1.139986256 },
		};
		const XMFLOAT2 J = { 0.25f, -1.0f / 6.0f };
		const XMUINT2 O = { kOutW, kOutH };

		R.BeginTest("T9");
		for (const FCase& c : kCases)
		{
			const XMUINT2 In = { c.Rx, c.Ry };
			const float UF = (float)O.x / (float)In.x;
			XMINT2 K;
			XMFLOAT2 dKO;
			ComputeTAAUInputMappingCPU({ c.Px, c.Py }, O, In, J, K, dKO);
			const std::string at = Fmt("p(%u,%u) R %ux%u", c.Px, c.Py, c.Rx, c.Ry);
			R.Check(at + " K.x", K.x, c.Kx, 0.0);
			R.Check(at + " K.y", K.y, c.Ky, 0.0);
			R.Check(at + " dKO.x", dKO.x, c.DKOx, 1e-5);
			R.Check(at + " dKO.y", dKO.y, c.DKOy, 1e-5);

			// FilterCurrentFrame (AA_UPSAMPLE, 9 サンプル): ws = ComputeSampleWeigth(o - dKO, UF)、FTW = Σws
			double ftw = 0.0, centre = 0.0;
			for (int i = 0; i < 9; ++i)
			{
				const float ws = ComputeSampleWeigthCPU({ (float)kOffsets[i][0] - dKO.x, (float)kOffsets[i][1] - dKO.y }, UF);
				ftw += ws;
				if (i == 4) centre = ws;
			}
			R.Check(at + " centre weight", centre, c.Centre, 1e-5);
			R.Check(at + " FTW", ftw, c.FTW, 1e-5);
		}

		// UF = 1 の個別の重み (A.6): (1,0) = 0.193799431, (0,-1) = 0.080338783
		{
			XMINT2 K;
			XMFLOAT2 dKO;
			ComputeTAAUInputMappingCPU({ 10u, 10u }, O, O, J, K, dKO);
			R.Check("UF 1 p(10,10) w(1,0)", ComputeSampleWeigthCPU({ 1.0f - dKO.x, 0.0f - dKO.y }, 1.0f), 0.193799431, 1e-5);
			R.Check("UF 1 p(10,10) w(0,-1)", ComputeSampleWeigthCPU({ 0.0f - dKO.x, -1.0f - dKO.y }, 1.0f), 0.080338783, 1e-5);
		}

		// dKO は常に [-0.5, 0.5] の内側 (出力画素を間引いて走査、SP 50 / 71 / 150 %、一様ジッタ #0..#7)
		{
			double worst = -1.0;
			static const unsigned kInputWidths[] = { 960, 1364, 2880 };
			for (unsigned inW : kInputWidths)
			{
				const XMUINT2 In = { inW, (unsigned)std::ceil(1080.0 * (double)inW / 1920.0 - 1e-6) };
				for (int idx = 0; idx < 8; ++idx)
				{
					const XMFLOAT2 j = ComputeTemporalAASample(true, 8, 32, idx, 1.0f);
					for (unsigned p = 0; p < O.x; p += 7)
					{
						XMINT2 K;
						XMFLOAT2 dKO;
						ComputeTAAUInputMappingCPU({ p, (p * 9u) % O.y }, O, In, j, K, dKO);
						worst = (std::max)(worst, (std::max)(std::fabs((double)dKO.x), std::fabs((double)dKO.y)) - 0.5);
					}
				}
			}
			R.CheckBool(Fmt("scan: max |dKO| - 0.5 = %.3g <= 0", worst), worst <= 1e-6, true);
		}
		R.EndTest();
	}


	// ============================================================
	//  T10: Catmull-Rom の 1D 重み (A.8。CatmullRom5Taps の w0..w3, 統合 W0/W1/W2, S1 オフセット, 5 タップ和)
	//   f = 0 / .25 / .5 / .75。f = 0.25 の 2D 中心タップ重み W1 x W1 = 1.1962890625
	// ============================================================
	void TestT10(FSelfTestReport& R)
	{
		struct FCase { double F; double W[4]; double S1; double Sum5; double Centre; };
		static const FCase kCases[] =
		{
			{ 0.0,  {  0.0,        1.0,       0.0,        0.0        }, 0.0,                 1.0,          1.0          },
			{ 0.25, { -0.0703125,  0.8671875, 0.2265625, -0.0234375  }, 0.20714285714285716, 0.9912109375, 1.1962890625 },
			{ 0.5,  { -0.0625,     0.5625,    0.5625,    -0.0625     }, 0.5,                 0.984375,     1.265625     },
			{ 0.75, { -0.0234375,  0.2265625, 0.8671875, -0.0703125  }, 0.7928571428571428,  0.9912109375, 1.1962890625 },
		};

		R.BeginTest("T10");
		for (const FCase& c : kCases)
		{
			float w[4];
			CatmullRomWeights1DCPU((float)c.F, w);
			for (int i = 0; i < 4; ++i)
				R.Check(Fmt("f=%g w%d", c.F, i), w[i], c.W[i], 1e-7);
			const double W0 = w[0], W1 = (double)w[1] + (double)w[2], W2 = w[3];
			R.Check(Fmt("f=%g S1 offset w2/W1", c.F), (double)w[2] / W1, c.S1, 1e-7);
			const double sum5 = W1 * W0 + W0 * W1 + W1 * W1 + W2 * W1 + W1 * W2;	// 角除去の 5 タップ (fx = fy)
			R.Check(Fmt("f=%g 5-tap sum", c.F), sum5, c.Sum5, 1e-7);
			R.Check(Fmt("f=%g centre W1*W1", c.F), W1 * W1, c.Centre, 1e-7);
			R.Check(Fmt("f=%g sum w", c.F), (double)w[0] + w[1] + w[2] + w[3], 1.0, 1e-7);
		}
		R.EndTest();
	}


	// ============================================================
	//  T11: Mitchell-Netravali (B = C = 1/3) のカーネルと正規化タップ (A.8, §6.6)
	//   k(0, .25, ..., 2) = 0.888889, 0.782118, 0.534722, 0.256076, 0.055556, -0.023438, -0.034722, -0.014757, 0
	//   比 2  : 出力画素 10 -> 入力 21.0、先頭 17、正規化タップ (i = 17..24)
	//           -0.007378, -0.011719, 0.128038, 0.391059, 0.391059, 0.128038, -0.011719, -0.007378 (生の和 2.0)
	//   比 1.5: 入力 15.75、先頭 13、(i = 13..18) -0.023148, 0.116770, 0.559156, 0.356481, -0.004287, -0.004973 (生の和 1.5)
	// ============================================================
	void TestT11(FSelfTestReport& R)
	{
		static const double kKernel[9] =
		{
			0.888888889, 0.782118056, 0.534722222, 0.256076389, 0.055555556, -0.0234375, -0.034722222, -0.014756944, 0.0,
		};
		struct FTaps { float Ratio; int First; double RawSum; double W[9]; };
		static const FTaps kTaps[] =
		{
			{ 2.0f, 17, 2.0, { -0.007378472, -0.01171875, 0.128038194, 0.391059028, 0.391059028, 0.128038194, -0.01171875, -0.007378472, 0.0 } },
			{ 1.5f, 13, 1.5, { -0.023148148, 0.116769547, 0.559156379, 0.356481481, -0.004286694, -0.004972565, 0.0, 0.0, 0.0 } },
		};

		R.BeginTest("T11");
		for (int i = 0; i < 9; ++i)
		{
			const float x = 0.25f * (float)i;
			R.Check(Fmt("k(%g)", x), MitchellNetravaliCPU(x), kKernel[i], 1e-6);
			R.Check(Fmt("k(-%g) symmetric", x), MitchellNetravaliCPU(-x), kKernel[i], 1e-6);
		}
		R.Check("k(2.5)", MitchellNetravaliCPU(2.5f), 0.0, 0.0);
		for (const FTaps& t : kTaps)
		{
			int first = 0;
			float w[9], raw = 0.0f;
			ComputeMitchellNetravaliTapsCPU(10.0f, t.Ratio, first, w, raw);
			R.Check(Fmt("ratio %g first", t.Ratio), first, t.First, 0.0);
			R.Check(Fmt("ratio %g raw sum", t.Ratio), raw, t.RawSum, 1e-5);	// float 9 項の和 (補助検査。仕様の 1e-6 は正規化タップ)
			double sum = 0.0;
			for (int k = 0; k < 9; ++k)
			{
				R.Check(Fmt("ratio %g tap %d", t.Ratio, first + k), w[k], t.W[k], 1e-6);
				sum += w[k];
			}
			R.Check(Fmt("ratio %g normalized sum", t.Ratio), sum, 1.0, 1e-6);
		}
		// 比 1 (H = S): 出力画素中心 = 入力画素中心。Mitchell は非補間なので中心 0.888889 / 両隣 0.055556
		{
			int first = 0;
			float w[9], raw = 0.0f;
			ComputeMitchellNetravaliTapsCPU(10.0f, 1.0f, first, w, raw);
			R.Check("ratio 1 first", first, 9, 0.0);
			R.Check("ratio 1 raw sum", raw, 1.0, 1e-6);
			R.Check("ratio 1 centre tap (i = 10)", w[1], 0.888888889, 1e-6);
			R.Check("ratio 1 neighbour tap (i = 11)", w[2], 0.055555556, 1e-6);
		}
		R.EndTest();
	}


	// ============================================================
	//  T14: TemporalAACommon.hlsl の CPU 鏡像 (A.9)
	//   YCoCg(1, 0.5, 0.25) = (2.25, 1.5, -0.25) と逆変換 / HdrWeightY(2.25, 1) = 0.16, E = 0.25 で 0.219178 /
	//   WeightedLerpFactors(1/6, 0.1, 0.04) = (0.97561, 0.02439) / 量子化誤差 (2^-10, R11G11B10 2^-6/2^-6/2^-5)
	// ============================================================
	void TestT14(FSelfTestReport& R)
	{
		R.BeginTest("T14");
		const XMFLOAT3 ycc = RGBToYCoCgCPU({ 1.0f, 0.5f, 0.25f });
		R.Check("YCoCg.Y", ycc.x, 2.25, 1e-6);
		R.Check("YCoCg.Co", ycc.y, 1.5, 1e-6);
		R.Check("YCoCg.Cg", ycc.z, -0.25, 1e-6);
		const XMFLOAT3 rgb = YCoCgToRGBCPU(ycc);
		R.Check("RGB.r", rgb.x, 1.0, 1e-6);
		R.Check("RGB.g", rgb.y, 0.5, 1e-6);
		R.Check("RGB.b", rgb.z, 0.25, 1e-6);

		// 往復 (いくつかの HDR 色)
		static const XMFLOAT3 kColors[] = { { 0.0f, 0.0f, 0.0f }, { 3.0f, 0.2f, 7.5f }, { 100.0f, 50.0f, 1.0f }, { 0.01f, 0.02f, 0.03f } };
		double worst = 0.0;
		for (const XMFLOAT3& c : kColors)
		{
			const XMFLOAT3 back = YCoCgToRGBCPU(RGBToYCoCgCPU(c));
			worst = (std::max)(worst, std::fabs((double)back.x - c.x) / (std::max)(1.0, (double)std::fabs(c.x)));
			worst = (std::max)(worst, std::fabs((double)back.y - c.y) / (std::max)(1.0, (double)std::fabs(c.y)));
			worst = (std::max)(worst, std::fabs((double)back.z - c.z) / (std::max)(1.0, (double)std::fabs(c.z)));
		}
		R.Check("YCoCg round trip (relative)", worst, 0.0, 1e-6);

		R.Check("HdrWeightY(2.25, 1)", HdrWeightYCPU(2.25f, 1.0f), 0.16, 1e-6);
		R.Check("HdrWeightY(2.25, 0.25)", HdrWeightYCPU(2.25f, 0.25f), 0.219178082, 1e-6);
		const XMFLOAT2 wl = WeightedLerpFactorsCPU(1.0f / 6.0f, 0.1f, 0.04f);
		R.Check("WeightedLerpFactors.x", wl.x, 0.975609756, 1e-6);
		R.Check("WeightedLerpFactors.y", wl.y, 0.024390244, 1e-6);
		R.Check("WeightedLerpFactors sum", (double)wl.x + (double)wl.y, 1.0, 1e-6);

		const XMFLOAT3 q16 = ComputePixelFormatQuantizationError(DXGI_FORMAT_R16G16B16A16_FLOAT);
		const XMFLOAT3 q11 = ComputePixelFormatQuantizationError(DXGI_FORMAT_R11G11B10_FLOAT);
		R.Check("QE RGBA16F", q16.x, 1.0 / 1024.0, 0.0);
		R.Check("QE R11G11B10 R", q11.x, 1.0 / 64.0, 0.0);
		R.Check("QE R11G11B10 G", q11.y, 1.0 / 64.0, 0.0);
		R.Check("QE R11G11B10 B", q11.z, 1.0 / 32.0, 0.0);
		R.EndTest();
	}


	// ============================================================
	//  GPU パリティ (§6.9): TemporalAASelfTest_CS を 1 回ディスパッチして読み戻し、
	//  CPU の期待値と比べる。許容誤差 1e-5、rcp に依存する項目 (†) は 2e-3
	// ============================================================
	void TestGPU(FSelfTestReport& R, FSceneRenderer& Renderer)
	{
		struct FEntry { int Index; const char* What; double Expected; double Tol; };
		static const FEntry kEntries[] =
		{
			{  0, "EncodeVelocity(-0.01357995,0).x", 0.49660417, 1e-5 },
			{  1, "EncodeVelocity(-0.01357995,0).y", 0.49999237, 1e-5 },
			{  2, "DecodeVelocity(32545/65535).x", -0.0135772, 1e-5 },
			{  3, "DecodeVelocity(32767/65535).y", 0.0, 1e-5 },
			{  4, "EncodeVelocity(3,0).x (clamp)", 0.99899237, 1e-5 },
			{  5, "ComputeSampleWeigth((0.5,0),1)", 0.5815625, 1e-5 },
			{  6, "ComputeSampleWeigth((0,-1/12),2)", 0.9479205, 1e-5 },
			{  7, "RGBToYCoCg.Y", 2.25, 1e-5 },
			{  8, "RGBToYCoCg.Co", 1.5, 1e-5 },
			{  9, "RGBToYCoCg.Cg", -0.25, 1e-5 },
			{ 10, "YCoCgToRGB.r", 1.0, 1e-5 },
			{ 11, "YCoCgToRGB.g", 0.5, 1e-5 },
			{ 12, "YCoCgToRGB.b", 0.25, 1e-5 },
			{ 13, "HdrWeightY(2.25,1) (rcp)", 0.16, 2e-3 },
			{ 14, "WeightedLerpFactors.x (rcp)", 0.9756098, 2e-3 },
			{ 15, "WeightedLerpFactors.y (rcp)", 0.0243902, 2e-3 },
			{ 16, "ViewZToDeviceZ(10) (rcp)", 0.99019804, 2e-3 },
			{ 17, "ViewZToDeviceZ(500) far", 1.00020004, 1e-5 },
			{ 18, "ViewZToDeviceZ(499) (rcp)", 0.99999960, 2e-3 },
			{ 19, "ClosestDepth(5,3,4,6).x", 2.0, 1e-5 },
			{ 20, "ClosestDepth(5,3,4,6).y", -2.0, 1e-5 },
			{ 21, "ClosestDepth(5,3,4,6).z", 3.0, 1e-5 },
			{ 22, "ClosestDepth(9,2,8,1).x", 2.0, 1e-5 },
			{ 23, "ClosestDepth(9,2,8,1).y", 2.0, 1e-5 },
			{ 24, "ClosestDepth(9,2,8,1).z", 1.0, 1e-5 },
			{ 25, "ClosestDepth(9,9,9,9).x", 0.0, 1e-5 },
			{ 26, "ClosestDepth(9,9,9,9).y", 0.0, 1e-5 },
			{ 27, "ClosestDepth(9,9,9,9).z", 7.0, 1e-5 },
			{ 28, "CatmullRom5Taps TapW[2]", 1.1962891, 1e-5 },
			{ 29, "CatmullRom5Taps TapUV[2].x*100-10.5", 0.2071429, 1e-5 },
			{ 30, "MitchellNetravali(0.5)", 0.5347222, 1e-5 },
			{ 31, "MitchellNetravali(1.5)", -0.0347222, 1e-5 },
			{ 32, "Quantize(1, 0.5, 1/64)", 1.0078125, 1e-5 },
			{ 33, "Quantize(1, 0.5, 1/1024)", 1.00048828, 1e-5 },
			{ 34, "Hammersley16(Rand3DPCG16(0,0,0)).x", 0.10681152, 1e-5 },
			{ 35, "Hammersley16(Rand3DPCG16(10,20,3)).x", 0.78860474, 1e-5 },
			{ 36, "InterleavedGradientNoise((1,0),0)", 0.5557134, 1e-5 },
			{ 37, "IsVelocityWritten(0)", 0.0, 0.0 },
			{ 38, "IsVelocityWritten(1/65535)", 1.0, 0.0 },
		};

		R.BeginTest("GPU");
		FDefaultTemporalUpscaler* upscaler = Renderer.GetTemporalUpscaler();
		std::vector<float> values;
		const bool bRan = upscaler && upscaler->HasSelfTestPSO() && upscaler->RunGPUSelfTest(values);
		R.CheckBool("dispatch + readback (TemporalAASelfTest_CS)", bRan, true);
		if (bRan)
		{
			for (const FEntry& e : kEntries)
			{
				const double got = (e.Index < (int)values.size()) ? (double)values[(size_t)e.Index] : std::nan("");
				R.Check(Fmt("[%d] %s", e.Index, e.What), got, e.Expected, e.Tol);
			}
			// 未使用の添字は 0 のまま
			double unused = 0.0;
			for (size_t i = 39; i < values.size(); ++i) unused = (std::max)(unused, std::fabs((double)values[i]));
			R.Check("[39..63] unused = 0", unused, 0.0, 0.0);
		}
		R.EndTest();
	}
}


int RunTemporalAASelfTests(FSceneRenderer& Renderer, bool bIncludeGPU)
{
	FSelfTestReport report(Renderer);
	report.Line(bIncludeGPU ? "begin (CPU: T1 T2 T3 T4 T4b T5 T6 T7 T8 T9 T10 T11 T12 T13 T14 T15 T16, GPU parity)"
	                        : "begin (CPU: T1 T2 T3 T4 T4b T5 T6 T7 T8 T9 T10 T11 T12 T13 T14 T15 T16)");

	TestT1(report);
	TestT2(report);
	TestT3(report);
	TestT4(report);
	TestT4b(report);
	TestT5(report);
	TestT6(report);
	TestT7(report);
	TestT8(report);
	TestT9(report);
	TestT10(report);
	TestT11(report);
	TestT12(report);
	TestT13(report);
	TestT14(report);
	TestT15(report);
	TestT16(report);

	if (bIncludeGPU)
	{
		// GPU パリティ (TemporalAASelfTest_CS)。BeginFrame 先頭からのみ (FlushAndResetCommandList を使う)
		TestGPU(report, Renderer);
	}

	char summary[64];
	sprintf_s(summary, "%d/%d PASS", report.GetNumTests() - report.GetNumFailedTests(), report.GetNumTests());
	report.Line(summary);

	return report.GetNumFailedTests();
}
