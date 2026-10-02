// ============================================================
//  LTCFit
//  レクトライトのスペキュラ (RectGGXApproxLTC) が引く LTC
//  (Linearly Transformed Cosines) テーブルの生成ツール。
//
//  [Heitz et al. 2016, "Real-Time Polygonal-Light Shading with
//   Linearly Transformed Cosines"] の公開フィット手順 (fitLTC) に従い、
//  GGX (Smith 高さ相関) の BRDF x cos を LTC で近似する。
//    - 64 x 64。列 = ラフネス (a / 63)、行 = sqrt(1 - cos(theta_v)) (t / 63)
//    - セルごとに Nelder-Mead で (m11, m22, m13) を当てはめる
//      (誤差 = |BRDF - LTC|^3 を LTC / BRDF の両方で重点サンプルした MIS 積分)
//  UE の LTC.h (LTCMat / LTCAmp) と同じ手順・同じパラメータ化なので、
//  シェーダ (UV = (Roughness, sqrt(1 - NoV))) はそのまま使える。
//
//  出力 (LTC.cpp):
//    LTCMat[y * 64 + x][4] : 逆行列 M^-1 を中央要素で正規化した 4 成分
//                            (r0c0, r2c0, r0c2, r2c2)
//    LTCAmp[y * 64 + x][2] : (大きさ = F0 が 1 のときの方向アルベド, フレネル項)
//
//  ビルド (x64 Native Tools):  cl /O2 /EHsc /std:c++17 /utf-8 /D_CRT_SECURE_NO_WARNINGS LTCFit.cpp (build.bat)
//  実行:                       LTCFit.exe <出力先 LTC.cpp> [--verify]
//  エンジン本体のビルドには含めない。
// ============================================================

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

namespace
{
	const double kPi = 3.14159265358979323846;
	const int    kSize = 64;		// テーブルの一辺
	const int    kNumSamples = 32;	// 1 軸あたりのサンプル数 (32 x 32)
	const double kMinAlpha = 0.00001;

	struct vec3
	{
		double x = 0.0, y = 0.0, z = 0.0;
		vec3() = default;
		vec3(double X, double Y, double Z) : x(X), y(Y), z(Z) {}
	};

	vec3 operator+(const vec3& a, const vec3& b) { return vec3(a.x + b.x, a.y + b.y, a.z + b.z); }
	vec3 operator-(const vec3& a, const vec3& b) { return vec3(a.x - b.x, a.y - b.y, a.z - b.z); }
	vec3 operator*(const vec3& a, double s) { return vec3(a.x * s, a.y * s, a.z * s); }
	vec3 operator*(double s, const vec3& a) { return a * s; }
	double dot(const vec3& a, const vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
	vec3 cross(const vec3& a, const vec3& b) { return vec3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x); }
	double length(const vec3& a) { return std::sqrt(dot(a, a)); }
	vec3 normalize(const vec3& a) { const double l = length(a); return (l > 0.0) ? a * (1.0 / l) : a; }

	// 行優先の 3x3 (m[行][列])。ベクトルは列ベクトルとして右から掛ける
	struct mat3
	{
		double m[3][3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } };
	};

	vec3 mul(const mat3& M, const vec3& v)
	{
		return vec3(
			M.m[0][0] * v.x + M.m[0][1] * v.y + M.m[0][2] * v.z,
			M.m[1][0] * v.x + M.m[1][1] * v.y + M.m[1][2] * v.z,
			M.m[2][0] * v.x + M.m[2][1] * v.y + M.m[2][2] * v.z);
	}

	mat3 mul(const mat3& A, const mat3& B)
	{
		mat3 R;
		for (int r = 0; r < 3; ++r)
			for (int c = 0; c < 3; ++c)
				R.m[r][c] = A.m[r][0] * B.m[0][c] + A.m[r][1] * B.m[1][c] + A.m[r][2] * B.m[2][c];
		return R;
	}

	double determinant(const mat3& M)
	{
		return M.m[0][0] * (M.m[1][1] * M.m[2][2] - M.m[1][2] * M.m[2][1])
			- M.m[0][1] * (M.m[1][0] * M.m[2][2] - M.m[1][2] * M.m[2][0])
			+ M.m[0][2] * (M.m[1][0] * M.m[2][1] - M.m[1][1] * M.m[2][0]);
	}

	mat3 inverse(const mat3& M)
	{
		const double d = 1.0 / determinant(M);
		mat3 R;
		R.m[0][0] = (M.m[1][1] * M.m[2][2] - M.m[1][2] * M.m[2][1]) * d;
		R.m[0][1] = (M.m[0][2] * M.m[2][1] - M.m[0][1] * M.m[2][2]) * d;
		R.m[0][2] = (M.m[0][1] * M.m[1][2] - M.m[0][2] * M.m[1][1]) * d;
		R.m[1][0] = (M.m[1][2] * M.m[2][0] - M.m[1][0] * M.m[2][2]) * d;
		R.m[1][1] = (M.m[0][0] * M.m[2][2] - M.m[0][2] * M.m[2][0]) * d;
		R.m[1][2] = (M.m[0][2] * M.m[1][0] - M.m[0][0] * M.m[1][2]) * d;
		R.m[2][0] = (M.m[1][0] * M.m[2][1] - M.m[1][1] * M.m[2][0]) * d;
		R.m[2][1] = (M.m[0][1] * M.m[2][0] - M.m[0][0] * M.m[2][1]) * d;
		R.m[2][2] = (M.m[0][0] * M.m[1][1] - M.m[0][1] * M.m[1][0]) * d;
		return R;
	}

	// ------------------------------------------------------------
	//  GGX (Smith 高さ相関)。eval は BRDF x cos(theta_l) を返す (フレネル項は 1)
	// ------------------------------------------------------------
	struct BrdfGGX
	{
		static double lambda(double alpha, double cosTheta)
		{
			if (cosTheta >= 1.0)
			{
				return 0.0;
			}
			const double a = 1.0 / alpha / std::tan(std::acos(cosTheta));
			return 0.5 * (-1.0 + std::sqrt(1.0 + 1.0 / a / a));
		}

		static double eval(const vec3& V, const vec3& L, double alpha, double& pdf)
		{
			if (V.z <= 0.0)
			{
				pdf = 0.0;
				return 0.0;
			}

			// マスキング / シャドウイング
			const double LambdaV = lambda(alpha, V.z);
			double G2 = 0.0;
			if (L.z > 0.0)
			{
				const double LambdaL = lambda(alpha, L.z);
				G2 = 1.0 / (1.0 + LambdaV + LambdaL);
			}

			// 法線分布
			const vec3 H = normalize(V + L);
			const double slopex = H.x / H.z;
			const double slopey = H.y / H.z;
			double D = 1.0 / (1.0 + (slopex * slopex + slopey * slopey) / alpha / alpha);
			D = D * D;
			D = D / (kPi * alpha * alpha * H.z * H.z * H.z * H.z);

			pdf = std::fabs(D * H.z / 4.0 / dot(V, H));
			return D * G2 / 4.0 / V.z;
		}

		static vec3 sample(const vec3& V, double alpha, double U1, double U2)
		{
			const double phi = 2.0 * kPi * U1;
			const double r = alpha * std::sqrt(U2 / (1.0 - U2));
			const vec3 N = normalize(vec3(r * std::cos(phi), r * std::sin(phi), 1.0));
			return (-1.0 * V) + 2.0 * N * dot(N, V);
		}
	};

	// ------------------------------------------------------------
	//  LTC: クランプコサイン分布を行列 M で変換した分布
	//    M = [X Y Z] * [[m11, 0, m13], [0, m22, 0], [0, 0, 1]]
	// ------------------------------------------------------------
	struct LTC
	{
		double magnitude = 1.0;	// 方向アルベド (F0 = 1)
		double fresnel = 1.0;	// フレネル項 (Schlick の (1 - VoH)^5 で重み付けした積分)

		double m11 = 1.0, m22 = 1.0, m13 = 0.0;
		vec3 X = vec3(1, 0, 0), Y = vec3(0, 1, 0), Z = vec3(0, 0, 1);

		mat3 M, invM;
		double detM = 1.0;

		LTC() { update(); }

		void update()
		{
			mat3 B;	// 列 = X, Y, Z
			B.m[0][0] = X.x; B.m[0][1] = Y.x; B.m[0][2] = Z.x;
			B.m[1][0] = X.y; B.m[1][1] = Y.y; B.m[1][2] = Z.y;
			B.m[2][0] = X.z; B.m[2][1] = Y.z; B.m[2][2] = Z.z;

			mat3 A;
			A.m[0][0] = m11; A.m[0][1] = 0.0; A.m[0][2] = m13;
			A.m[1][0] = 0.0; A.m[1][1] = m22; A.m[1][2] = 0.0;
			A.m[2][0] = 0.0; A.m[2][1] = 0.0; A.m[2][2] = 1.0;

			M = mul(B, A);
			invM = inverse(M);
			detM = std::fabs(determinant(M));
		}

		double eval(const vec3& L) const
		{
			const vec3 Loriginal = normalize(mul(invM, L));
			const vec3 L_ = mul(M, Loriginal);

			const double l = length(L_);
			const double Jacobian = detM / (l * l * l);

			const double D = 1.0 / kPi * std::max(0.0, Loriginal.z);
			return magnitude * D / Jacobian;
		}

		vec3 sample(double U1, double U2) const
		{
			const double theta = std::acos(std::sqrt(U1));
			const double phi = 2.0 * kPi * U2;
			return normalize(mul(M, vec3(std::sin(theta) * std::cos(phi), std::sin(theta) * std::sin(phi), std::cos(theta))));
		}
	};

	// 方向アルベド / フレネル項 / 平均方向 (BRDF の重点サンプル)
	void computeAvgTerms(const vec3& V, double alpha, double& norm, double& fresnel, vec3& averageDir)
	{
		norm = 0.0;
		fresnel = 0.0;
		averageDir = vec3(0, 0, 0);

		for (int j = 0; j < kNumSamples; ++j)
			for (int i = 0; i < kNumSamples; ++i)
			{
				const double U1 = (i + 0.5) / kNumSamples;
				const double U2 = (j + 0.5) / kNumSamples;

				const vec3 L = BrdfGGX::sample(V, alpha, U1, U2);

				double pdf;
				const double eval = BrdfGGX::eval(V, L, alpha, pdf);

				if (pdf > 0.0)
				{
					const double weight = eval / pdf;
					const vec3 H = normalize(V + L);

					norm += weight;
					fresnel += weight * std::pow(1.0 - std::max(dot(V, H), 0.0), 5.0);
					averageDir = averageDir + L * weight;
				}
			}

		norm /= (double)(kNumSamples * kNumSamples);
		fresnel /= (double)(kNumSamples * kNumSamples);

		// 等方 BRDF では y 成分は 0 のはず
		averageDir.y = 0.0;
		averageDir = normalize(averageDir);
	}

	// |BRDF - LTC|^3 の MIS 積分
	double computeError(const LTC& ltc, const vec3& V, double alpha)
	{
		double error = 0.0;

		for (int j = 0; j < kNumSamples; ++j)
			for (int i = 0; i < kNumSamples; ++i)
			{
				const double U1 = (i + 0.5) / kNumSamples;
				const double U2 = (j + 0.5) / kNumSamples;

				// LTC を重点サンプル
				{
					const vec3 L = ltc.sample(U1, U2);

					double pdf_brdf;
					const double eval_brdf = BrdfGGX::eval(V, L, alpha, pdf_brdf);
					const double eval_ltc = ltc.eval(L);
					const double pdf_ltc = eval_ltc / ltc.magnitude;

					double e = std::fabs(eval_brdf - eval_ltc);
					e = e * e * e;
					const double denom = pdf_ltc + pdf_brdf;
					if (denom > 0.0)
					{
						error += e / denom;
					}
				}

				// BRDF を重点サンプル
				{
					const vec3 L = BrdfGGX::sample(V, alpha, U1, U2);

					double pdf_brdf;
					const double eval_brdf = BrdfGGX::eval(V, L, alpha, pdf_brdf);
					const double eval_ltc = ltc.eval(L);
					const double pdf_ltc = eval_ltc / ltc.magnitude;

					double e = std::fabs(eval_brdf - eval_ltc);
					e = e * e * e;
					const double denom = pdf_ltc + pdf_brdf;
					if (denom > 0.0)
					{
						error += e / denom;
					}
				}
			}

		return error / (double)(kNumSamples * kNumSamples);
	}

	struct FitLTC
	{
		LTC& ltc;
		bool isotropic;
		const vec3& V;
		double alpha;

		void update(const double* params)
		{
			const double m11 = std::max(params[0], 1e-7);
			const double m22 = std::max(params[1], 1e-7);
			const double m13 = params[2];

			if (isotropic)
			{
				ltc.m11 = m11;
				ltc.m22 = m11;
				ltc.m13 = 0.0;
			}
			else
			{
				ltc.m11 = m11;
				ltc.m22 = m22;
				ltc.m13 = m13;
			}
			ltc.update();
		}

		double operator()(const double* params)
		{
			update(params);
			return computeError(ltc, V, alpha);
		}
	};

	// Nelder-Mead (滑降シンプレックス法)。3 次元
	template <class FUNC>
	double NelderMead3(double* pmin, const double* start, double delta, double tolerance, int maxIters, FUNC& objectiveFn)
	{
		const int DIM = 3;
		const int NB_POINTS = DIM + 1;
		const double reflect = 1.0, expand = 2.0, contract = 0.5, shrink = 0.5;

		double s[NB_POINTS][DIM];
		double f[NB_POINTS];

		// シンプレックスの初期化
		for (int i = 0; i < NB_POINTS; ++i)
		{
			std::memcpy(s[i], start, sizeof(double) * DIM);
			if (i > 0)
			{
				s[i][i - 1] += delta;
			}
		}
		for (int i = 0; i < NB_POINTS; ++i)
		{
			f[i] = objectiveFn(s[i]);
		}

		int lo = 0, hi, nh;

		for (int j = 0; j < maxIters; ++j)
		{
			// 最良 / 最悪 / 2 番目に悪い点
			lo = hi = nh = 0;
			for (int i = 1; i < NB_POINTS; ++i)
			{
				if (f[i] < f[lo]) lo = i;
				if (f[i] > f[hi]) { nh = hi; hi = i; }
				else if (f[i] > f[nh]) nh = i;
			}

			// 収束判定
			const double a = std::fabs(f[lo]);
			const double b = std::fabs(f[hi]);
			if (2.0 * std::fabs(a - b) < (a + b) * tolerance)
			{
				break;
			}

			// 最悪点を除いた重心
			double o[DIM] = { 0.0, 0.0, 0.0 };
			for (int i = 0; i < NB_POINTS; ++i)
			{
				if (i == hi) continue;
				for (int k = 0; k < DIM; ++k) o[k] += s[i][k];
			}
			for (int k = 0; k < DIM; ++k) o[k] /= DIM;

			// 反射
			double r[DIM];
			for (int k = 0; k < DIM; ++k) r[k] = o[k] + reflect * (o[k] - s[hi][k]);
			const double fr = objectiveFn(r);
			if (fr < f[nh])
			{
				if (fr < f[lo])
				{
					// 拡張
					double e[DIM];
					for (int k = 0; k < DIM; ++k) e[k] = o[k] + expand * (o[k] - s[hi][k]);
					const double fe = objectiveFn(e);
					if (fe < fr)
					{
						std::memcpy(s[hi], e, sizeof(e));
						f[hi] = fe;
						continue;
					}
				}

				std::memcpy(s[hi], r, sizeof(r));
				f[hi] = fr;
				continue;
			}

			// 収縮
			double c[DIM];
			for (int k = 0; k < DIM; ++k) c[k] = o[k] - contract * (o[k] - s[hi][k]);
			const double fc = objectiveFn(c);
			if (fc < f[hi])
			{
				std::memcpy(s[hi], c, sizeof(c));
				f[hi] = fc;
				continue;
			}

			// 縮小
			for (int k = 0; k < NB_POINTS; ++k)
			{
				if (k == lo) continue;
				for (int i = 0; i < DIM; ++i) s[k][i] = s[lo][i] + shrink * (s[k][i] - s[lo][i]);
				f[k] = objectiveFn(s[k]);
			}
		}

		std::memcpy(pmin, s[lo], sizeof(double) * DIM);
		return f[lo];
	}

	void fit(LTC& ltc, const vec3& V, double alpha, double epsilon, bool isotropic)
	{
		const double startFit[3] = { ltc.m11, ltc.m22, ltc.m13 };
		double resultFit[3];

		FitLTC fitter{ ltc, isotropic, V, alpha };
		NelderMead3(resultFit, startFit, epsilon, 1e-5, 100, fitter);
		fitter.update(resultFit);
	}

	vec3 viewVector(int t)
	{
		// sqrt(1 - cos(theta)) でパラメータ化
		const double x = t / double(kSize - 1);
		const double ct = 1.0 - x * x;
		const double theta = std::min(1.57, std::acos(ct));
		return vec3(std::sin(theta), 0.0, std::cos(theta));
	}

	double alphaOf(int a)
	{
		const double roughness = a / double(kSize - 1);
		return std::max(roughness * roughness, kMinAlpha);
	}

	struct FTables
	{
		std::vector<mat3>   M;			// [a + t * kSize]
		std::vector<double> Magnitude;
		std::vector<double> Fresnel;
		std::vector<double> m11, m22;	// t = 0 の当てはめ値 (次の a の初期値)
	};

	// 1 セルを当てはめて表へ書く。ltc は (m11, m22, m13) を前のセルから引き継ぐ
	void fitCell(FTables& T, LTC& ltc, int a, int t)
	{
		const vec3 V = viewVector(t);
		const double alpha = alphaOf(a);

		vec3 averageDir;
		computeAvgTerms(V, alpha, ltc.magnitude, ltc.fresnel, averageDir);

		bool isotropic;
		if (t == 0)
		{
			// theta = 0 のローブは回転対称で Z に揃う
			ltc.X = vec3(1, 0, 0);
			ltc.Y = vec3(0, 1, 0);
			ltc.Z = vec3(0, 0, 1);

			if (a == kSize - 1)
			{
				ltc.m11 = 1.0;
				ltc.m22 = 1.0;
			}
			else
			{
				// ひとつ粗いラフネスの結果を初期値にする
				ltc.m11 = T.m11[a + 1];
				ltc.m22 = T.m22[a + 1];
			}

			ltc.m13 = 0.0;
			ltc.update();
			isotropic = true;
		}
		else
		{
			// 平均方向に揃えた基底で、前のセルの形を初期値にする
			const vec3 L = averageDir;
			ltc.X = vec3(L.z, 0, -L.x);
			ltc.Y = vec3(0, 1, 0);
			ltc.Z = L;
			ltc.update();
			isotropic = false;
		}

		fit(ltc, V, alpha, 0.05, isotropic);

		const int index = a + t * kSize;
		T.M[index] = ltc.M;
		T.Magnitude[index] = ltc.magnitude;
		T.Fresnel[index] = ltc.fresnel;

		// 対称性から 0 になる成分は 0 にする
		T.M[index].m[1][0] = 0.0;
		T.M[index].m[0][1] = 0.0;
		T.M[index].m[1][2] = 0.0;
		T.M[index].m[2][1] = 0.0;

		if (t == 0)
		{
			T.m11[a] = ltc.m11;
			T.m22[a] = ltc.m22;
		}
	}

	void fitTab(FTables& T)
	{
		T.M.resize(kSize * kSize);
		T.Magnitude.resize(kSize * kSize);
		T.Fresnel.resize(kSize * kSize);
		T.m11.resize(kSize);
		T.m22.resize(kSize);

		// 行 (ラフネス a) ごとの LTC の状態。t = 0 の列は粗い方から順に当てはめる
		// (a の初期値が a + 1 の結果に依存するため)
		std::vector<LTC> rowState(kSize);
		for (int a = kSize - 1; a >= 0; --a)
		{
			fitCell(T, rowState[a], a, 0);
		}
		std::printf("theta = 0 column done\n");

		// 残り (t >= 1) は行ごとに独立なので並列に当てはめる
		std::atomic<int> next(0);
		const unsigned int numThreads = std::max(1u, std::thread::hardware_concurrency());
		std::vector<std::thread> threads;
		for (unsigned int i = 0; i < numThreads; ++i)
		{
			threads.emplace_back([&]()
				{
					for (;;)
					{
						const int a = next.fetch_add(1);
						if (a >= kSize)
						{
							return;
						}
						for (int t = 1; t < kSize; ++t)
						{
							fitCell(T, rowState[a], a, t);
						}
					}
				});
		}
		for (std::thread& th : threads)
		{
			th.join();
		}
		std::printf("fit done (%u threads)\n", numThreads);
	}

	// 逆行列を中央要素で正規化した 4 成分 (r0c0, r2c0, r0c2, r2c2)
	void packInverse(const mat3& M, double out[4])
	{
		mat3 invM = inverse(M);
		const double s = 1.0 / invM.m[1][1];
		out[0] = invM.m[0][0] * s;
		out[1] = invM.m[2][0] * s;
		out[2] = invM.m[0][2] * s;
		out[3] = invM.m[2][2] * s;
	}

	// C++ の float リテラルとして書く (小数点か指数を必ず含める。非正規化数は 0 にする)
	void printFloat(FILE* f, double value)
	{
		float v = (float)value;
		if (std::fabs(v) < 1.0e-30f)
		{
			v = 0.0f;
		}

		char buf[64];
		std::snprintf(buf, sizeof(buf), "%.9g", v);
		if (std::strchr(buf, '.') == nullptr && std::strchr(buf, 'e') == nullptr)
		{
			std::strcat(buf, ".0");
		}
		std::fprintf(f, "%sf", buf);
	}

	bool writeTables(const FTables& T, const char* path)
	{
		FILE* f = std::fopen(path, "wb");
		if (f == nullptr)
		{
			return false;
		}

		std::fprintf(f, "// ============================================================\r\n");
		std::fprintf(f, "//  LTC テーブル (GGX)。Tools/LTCFit/LTCFit.cpp が生成した。\r\n");
		std::fprintf(f, "//  手で編集しないこと。レイアウトは LTC.h を参照。\r\n");
		std::fprintf(f, "// ============================================================\r\n");
		std::fprintf(f, "#include \"LTC.h\"\r\n\r\n");

		std::fprintf(f, "const float LTCMat[LTC_Size * LTC_Size][4] =\r\n{\r\n");
		for (int i = 0; i < kSize * kSize; ++i)
		{
			double p[4];
			packInverse(T.M[i], p);
			std::fprintf(f, "\t{ ");
			for (int k = 0; k < 4; ++k)
			{
				printFloat(f, p[k]);
				std::fprintf(f, (k < 3) ? ", " : " },\r\n");
			}
		}
		std::fprintf(f, "};\r\n\r\n");

		std::fprintf(f, "const float LTCAmp[LTC_Size * LTC_Size][2] =\r\n{\r\n");
		for (int i = 0; i < kSize * kSize; ++i)
		{
			std::fprintf(f, "\t{ ");
			printFloat(f, T.Magnitude[i]);
			std::fprintf(f, ", ");
			printFloat(f, T.Fresnel[i]);
			std::fprintf(f, " },\r\n");
		}
		std::fprintf(f, "};\r\n");

		std::fclose(f);
		return true;
	}

	// ------------------------------------------------------------
	//  検証: シェーダ (RectGGXApproxLTC) と同じ手順を C++ で行い、
	//  矩形を数値積分した GGX の真値と比べる
	// ------------------------------------------------------------

	// シェーダの SphereHorizonCosWrap (Hermite 近似)
	double SphereHorizonCosWrap(double NoL, double SinAlphaSqr)
	{
		const double SinAlpha = std::sqrt(SinAlphaSqr);
		if (NoL < SinAlpha)
		{
			NoL = std::max(NoL, -SinAlpha);
			NoL = (SinAlpha + NoL) * (SinAlpha + NoL) / (4.0 * SinAlpha);
		}
		return NoL;
	}

	// シェーダの PolygonIrradiance (辺の角度 / sin の近似式込み)
	vec3 PolygonIrradiance(const vec3 Poly[4])
	{
		const vec3 L0 = normalize(Poly[0]);
		const vec3 L1 = normalize(Poly[1]);
		const vec3 L2 = normalize(Poly[2]);
		const vec3 L3 = normalize(Poly[3]);

		const double c01 = dot(L0, L1), c12 = dot(L1, L2), c23 = dot(L2, L3), c30 = dot(L3, L0);

		const double w01 = (1.5708 - 0.175 * c01) / std::sqrt(c01 + 1.0);
		const double w12 = (1.5708 - 0.175 * c12) / std::sqrt(c12 + 1.0);
		const double w23 = (1.5708 - 0.175 * c23) / std::sqrt(c23 + 1.0);
		const double w30 = (1.5708 - 0.175 * c30) / std::sqrt(c30 + 1.0);

		vec3 L = cross(L1, (L0 * -w01) + (L2 * w12));
		L = L + cross(L3, (L0 * w30) + (L2 * -w23));

		// ベクトル放射照度 (放射輝度 1 の多角形が作る放射照度。大きさは π x フォームファクタ)
		return L * 0.5;
	}

	// テーブルをバイリニアで引く (UV はシェーダと同じ)
	void sampleTables(const FTables& T, double Roughness, double NoV, double mat[4], double amp[2])
	{
		const double u = Roughness * (kSize - 1);
		const double v = std::sqrt(1.0 - NoV) * (kSize - 1);
		const int x0 = std::min((int)u, kSize - 1), y0 = std::min((int)v, kSize - 1);
		const int x1 = std::min(x0 + 1, kSize - 1), y1 = std::min(y0 + 1, kSize - 1);
		const double fx = u - x0, fy = v - y0;

		auto fetch = [&](int x, int y, double m[4], double a[2])
			{
				packInverse(T.M[x + y * kSize], m);
				a[0] = T.Magnitude[x + y * kSize];
				a[1] = T.Fresnel[x + y * kSize];
			};

		double m00[4], m10[4], m01[4], m11[4], a00[2], a10[2], a01[2], a11[2];
		fetch(x0, y0, m00, a00); fetch(x1, y0, m10, a10); fetch(x0, y1, m01, a01); fetch(x1, y1, m11, a11);
		for (int i = 0; i < 4; ++i)
			mat[i] = (m00[i] * (1 - fx) + m10[i] * fx) * (1 - fy) + (m01[i] * (1 - fx) + m11[i] * fx) * fy;
		for (int i = 0; i < 2; ++i)
			amp[i] = (a00[i] * (1 - fx) + a10[i] * fx) * (1 - fy) + (a01[i] * (1 - fx) + a11[i] * fx) * fy;
	}

	// RectGGXApproxLTC と同じ計算 (F0 = 1、ライトの放射輝度 = 1)。接空間 (N = +Z) で評価する
	double evalLTCRect(const FTables& T, double Roughness, const vec3& V, const vec3 RectVerts[4])
	{
		const double NoV = std::min(std::fabs(V.z) + 1e-5, 1.0);

		double mat[4], amp[2];
		sampleTables(T, Roughness, NoV, mat, amp);

		// 接基底 (T1 = V の接平面成分)
		const vec3 N(0, 0, 1);
		vec3 T1 = V - N * dot(N, V);
		T1 = (dot(T1, T1) > 1e-12) ? normalize(T1) : vec3(1, 0, 0);
		const vec3 T2 = cross(N, T1);

		vec3 Poly[4];
		for (int i = 0; i < 4; ++i)
		{
			// ワールド -> 接空間 -> LTC の逆変換
			const vec3 p(dot(T1, RectVerts[i]), dot(T2, RectVerts[i]), dot(N, RectVerts[i]));
			Poly[i] = vec3(mat[0] * p.x + mat[2] * p.z, p.y, mat[1] * p.x + mat[3] * p.z);
		}

		vec3 L = PolygonIrradiance(Poly);
		const double Length = length(L);
		if (Length <= 0.0)
		{
			return 0.0;
		}
		L = L * (1.0 / Length);

		// 放射照度 / π = フォームファクタ (等価な球の sin^2)
		const double SinAlphaSqr = Length / kPi;
		const double NoL = SphereHorizonCosWrap(L.z, SinAlphaSqr);
		const double Irradiance = std::max(SinAlphaSqr * NoL, 0.0);

		// F0 = 1: LTCAmp.y + (LTCAmp.x - LTCAmp.y) * 1 = LTCAmp.x
		return Irradiance * amp[0];
	}

	// 矩形の面積分による GGX の真値 (F = 1、放射輝度 = 1):
	//   ∫ f_r cos(theta_l) dω  (eval は f_r * cos を返す)
	// シェーダの結果 (LightColor * Irradiance * SpecularColor) と同じ量
	double evalReferenceRect(double Roughness, const vec3& V, const vec3& Center, const vec3& AxisX, const vec3& AxisY, double HalfX, double HalfY)
	{
		const double alpha = std::max(Roughness * Roughness, kMinAlpha);
		const int n = 400;
		const vec3 normal = normalize(cross(AxisX, AxisY));

		double sum = 0.0;
		for (int j = 0; j < n; ++j)
			for (int i = 0; i < n; ++i)
			{
				const double sx = ((i + 0.5) / n * 2.0 - 1.0) * HalfX;
				const double sy = ((j + 0.5) / n * 2.0 - 1.0) * HalfY;
				const vec3 P = Center + AxisX * sx + AxisY * sy;
				const double d2 = dot(P, P);
				const vec3 L = P * (1.0 / std::sqrt(d2));
				if (L.z <= 0.0)
				{
					continue;
				}

				double pdf;
				const double brdfCos = BrdfGGX::eval(V, L, alpha, pdf);
				const double dA = (2.0 * HalfX / n) * (2.0 * HalfY / n);
				const double cosLight = std::fabs(dot(normal, L));
				sum += brdfCos * cosLight * dA / d2;
			}

		return sum;
	}

	void verify(const FTables& T)
	{
		std::printf("\nverify: LTC (shader path) vs numerical GGX integration\n");
		std::printf("%9s %9s %-22s %12s %12s %8s\n", "roughness", "theta_v", "rect", "LTC", "reference", "ratio");

		// 矩形は鏡面反射方向 R を基準に置く (ローブの本体を積分するため)。
		//   center  : R 上の距離 2 m、受光点を向く、R から見て ±20 度
		//   toward N: 同じ矩形を法線側へ半分ずらす (矩形の縁が R を通る)
		//   away N  : 接平面側へ半分ずらす
		//   offside : 入射面の横 (T2 方向) へ半分ずらす
		// 向きを取り違えていれば toward / away / offside の比が大きく崩れる
		struct FCase { const char* Name; double ShiftAlong; double ShiftSide; };
		const FCase cases[] =
		{
			{ "center", 0.0, 0.0 },
			{ "toward N", 1.0, 0.0 },
			{ "away N", -1.0, 0.0 },
			{ "offside", 0.0, 1.0 },
		};

		const double roughnesses[] = { 0.15, 0.3, 0.5, 0.8, 1.0 };
		const double thetas[] = { 0.0, 30.0, 60.0, 75.0 };

		double maxRelErr = 0.0;
		for (const FCase& cs : cases)
			for (double r : roughnesses)
				for (double thDeg : thetas)
				{
					const double th = thDeg * kPi / 180.0;
					const vec3 V(std::sin(th), 0.0, std::cos(th));

					// 鏡面反射方向と、それに直交する矩形の軸
					const vec3 R(-V.x, -V.y, V.z);
					const vec3 Side(0, 1, 0);							// 入射面に垂直
					const vec3 Along = normalize(cross(Side, R));		// 入射面内 (法線側が正になるよう下で調整)
					const vec3 AlongN = (Along.z >= 0.0) ? Along : (Along * -1.0);
					const double Dist = 2.0;
					const double Half = Dist * std::tan(20.0 * kPi / 180.0);

					struct { const char* Name; vec3 Center; vec3 AxisX; vec3 AxisY; double HalfX; double HalfY; } c;
					c.Name = cs.Name;
					c.AxisX = AlongN;
					c.AxisY = Side;
					c.HalfX = Half;
					c.HalfY = Half;
					c.Center = R * Dist + AlongN * (cs.ShiftAlong * Half) + Side * (cs.ShiftSide * Half);

					vec3 verts[4];
					verts[0] = c.Center - c.AxisX * c.HalfX - c.AxisY * c.HalfY;
					verts[1] = c.Center + c.AxisX * c.HalfX - c.AxisY * c.HalfY;
					verts[2] = c.Center + c.AxisX * c.HalfX + c.AxisY * c.HalfY;
					verts[3] = c.Center - c.AxisX * c.HalfX + c.AxisY * c.HalfY;

					// 多角形の向きは受光点から見て反時計回りに揃える (ベクトル放射照度が光源側を向く)
					{
						vec3 chk = PolygonIrradiance(verts);
						if (dot(chk, c.Center) < 0.0)
						{
							std::swap(verts[1], verts[3]);
						}
					}

					const double ltc = evalLTCRect(T, r, V, verts);
					const double ref = evalReferenceRect(r, V, c.Center, c.AxisX, c.AxisY, c.HalfX, c.HalfY);
					const double ratio = (ref > 1e-9) ? ltc / ref : 0.0;
					if (ref > 1e-3)
					{
						maxRelErr = std::max(maxRelErr, std::fabs(ratio - 1.0));
					}
					std::printf("%9.2f %9.1f %-22s %12.6f %12.6f %8.3f\n", r, thDeg, c.Name, ltc, ref, ratio);
				}

		std::printf("max relative error (reference > 1e-3): %.1f %%\n", maxRelErr * 100.0);
	}
}

int main(int argc, char** argv)
{
	if (argc < 2)
	{
		std::printf("usage: LTCFit <output LTC.cpp> [--verify]\n");
		return 2;
	}

	FTables T;
	fitTab(T);

	if (!writeTables(T, argv[1]))
	{
		std::printf("failed to write %s\n", argv[1]);
		return 1;
	}
	std::printf("wrote %s\n", argv[1]);

	// 目安の値 (ラフネス 1 / theta 0 と ラフネス 0.5 / theta 45 度付近)
	{
		double p[4];
		packInverse(T.M[(kSize - 1) + 0 * kSize], p);
		std::printf("roughness 1, theta 0 : Minv = (%.4f %.4f %.4f %.4f) amp = (%.4f %.4f)\n",
			p[0], p[1], p[2], p[3], T.Magnitude[(kSize - 1)], T.Fresnel[(kSize - 1)]);
		packInverse(T.M[32 + 34 * kSize], p);
		std::printf("roughness 0.5, t 34  : Minv = (%.4f %.4f %.4f %.4f) amp = (%.4f %.4f)\n",
			p[0], p[1], p[2], p[3], T.Magnitude[32 + 34 * kSize], T.Fresnel[32 + 34 * kSize]);
	}

	if (argc > 2 && std::strcmp(argv[2], "--verify") == 0)
	{
		verify(T);
	}

	return 0;
}
