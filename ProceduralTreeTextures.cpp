#include "Main.h"
#include "ProceduralTree.h"

#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

namespace fs = std::filesystem;

// ============================================================
//  FProceduralTreeTextures
//  木用のテクスチャを CPU で生成して RGBA8 + ミップチェーンの DDS
//  (DX10 ヘッダ) として Saved/ProceduralTexture/ に書き出す。
//  葉 (512²) は樫の葉: 倒卵形に丸い裂片 (片側 3〜4 個、左右で位相がずれる) + 裂片ごとの側脈 (EvalLeaf)、
//  脈のハイトフィールドから法線、AO / ラフネスの入った ARM。
//  房アトラス (1024², 2x2 セル) は小枝に付いた 5〜6 枚の葉を EvalLeaf で合成し、
//  インスタンスごとの傾きを法線に入れる (偽の立体感)。
//  樹皮 (1024², 両方向タイル) は v6: 蛇行する縦の溝の曲線 + 斜めの連絡溝 + 途切れる溝で作る長い畝の網
//  (格子なし。溝は浅い Y で合流 / 分岐)、V 字の壁と丸い天辺 (溝の深さは色でなく法線 / AO で出す)、
//  天辺の横の割れ目 / 欠け / 薄片 / 縦の繊維、地衣 / 苔は控えめ。小枝 (256²) は皮目 (横長の楕円) と
//  縦の繊維のある若い樹皮 (幹の畝の天辺程度の暗さ)。
//  葉 / 房の α は輪郭の符号付き距離からのランプ (AlphaFromEdge) で作り、
//  BaseColor の DDS はカバレッジ保存ミップ (WriteDDS の PreserveCoverageClip)
//  で書く。法線 / ARM / 樹皮は素のボックスフィルタ。
//  生成は初回だけなので所要時間を OutputDebugStringA に出す。
// ============================================================
namespace
{
	constexpr float PT_PI = 3.14159265358979323846f;

	inline float Clamp(float v, float lo, float hi) { return (v < lo) ? lo : (v > hi) ? hi : v; }
	inline float Lerp(float a, float b, float t) { return a + (b - a) * t; }
	inline float SmoothStep(float e0, float e1, float x)
	{
		const float t = Clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
		return t * t * (3.0f - 2.0f * t);
	}

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

	// ---- α のカバレッジ保存 (マスク付きテクスチャ用) ----
	// ボックスフィルタだけだと遠いミップほど α >= clip のテクセルが減って葉がやせる
	// (遠景で樹冠が透け、CSM の遠いカスケードで葉の影が消える)。
	// セル (CellsX x CellsY、アトラスはセルごと) 単位で mip 0 のカバレッジ
	// c0 = (α >= clip255 のテクセル比) を取り、各ミップの α に倍率 s を掛けて
	// カバレッジが c0 に最も近くなるようにする (s は [0.25, 8] を二分探索)。
	// セルが 4x4 以下になったら探索をやめて直前の倍率を使い回す。RGB は触らない
	// (α 0 の下でも色は有効なので、ボックスフィルタのままで良い)。
	// ミップ連鎖は倍率を掛ける前の α から作る (倍率が段ごとに累積しないように)。
	inline unsigned char ScaleAlpha(unsigned char A, float Scale)
	{
		const float v = static_cast<float>(A) * Scale + 0.5f;
		return (v >= 255.0f) ? 255 : static_cast<unsigned char>(v);
	}

	void PreserveAlphaCoverage(std::vector<FImageRGBA>& Mips, float Clip, unsigned int CellsX, unsigned int CellsY)
	{
		if (Mips.empty()) return;
		CellsX = (CellsX > 0) ? CellsX : 1;
		CellsY = (CellsY > 0) ? CellsY : 1;
		const unsigned int clip255 = static_cast<unsigned int>(Clamp(Clip, 0.0f, 1.0f) * 255.0f + 0.5f);
		const size_t numCells = static_cast<size_t>(CellsX) * CellsY;

		// テクセル -> セル番号 (ミップがセル数より小さくなっても破綻しない)
		auto CellOf = [&](unsigned int X, unsigned int Y, unsigned int W, unsigned int H) -> size_t
			{
				return static_cast<size_t>(Y * CellsY / H) * CellsX + static_cast<size_t>(X * CellsX / W);
			};
		// セルごとの α リストを集める
		auto Gather = [&](const FImageRGBA& Img, std::vector<std::vector<unsigned char>>& OutCells)
			{
				OutCells.assign(numCells, {});
				for (unsigned int y = 0; y < Img.Height; ++y)
					for (unsigned int x = 0; x < Img.Width; ++x)
						OutCells[CellOf(x, y, Img.Width, Img.Height)].push_back(Img.Pixels[(static_cast<size_t>(y) * Img.Width + x) * 4 + 3]);
			};
		// 倍率 Scale を掛けたときのカバレッジ
		auto Coverage = [&](const std::vector<unsigned char>& Alphas, float Scale) -> double
			{
				if (Alphas.empty()) return 0.0;
				size_t n = 0;
				for (unsigned char a : Alphas) n += (ScaleAlpha(a, Scale) >= clip255) ? 1 : 0;
				return static_cast<double>(n) / static_cast<double>(Alphas.size());
			};

		std::vector<std::vector<unsigned char>> cells;
		Gather(Mips[0], cells);
		std::vector<double> target(numCells, 0.0);
		for (size_t c = 0; c < numCells; ++c) target[c] = Coverage(cells[c], 1.0f);

		std::vector<float> scale(numCells, 1.0f);
		for (size_t m = 1; m < Mips.size(); ++m)
		{
			FImageRGBA& mip = Mips[m];
			const unsigned int cellW = mip.Width / CellsX;
			const unsigned int cellH = mip.Height / CellsY;
			const bool bRefine = (cellW > 4) || (cellH > 4);

			if (bRefine)
			{
				Gather(mip, cells);
				for (size_t c = 0; c < numCells; ++c)
				{
					if (target[c] <= 0.0 || cells[c].empty()) continue;	// 空セルは倍率をいじらない

					// カバレッジは倍率に対して単調非減少なので二分探索で c0 を挟み込む
					float lo = 0.25f, hi = 8.0f;
					for (int i = 0; i < 12; ++i)
					{
						const float mid = 0.5f * (lo + hi);
						if (Coverage(cells[c], mid) < target[c]) lo = mid; else hi = mid;
					}
					const double dLo = fabs(Coverage(cells[c], lo) - target[c]);
					const double dHi = fabs(Coverage(cells[c], hi) - target[c]);
					scale[c] = (dLo <= dHi) ? lo : hi;
				}
			}

			for (unsigned int y = 0; y < mip.Height; ++y)
			{
				for (unsigned int x = 0; x < mip.Width; ++x)
				{
					unsigned char& a = mip.Pixels[(static_cast<size_t>(y) * mip.Width + x) * 4 + 3];
					a = ScaleAlpha(a, scale[CellOf(x, y, mip.Width, mip.Height)]);
				}
			}
		}
	}

	// PreserveCoverageClip >= 0 でマスク用のカバレッジ保存ミップにする (葉 / 房の BaseColor のみ。
	// 法線 / ARM / 樹皮は -1 で素のボックスフィルタ)。CellsX x CellsY はアトラスのセル分割
	bool WriteDDS(const std::string& Path, const FImageRGBA& Image, float PreserveCoverageClip = -1.0f, unsigned int CellsX = 1, unsigned int CellsY = 1)
	{
		// ---- ミップチェーン ----
		std::vector<FImageRGBA> mips;
		mips.push_back(Image);
		while (mips.back().Width > 1 || mips.back().Height > 1)
		{
			mips.push_back(Downsample(mips.back()));
		}
		if (PreserveCoverageClip >= 0.0f)
		{
			PreserveAlphaCoverage(mips, PreserveCoverageClip, CellsX, CellsY);
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

	// 0 <= V < Period ならそのまま (ノイズの格子は普通この範囲。剰余は遅いので避ける)、外れたときだけ WrapInt
	inline int WrapFast(int V, int Period)
	{
		return (static_cast<unsigned int>(V) < static_cast<unsigned int>(Period)) ? V : WrapInt(V, Period);
	}

	// floorf と同じ結果 (|x| < 2^31) を切り捨て変換で (CRT の floorf 呼び出しは内側のループで目立つ)
	inline int FloorI(float X)
	{
		const int i = static_cast<int>(X);
		return (static_cast<float>(i) > X) ? i - 1 : i;
	}

	// U / V (0..1) をそれぞれ PeriodX / PeriodY 個の格子で評価する。両方向にタイルする
	float TileableValueNoise(float U, float V, int PeriodX, int PeriodY, unsigned int Seed)
	{
		const float x = U * static_cast<float>(PeriodX);
		const float y = V * static_cast<float>(PeriodY);
		const int ix = FloorI(x);
		const int iy = FloorI(y);
		const float fx = x - static_cast<float>(ix);
		const float fy = y - static_cast<float>(iy);
		const float sx = fx * fx * (3.0f - 2.0f * fx);
		const float sy = fy * fy * (3.0f - 2.0f * fy);

		const int x0 = WrapFast(ix, PeriodX), x1 = WrapFast(ix + 1, PeriodX);
		const int y0 = WrapFast(iy, PeriodY), y1 = WrapFast(iy + 1, PeriodY);
		const float n00 = LatticeHash(x0, y0, Seed);
		const float n10 = LatticeHash(x1, y0, Seed);
		const float n01 = LatticeHash(x0, y1, Seed);
		const float n11 = LatticeHash(x1, y1, Seed);

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

	// ハイトフィールド (0..1) から接線空間法線 (T = +U, B = cross(N, T) = +V, 既存アセットの nor_dx と同じ規約) を作る。
	// bWrap = true でタイル境界をラップ、false で端をクランプ
	void HeightToNormal(const std::vector<float>& Height, unsigned int Size, float Strength, bool bWrap, FImageRGBA& OutNormal)
	{
		// 隣のテクセルの番号 (ラップ / クランプ)。剰余は内側のループで遅いので端だけ特別扱い
		auto Prev = [&](unsigned int i) -> unsigned int { return (i > 0) ? i - 1 : (bWrap ? Size - 1 : 0); };
		auto Next = [&](unsigned int i) -> unsigned int { return (i + 1 < Size) ? i + 1 : (bWrap ? 0 : Size - 1); };

		for (unsigned int y = 0; y < Size; ++y)
		{
			const float* row = &Height[static_cast<size_t>(y) * Size];
			const float* rowM = &Height[static_cast<size_t>(Prev(y)) * Size];
			const float* rowP = &Height[static_cast<size_t>(Next(y)) * Size];
			for (unsigned int x = 0; x < Size; ++x)
			{
				const float dhdu = (row[Next(x)] - row[Prev(x)]) * 0.5f;
				const float dhdv = (rowP[x] - rowM[x]) * 0.5f;

				XMFLOAT3 n{ -dhdu * Strength, -dhdv * Strength, 1.0f };
				const float len = sqrtf(n.x * n.x + n.y * n.y + n.z * n.z);
				n.x /= len; n.y /= len; n.z /= len;

				OutNormal.Set(x, y, 0.5f + 0.5f * n.x, 0.5f + 0.5f * n.y, 0.5f + 0.5f * n.z, 1.0f);
			}
		}
	}

	// ---- タイル可能な Worley (セル) ノイズ ----
	// PeriodX x PeriodY の格子の各セルに特徴点を 1 つ置き、3x3 近傍から最近 (F1) / 2 番目 (F2) の距離
	// (格子単位) と最近セルの番号を返す。AnisoX / AnisoY は距離の軸倍率 (皮目を横長にする等)。
	// セル番号は WrapInt で巻くので両方向にタイルする
	struct FWorleySample
	{
		float F1 = 0.0f;
		float F2 = 0.0f;
		unsigned int CellId = 0;
	};

	void TileableWorley(float U, float V, int PeriodX, int PeriodY, unsigned int Seed, FWorleySample& Out, float AnisoX = 1.0f, float AnisoY = 1.0f)
	{
		const float x = U * static_cast<float>(PeriodX);
		const float y = V * static_cast<float>(PeriodY);
		const int ix = FloorI(x);
		const int iy = FloorI(y);

		// 距離を縮める軸は 2 セル先の点も最近になり得るので探索を広げる
		const int rx = (AnisoX < 0.75f) ? 2 : 1;
		const int ry = (AnisoY < 0.75f) ? 2 : 1;
		float f1 = 1.0e9f, f2 = 1.0e9f;
		unsigned int id = 0;
		for (int dy = -ry; dy <= ry; ++dy)
		{
			for (int dx = -rx; dx <= rx; ++dx)
			{
				const int cx = ix + dx, cy = iy + dy;
				const int wx = WrapInt(cx, PeriodX), wy = WrapInt(cy, PeriodY);
				const float px = static_cast<float>(cx) + LatticeHash(wx, wy, Seed);
				const float py = static_cast<float>(cy) + LatticeHash(wx, wy, Seed ^ 0x9E37u);
				const float ex = (x - px) * AnisoX, ey = (y - py) * AnisoY;
				const float d = sqrtf(ex * ex + ey * ey);
				if (d < f1) { f2 = f1; f1 = d; id = static_cast<unsigned int>(wy * PeriodX + wx); }
				else if (d < f2) { f2 = d; }
			}
		}
		Out.F1 = f1;
		Out.F2 = f2;
		Out.CellId = id;
	}

	// リッジマルチフラクタル (Musgrave): オクターブごとに n = 1 - |2 noise - 1| の尖った山を n² x 重みで重ね、
	// 重み = saturate(2n) で山の上ほど細部を足す。周期は 1 オクターブごとに 2 倍。0..1 に正規化
	float TileableRidged(float U, float V, int PeriodX, int PeriodY, int Octaves, unsigned int Seed)
	{
		float sum = 0.0f, amplitude = 1.0f, total = 0.0f, weight = 1.0f;
		int px = PeriodX, py = PeriodY;
		for (int o = 0; o < Octaves; ++o)
		{
			float n = TileableValueNoise(U, V, px, py, Seed + static_cast<unsigned int>(o) * 7919u);
			n = 1.0f - fabsf(2.0f * n - 1.0f);
			n = n * n * weight;
			weight = Clamp(2.0f * n, 0.0f, 1.0f);
			sum += n * amplitude;
			total += amplitude;
			amplitude *= 0.5f;
			px *= 2;
			py *= 2;
		}
		return (total > 0.0f) ? sum / total : 0.0f;
	}

	// 正方の高さ場を半径 Radius のボックスで分離ぼかし (タイル境界をラップ)。キャビティ AO 用
	void BoxBlurWrap(const std::vector<float>& Src, unsigned int Size, int Radius, std::vector<float>& Out)
	{
		const size_t N = static_cast<size_t>(Size) * Size;
		std::vector<float> tmp(N);
		Out.assign(N, 0.0f);
		const float inv = 1.0f / static_cast<float>(2 * Radius + 1);
		const int S = static_cast<int>(Size);

		// 横 (移動和)
		for (int y = 0; y < S; ++y)
		{
			const float* row = &Src[static_cast<size_t>(y) * Size];
			float sum = 0.0f;
			for (int k = -Radius; k <= Radius; ++k) sum += row[WrapInt(k, S)];
			for (int x = 0; x < S; ++x)
			{
				tmp[static_cast<size_t>(y) * Size + static_cast<size_t>(x)] = sum * inv;
				sum += row[WrapInt(x + Radius + 1, S)] - row[WrapInt(x - Radius, S)];
			}
		}
		// 縦
		for (int x = 0; x < S; ++x)
		{
			float sum = 0.0f;
			for (int k = -Radius; k <= Radius; ++k) sum += tmp[static_cast<size_t>(WrapInt(k, S)) * Size + static_cast<size_t>(x)];
			for (int y = 0; y < S; ++y)
			{
				Out[static_cast<size_t>(y) * Size + static_cast<size_t>(x)] = sum * inv;
				sum += tmp[static_cast<size_t>(WrapInt(y + Radius + 1, S)) * Size + static_cast<size_t>(x)]
					- tmp[static_cast<size_t>(WrapInt(y - Radius, S)) * Size + static_cast<size_t>(x)];
			}
		}
	}

	// 低周波のタイル可能ノイズを粗い格子で評価して双一次補間で読む (1024² で 4x4 周期の FBM をテクセルごとに
	// 評価するのは無駄なので)。格子はラップするので結果もタイルする。Eval(u, v) を (Res + 1)² 点で呼ぶ
	struct FCoarseField
	{
		unsigned int Res = 0;
		std::vector<float> Values;	// (Res + 1) x (Res + 1)、最後の行 / 列は最初と同じ (ラップ)

		template <typename TEval>
		void Build(unsigned int InRes, TEval&& Eval)
		{
			Res = InRes;
			Values.resize(static_cast<size_t>(Res + 1) * (Res + 1));
			for (unsigned int y = 0; y <= Res; ++y)
			{
				for (unsigned int x = 0; x <= Res; ++x)
				{
					const unsigned int wx = (x == Res) ? 0 : x, wy = (y == Res) ? 0 : y;
					Values[static_cast<size_t>(y) * (Res + 1) + x] = Eval(static_cast<float>(wx) / static_cast<float>(Res), static_cast<float>(wy) / static_cast<float>(Res));
				}
			}
		}

		float Sample(float U, float V) const
		{
			const float x = (U - static_cast<float>(FloorI(U))) * static_cast<float>(Res);
			const float y = (V - static_cast<float>(FloorI(V))) * static_cast<float>(Res);
			const unsigned int ix = static_cast<unsigned int>(x) < Res ? static_cast<unsigned int>(x) : Res - 1;
			const unsigned int iy = static_cast<unsigned int>(y) < Res ? static_cast<unsigned int>(y) : Res - 1;
			const float fx = x - static_cast<float>(ix), fy = y - static_cast<float>(iy);
			const size_t i = static_cast<size_t>(iy) * (Res + 1) + ix;
			return Lerp(Lerp(Values[i], Values[i + 1], fx), Lerp(Values[i + Res + 1], Values[i + Res + 2], fx), fy);
		}
	};

	// ============================================================
	//  樹皮 v6 (Size², 両方向タイル。BaseColor / Normal / ARM を同じ高さ場から作る)
	//  v4 の千鳥格子の異方 Worley は畝が全部同じ長さの尖った菱形になり、楔の先が横の帯に揃って
	//  幹の上で黒い菱形の網 (蛇皮 / パイナップル) に見えた。成熟した樫の溝は長く (0.5〜2 m)、まれに浅い Y で
	//  合流 / 分岐するだけなので、格子をやめて溝そのものを曲線で持つ:
	//   - FFurrowNetwork: BarkFurrowsU 本の縦の溝。溝 k の中心線 X_k(v) = k + 0.5 + ジッタ + 1D ノイズの蛇行
	//     (V に 3 / 6 / 12 / 24 周期、溝ごとに種が違う) を表 (BarkFurrowTable 行) に前計算。隣の溝とは独立に蛇行するので
	//     畝の幅が場所ごとに 0.5〜1.5 倍に変わる。溝の幅も V に沿って揺らぐ
	//   - 斜めの連絡溝 (FLink): 隣り合う溝 g / g+1 の間 (= 畝) を 0〜3 本 (確率 10 / 30 / 40 / 20 %) が浅い角度
	//     (長さ BarkLinkLenMin..Max タイル、約 20〜35 度) で横切る。両端は溝に接線方向で合流する = 溝の Y。
	//     連絡溝で畝が切られ、片方の端が楔になる。位置は畝ごとのハッシュなので楔の高さは揃わない。
	//     端が途切れた溝に当たる連絡溝は使わない (宙から始まる溝にしない)
	//   - 途切れる溝: 一部の溝は V の一区間で浅くなって消える (先細りで閉じる) = 2 本の畝が合流する
	//   - 畝の個体 (高さ / 色味 / 傾き など) は「溝の間 g + 連絡溝で区切られた何番目か」で引く。消えかけの溝の
	//     両側では隣の畝の値と滑らかに混ぜる (合流した畝の上に段差を出さない)
	//   - 高さ: 溝の底 (en = 中心線からの距離 x 2 < BarkFurrowFloor) は広めで平ら、壁は BarkFurrowWall の幅の V 字
	//     (ほぼ直線、底と上端は少し丸い。法線が畝ごとに光の当たる側と影の側を持つ)、縁は縦に長いノイズでぎざぎざ、
	//     天辺は丸い (BarkCrest)。畝ごとの高さは 0.55..1.0 と大きくばらし (高い畝が隣を覆う AO)、畝に沿った低周波の
	//     うねり + 横の傾き、天辺を段々のブロックに割る V 字の横の割れ目 (畝ごとの位相、4 割は欠く、まれに途切れる)、
	//     天辺のもろい欠けと細かな穴、縦長の小さな薄片、縦の繊維、風化のむら、ざらつき。法線用の高さでは溝の断面だけ
	//     BarkFurrowRelief 倍 (細部は等倍) にして、溝の深さを法線で出す
	//   - 色: 溝の底は焦げ茶 (平均 ≈ 0.25 / 0.19 / 0.13、中心の細い割れ目はさらに暗い)、壁は底から上へなだらかに明るく
	//     (赤みのある茶、上半分は天辺に近い)、天辺は風化した明るい灰茶 (平均 ≈ 0.42 / 0.36 / 0.28。天辺 : 底 ≈ 3 : 1 の
	//     線形輝度なので、日の当たらない幹でも溝が読める)。畝ごとの明るさ、3x4 周期の大きな明暗、2x2 周期の暖色 / 灰のむら、
	//     繊維 / ブロック / 薄片の縞、割れ目と欠けは少し暗い。地衣 (灰緑の痂) は溝の壁と一部の畝の天辺、
	//     苔 (くすんだオリーブ、緑みは弱い) は溝の底のまだら (どちらも控えめ)
	//   - v6: v5 の溝 (10 本 / タイル、底 0.04 / 壁 0.50) は幹の上の遠目 (ミップ 3〜4) で暗い芯だけが残り 1〜2 px の髪の毛の
	//     線になり、幹も暗いシルエットだった。溝を 8 本に減らし、底を広く (0.18) 壁を急に (0.35) して暗い帯を間隔の約 0.36
	//     (遠目で 3〜4 px) に、色を明るく、AO の底を 0.55 -> 0.80 に (溝の深さは帯の幅と法線の明暗で出す)
	//  ARM: AO = 溝で暗く (底 BarkAOFloor) x 弱いキャビティ (半径 6 / 48 の 2 スケールのぼかし差)、
	//       ラフネスは溝 0.95 / 畝 0.80 / 地衣 0.88。法線は半径 1 でぼかした高さから (ミップで斜面が残るように)、ラップ差分
	// ============================================================
	constexpr int   BarkFurrowsU = 8;			// 溝の本数 (U = 円周方向、タイルあたり)。畝の平均幅 = 128 テクセル
	constexpr int   BarkFurrowTable = 512;		// 溝の中心線の表の行数 (V 方向、線形補間で読む)
	constexpr float BarkFurrowJitter = 0.15f;	// 溝の基準位置のずれ (溝の間隔単位、±)
	constexpr float BarkFurrowWander = 0.30f;	// 溝の蛇行の振幅の上限 (同、±。FBM なので普段の振れは半分ほど)。ジッタとの和 < 0.5 (隣の溝と交差しない)
	constexpr float BarkFurrowWidthVar = 0.05f;	// 溝の幅の揺らぎ (同、±)
	constexpr float BarkFurrowEndChance = 0.35f;	// 溝が V の一区間で途切れる確率 (溝ごと)
	constexpr float BarkFurrowFade = 0.7f;		// 途切れた区間で中心線からの距離に足す量 (同。0.5 を超えると溝は完全に消える)
	constexpr float BarkLinkLenMin = 0.25f;		// 斜めの連絡溝の V の長さ (タイル単位)。溝の間隔 1/8 を横切るので約 20〜35 度
	constexpr float BarkLinkLenMax = 0.42f;
	constexpr int   BarkMaxLinks = 3;			// 畝 (溝の間) 1 本あたりの連絡溝の最大数
	constexpr float BarkFurrowFloor = 0.18f;	// 溝の底の幅 (en = 中心線からの距離の 2 倍、溝の間隔単位)。広めの平らな底 (遠目でも髪の毛の線にならない)
	constexpr float BarkFurrowWall = 0.35f;		// 溝の壁の幅 (同上)。底と合わせて溝の幅 ≈ 間隔の 0.53 (68 テクセル。暗い帯は約 45 テクセル)、残りが天辺
	constexpr float BarkEdgeNoise = 0.16f;		// 縁のぎざぎざ (同上)
	constexpr float BarkFloor = 0.10f;			// 溝の底の高さ
	constexpr int   BarkBlockRows = 8;			// 天辺を横に割る割れ目の平均間隔 (タイルあたり。≈ 128 テクセル。畝ごとに位相が違う)
	constexpr float BarkBlockCrackW = 0.10f;	// 同、割れ目の半幅 (行単位 ≈ 13 テクセル、V 字の断面)
	constexpr float BarkCrest = 0.25f;			// 天辺の丸み (縁で下がる高さ)
	constexpr int   BarkFlakesU = 54;			// 天辺の薄片の格子 (19 x 68 テクセル = 1:3.6)
	constexpr int   BarkFlakesV = 15;
	constexpr float BarkFlakeMetricY = 3.0f;	// 薄片の距離の y 倍率 (3.6 でテクセル等方。少し小さく = やや縦長の六角)
	constexpr float BarkNormalStrength = 7.0f;
	constexpr float BarkFurrowRelief = 4.0f;	// 法線用の高さで溝の断面 (底 -> 天辺) だけを強める倍率 (細部は等倍)
	constexpr int   BarkNormalBlurRadius = 1;	// 法線を取る前の高さのぼかし (テクセル)
	constexpr float BarkAOFloor = 0.80f;		// 溝の底の AO (x キャビティ 0.85..1 で最小 ≈ 0.68)
	constexpr int   BarkAOBlurRadius = 6;		// キャビティ AO のぼかし半径 (テクセル、細かいスケール)
	constexpr int   BarkAOBlurRadiusWide = 48;	// 同、溝全体のスケール (ミップ 3 でも見える幅)

	// 溝の網。表は Build で作るので Eval は表の参照と距離計算だけ。
	// 座標: X = U x BarkFurrowsU (溝の間隔単位)、V はタイル単位 (0..1、ラップ)。溝 k の中心線は X_k(V)
	struct FFurrowNetwork
	{
		struct FLink
		{
			float V0 = 0.0f;			// 連絡溝の始まり (左右どちらかの溝から離れる V)
			float Len = 0.3f;			// V の長さ
			bool bLeftToRight = true;	// V が増えると左の溝 (g) -> 右の溝 (g+1) へ移る
			bool bEnabled = true;		// 端が途切れた溝に当たる連絡溝は使わない (宙から始まる溝にしない)
		};

		struct FSample
		{
			float En = 1.0f;			// 最寄りの溝の中心線からの距離 x 2 (溝の間隔単位。v4 の F2 - F1 と同じ尺度)
			int Gap = 0;				// 畝の左の溝の番号 (ラップ済み)
			int Piece = 0;				// 連絡溝で区切られた何番目の畝か
			int GapL = 0, PieceL = 0;	// 左の溝の向こうの畝
			int GapR = 0, PieceR = 0;	// 右の溝の向こうの畝
			float SL = 0.5f, SR = 0.5f;	// 左 / 右の溝の中心線からの (横の) 距離
			float PL = 1.0f, PR = 1.0f;	// 左 / 右の溝の深さ (1 = ある、0 = 途切れている)
			float DX = 0.0f;			// 畝の中心からのずれ (溝の間隔単位、右が +)
		};

		int Count = 0;
		int Rows = 0;
		std::vector<float> CenterX;		// [k * Rows + j] 溝 k の中心線 (溝の間隔単位、0..Count の範囲)
		std::vector<float> Presence;	// [k * Rows + j] 溝の深さ (0..1)
		std::vector<float> Width;		// [k * Rows + j] 溝の幅の揺らぎ (距離から引く量)
		std::vector<int> NumLinks;		// [g]
		std::vector<FLink> Links;		// [g * BarkMaxLinks + i]

		void Build(int InCount, int InRows, unsigned int Seed)
		{
			Count = InCount; Rows = InRows;
			const size_t n = static_cast<size_t>(Count) * Rows;
			CenterX.resize(n); Presence.resize(n); Width.resize(n);
			for (int k = 0; k < Count; ++k)
			{
				const float base = static_cast<float>(k) + 0.5f + BarkFurrowJitter * (2.0f * LatticeHash(k, 0, Seed) - 1.0f);
				const bool bEnds = LatticeHash(k, 1, Seed) < BarkFurrowEndChance;
				const float endV = LatticeHash(k, 2, Seed);
				const float endHalf = 0.03f + 0.07f * LatticeHash(k, 3, Seed);
				const unsigned int ks = Seed + 101u * static_cast<unsigned int>(k + 1);
				for (int j = 0; j < Rows; ++j)
				{
					const float v = static_cast<float>(j) / static_cast<float>(Rows);
					const size_t i = static_cast<size_t>(k) * Rows + j;
					CenterX[i] = base + BarkFurrowWander * (TileableFBM(0.0f, v, 1, 3, 4, ks) - 0.5f) * 2.0f;
					Width[i] = BarkFurrowWidthVar * (TileableFBM(0.0f, v, 1, 5, 2, ks + 7u) - 0.5f) * 2.0f;
					float p = 1.0f;
					if (bEnds)
					{
						float dv = fabsf(v - endV);
						dv = (dv > 0.5f) ? 1.0f - dv : dv;
						p = SmoothStep(endHalf, endHalf + 0.12f, dv);	// 長く先細りして閉じる
					}
					Presence[i] = p;
				}
			}

			// 連絡溝: 畝ごとに 0..BarkMaxLinks 本、V を等分した区画に 1 本ずつ (重ならない)
			NumLinks.assign(static_cast<size_t>(Count), 0);
			Links.assign(static_cast<size_t>(Count) * BarkMaxLinks, FLink{});
			for (int g = 0; g < Count; ++g)
			{
				const float h = LatticeHash(g, 10, Seed);
				const int m = (h < 0.10f) ? 0 : (h < 0.40f) ? 1 : (h < 0.80f) ? 2 : 3;
				NumLinks[static_cast<size_t>(g)] = m;
				for (int i = 0; i < m; ++i)
				{
					FLink& L = Links[static_cast<size_t>(g) * BarkMaxLinks + i];
					const float slot = 1.0f / static_cast<float>(m);
					L.Len = fminf(Lerp(BarkLinkLenMin, BarkLinkLenMax, LatticeHash(g, 11 + i, Seed)), slot * 0.9f);
					L.V0 = slot * static_cast<float>(i) + (slot - L.Len) * LatticeHash(g, 13 + i, Seed);
					L.bLeftToRight = LatticeHash(g, 15 + i, Seed) < 0.5f;
					// 両端 (± 0.04) で合流する溝が途切れていれば使わない
					const int ka = L.bLeftToRight ? g : (g + 1) % Count, kz = L.bLeftToRight ? (g + 1) % Count : g;
					for (int e = -1; e <= 1 && L.bEnabled; ++e)
					{
						auto PresenceAt = [&](int K, float V)
							{
								const int j = WrapInt(FloorI((V - static_cast<float>(FloorI(V))) * static_cast<float>(Rows) + 0.5f), Rows);
								return Presence[static_cast<size_t>(K) * Rows + j];
							};
						const float dv = 0.04f * static_cast<float>(e);
						if (PresenceAt(ka, L.V0 + dv) < 0.99f || PresenceAt(kz, L.V0 + L.Len + dv) < 0.99f) L.bEnabled = false;
					}
				}
			}
		}

		// 溝 k (ラップ前の番号) の V における中心線 / 傾き (dX/dV) / 深さ / 幅
		void Furrow(int K, int J0, int J1, float F, float& OutX, float& OutSlope, float& OutP, float& OutW) const
		{
			const int wk = WrapInt(K, Count);
			const size_t i0 = static_cast<size_t>(wk) * Rows + J0, i1 = static_cast<size_t>(wk) * Rows + J1;
			const float x0 = CenterX[i0], x1 = CenterX[i1];
			OutX = Lerp(x0, x1, F) + static_cast<float>(K - wk);
			OutSlope = (x1 - x0) * static_cast<float>(Rows);
			OutP = Lerp(Presence[i0], Presence[i1], F);
			OutW = Lerp(Width[i0], Width[i1], F);
		}

		// 溝の両側の畝を混ぜる幅 (溝の中心線から ±、溝の間隔単位)。深い溝は底の中だけ、消えかけの溝ほど広く
		static float BlendWidth(float Presence) { return 0.04f + 0.6f * (1.0f - Presence); }

		// 連絡溝の横の位置の補間 (両端で溝に接線方向で合流し、中ほどで最も傾く)
		static float LinkBlend(float T) { return Lerp(T, T * T * (3.0f - 2.0f * T), 0.6f); }
		static float LinkBlendSlope(float T) { return Lerp(1.0f, 6.0f * T * (1.0f - T), 0.6f); }

		// 畝 g (ラップ前) の中で (X, V) が連絡溝で区切られた何番目にあるか。XL / XR はその V での左右の溝
		int PieceOf(int G, float X, float V, float XL, float XR) const
		{
			const int wg = WrapInt(G, Count);
			const int m = NumLinks[static_cast<size_t>(wg)];
			if (m <= 1) return 0;
			int best = 0;
			float bestSince = 1.0e9f;
			for (int i = 0; i < m; ++i)
			{
				const FLink& L = Links[static_cast<size_t>(wg) * BarkMaxLinks + i];
				if (!L.bEnabled) continue;
				float dv = V - L.V0;
				dv -= static_cast<float>(FloorI(dv));
				bool bPassed = dv >= L.Len;
				if (!bPassed)
				{
					const float b = LinkBlend(dv / L.Len);
					const float xc = L.bLeftToRight ? Lerp(XL, XR, b) : Lerp(XR, XL, b);
					bPassed = L.bLeftToRight ? (X < xc) : (X > xc);
				}
				const float since = bPassed ? dv : dv + 1.0f;	// 最後に越えた連絡溝 = 越えてからの V が最小
				if (since < bestSince) { bestSince = since; best = i; }
			}
			return best;
		}

		// U / V は 0..1 (ワープ込みで少し外れても良い)
		void Eval(float U, float V, FSample& Out) const
		{
			const float X = U * static_cast<float>(Count);
			const float vt = V - static_cast<float>(FloorI(V));
			const float jf = vt * static_cast<float>(Rows);
			const int j0r = static_cast<int>(jf);
			const int j0 = (j0r < Rows - 1) ? j0r : Rows - 1;
			const int j1 = (j0 + 1 < Rows) ? j0 + 1 : 0;
			const float f = jf - static_cast<float>(j0);
			const float invC = 1.0f / static_cast<float>(Count);

			// 溝 kb-2 .. kb+2 (蛇行 + ジッタは ±0.3 以内なので X を挟む左右の溝は kb-1 / kb / kb+1 のどれか。
			// その外側の溝は溝の向こうの畝の区切りの判定に使う)
			const int kb = FloorI(X);
			float fx[5], fs[5], fp[5], fw[5];
			for (int i = 0; i < 5; ++i) Furrow(kb - 2 + i, j0, j1, f, fx[i], fs[i], fp[i], fw[i]);

			// 左の溝 = X 以下で最大の中心線 (溝 kb-1 は必ず X 以下、溝 kb+1 は必ず X より右)
			const int li = (fx[2] <= X) ? 2 : 1;
			const int ri = li + 1;
			float dmin = 1.0e9f;
			for (int i = 0; i < 5; ++i)
			{
				const float d = fabsf(X - fx[i]) / sqrtf(1.0f + fs[i] * fs[i] * invC * invC);
				dmin = fminf(dmin, d + BarkFurrowFade * (1.0f - fp[i]) - fw[i]);
			}

			// 連絡溝 (左の溝の畝と、その左右の畝)
			for (int gi = li - 1; gi <= li + 1; ++gi)
			{
				const int g = kb - 2 + gi;
				const int wg = WrapInt(g, Count);
				const int m = NumLinks[static_cast<size_t>(wg)];
				for (int i = 0; i < m; ++i)
				{
					const FLink& L = Links[static_cast<size_t>(wg) * BarkMaxLinks + i];
					if (!L.bEnabled) continue;
					float dv = vt - L.V0;
					dv -= static_cast<float>(FloorI(dv));
					if (dv >= L.Len) continue;
					const float t = dv / L.Len;
					const float b = LinkBlend(t);
					const float xa = L.bLeftToRight ? fx[gi] : fx[gi + 1], xb = L.bLeftToRight ? fx[gi + 1] : fx[gi];
					const float sa = L.bLeftToRight ? fs[gi] : fs[gi + 1], sb = L.bLeftToRight ? fs[gi + 1] : fs[gi];
					const float xc = Lerp(xa, xb, b);
					const float slope = Lerp(sa, sb, b) + (xb - xa) * LinkBlendSlope(t) / L.Len;
					const float d = fabsf(X - xc) / sqrtf(1.0f + slope * slope * invC * invC);
					dmin = fminf(dmin, d - 0.5f * (fw[gi] + fw[gi + 1]));
				}
			}

			Out.En = 2.0f * fmaxf(dmin, 0.0f);
			const int g = kb - 2 + li;
			Out.Gap = WrapInt(g, Count);
			Out.Piece = PieceOf(g, X, vt, fx[li], fx[ri]);
			Out.SL = (X - fx[li]) / sqrtf(1.0f + fs[li] * fs[li] * invC * invC);
			Out.SR = (fx[ri] - X) / sqrtf(1.0f + fs[ri] * fs[ri] * invC * invC);
			Out.PL = fp[li];
			Out.PR = fp[ri];
			Out.DX = X - 0.5f * (fx[li] + fx[ri]);
			// 溝の向こうの畝 (溝のすぐ向こうの点で区切りを判定する)。混ぜる幅の外では使わないので求めない
			Out.GapL = WrapInt(g - 1, Count);
			Out.PieceL = (Out.SL < BlendWidth(Out.PL)) ? PieceOf(g - 1, fx[li] - 0.01f, vt, fx[li - 1], fx[li]) : 0;
			Out.GapR = WrapInt(g + 1, Count);
			Out.PieceR = (Out.SR < BlendWidth(Out.PR)) ? PieceOf(g + 1, fx[ri] + 0.01f, vt, fx[ri], fx[ri + 1]) : 0;
		}
	};

	// タイル可能な Voronoi 格子 (天辺の薄片用)。Build で特徴点を表に置くので、Eval はテーブル参照と距離計算だけ。
	// 距離は x セル単位 (y は MetricY 倍)、3x3 近傍
	struct FPlateLattice
	{
		struct FSample
		{
			float F1 = 0.0f, F2 = 0.0f;		// 最近 / 2 番目の特徴点までの距離
			int Id1 = 0;					// 最近のセル番号 (ラップ済み)
		};

		int Px = 0, Py = 0;
		float MetricY = 1.0f;
		std::vector<float> FX, FY;			// セル内の特徴点 (0..1)

		void Build(int InPx, int InPy, float InMetricY, unsigned int Seed)
		{
			Px = InPx; Py = InPy; MetricY = InMetricY;
			const size_t n = static_cast<size_t>(Px) * Py;
			FX.resize(n); FY.resize(n);
			for (int y = 0; y < Py; ++y)
			{
				for (int x = 0; x < Px; ++x)
				{
					const size_t i = static_cast<size_t>(y) * Px + x;
					FX[i] = 0.1f + 0.8f * LatticeHash(x, y, Seed);
					FY[i] = 0.1f + 0.8f * LatticeHash(x, y, Seed ^ 0x9E37u);
				}
			}
		}

		void Eval(float U, float V, FSample& Out) const
		{
			const float x = U * static_cast<float>(Px);
			const float y = V * static_cast<float>(Py);
			const int ix = FloorI(x);
			const int iy = FloorI(y);
			const float* fx = FX.data();
			const float* fy = FY.data();
			float f1 = 1.0e9f, f2 = 1.0e9f;
			int id1 = 0;
			for (int dy = -1; dy <= 1; ++dy)
			{
				const int cy = iy + dy;
				const int wy = WrapInt(cy, Py);
				for (int dx = -1; dx <= 1; ++dx)
				{
					const int cx = ix + dx;
					const int wx = WrapInt(cx, Px);
					const int i = wy * Px + wx;
					const float ex = x - (static_cast<float>(cx) + fx[i]);
					const float ey = (y - (static_cast<float>(cy) + fy[i])) * MetricY;
					const float d = ex * ex + ey * ey;
					if (d < f1) { f2 = f1; f1 = d; id1 = i; }
					else if (d < f2) { f2 = d; }
				}
			}
			Out.F1 = sqrtf(f1);
			Out.F2 = sqrtf(f2);
			Out.Id1 = id1;
		}
	};

	// 畝ごとの乱数パラメータ (畝の番号 = 溝の間 g x BarkMaxLinks + 区切り、で引く表)
	struct FRidgeParams
	{
		float Height = 0.0f;	// 天辺の高さ
		float Tint = 0.0f;		// 明るさ
		float Flaky = 0.0f;		// 薄片の見え方
		float Lichen = 0.0f;	// 天辺に地衣が付くか (0 / 1 に近い)
		float Warm = 0.0f;		// 暖色寄り (1) / 灰色寄り (0)
		float Lean = 0.0f;		// 天辺の横の傾き (片側が高い)
		float BlockPhase = 0.0f;	// 横の割れ目の位相 (行単位)
	};

	bool GenerateBarkTextures(const std::string& DiffPath, const std::string& NormalPath, const std::string& ARMPath, unsigned int Size)
	{
		const unsigned int size = Size;
		const size_t N = static_cast<size_t>(size) * size;
		FImageRGBA diff(size, size);
		FImageRGBA normal(size, size);
		FImageRGBA arm(size, size);
		std::vector<float> height(N), heightN(N), ridgeMap(N), microMap(N), lichenMap(N);

		// 低周波 / 中間のノイズは粗い格子で (ワープ 2x2 / 6x6、縁 32x12、むら 20x28、割れ目の途切れ 20x28、うねり 6x4、浅い溝 3x3、明暗 3x4、色味 2x2、地衣 12x14、苔 4x6)
		// (格子の解像度は周期に合わせて 1/16 / 1/8 / 1/4。1 周期に 4 点以上)
		const unsigned int coarse = (size >= 256) ? size / 4 : size;
		const unsigned int coarse8 = (size >= 512) ? size / 8 : coarse;
		const unsigned int coarse16 = (size >= 1024) ? size / 16 : coarse8;
		FCoarseField warpU, warpV, warpU2, warpV2, edgeLow, mottleLow, crackLow, swellLow, shallowLow, toneLow, hueLow, lichenLow, mossLow;
		warpU.Build(coarse16, [](float u, float v) { return TileableFBM(u, v, 2, 2, 3, 101u); });
		warpV.Build(coarse16, [](float u, float v) { return TileableFBM(u, v, 2, 2, 3, 103u); });
		warpU2.Build(coarse8, [](float u, float v) { return TileableFBM(u, v, 6, 6, 2, 105u); });
		warpV2.Build(coarse8, [](float u, float v) { return TileableFBM(u, v, 6, 6, 2, 109u); });
		edgeLow.Build(coarse, [](float u, float v) { return TileableFBM(u, v, 32, 12, 2, 41u); });
		mottleLow.Build(coarse, [](float u, float v) { return TileableFBM(u, v, 20, 28, 2, 33u); });
		crackLow.Build(coarse, [](float u, float v) { return TileableValueNoise(u, v, 20, 28, 31u); });
		swellLow.Build(coarse16, [](float u, float v) { return TileableFBM(u, v, 6, 4, 2, 113u); });
		shallowLow.Build(coarse16, [](float u, float v) { return TileableFBM(u, v, 3, 3, 2, 115u); });
		toneLow.Build(coarse8, [](float u, float v) { return TileableFBM(u, v, 3, 4, 3, 119u); });
		hueLow.Build(coarse16, [](float u, float v) { return TileableFBM(u, v, 2, 2, 2, 121u); });
		lichenLow.Build(coarse, [](float u, float v) { return TileableFBM(u, v, 12, 14, 2, 5u); });
		mossLow.Build(coarse8, [](float u, float v) { return TileableFBM(u, v, 4, 6, 3, 9u); });

		FFurrowNetwork furrows;
		furrows.Build(BarkFurrowsU, BarkFurrowTable, 11u);
		FPlateLattice flakes;
		flakes.Build(BarkFlakesU, BarkFlakesV, BarkFlakeMetricY, 19u);
		std::vector<FRidgeParams> params(static_cast<size_t>(BarkFurrowsU) * BarkMaxLinks);
		for (int c = 0; c < BarkFurrowsU * BarkMaxLinks; ++c)
		{
			FRidgeParams& p = params[static_cast<size_t>(c)];
			p.Height = 0.55f + 0.45f * LatticeHash(c, 1, 11u);
			p.Tint = 0.9f + 0.2f * LatticeHash(c, 2, 11u);
			p.Flaky = Lerp(0.3f, 1.0f, LatticeHash(c, 3, 11u));
			p.Lichen = SmoothStep(0.6f, 0.7f, LatticeHash(c, 4, 11u));
			p.Warm = LatticeHash(c, 5, 11u);
			p.Lean = (LatticeHash(c, 6, 11u) - 0.5f) * 0.16f;
			p.BlockPhase = LatticeHash(c, 8, 11u) * static_cast<float>(BarkBlockRows);
		}
		std::vector<float> flakeTones(static_cast<size_t>(BarkFlakesU) * BarkFlakesV);
		for (int c = 0; c < BarkFlakesU * BarkFlakesV; ++c) flakeTones[static_cast<size_t>(c)] = 0.94f + 0.12f * LatticeHash(c, 9, 19u);

		for (unsigned int y = 0; y < size; ++y)
		{
			for (unsigned int x = 0; x < size; ++x)
			{
				const size_t t = static_cast<size_t>(y) * size + x;
				const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(size);
				const float v = (static_cast<float>(y) + 0.5f) / static_cast<float>(size);

				// ---- 溝の網 (2 スケールでワープした座標で) ----
				const float micro = TileableFBM(u, v, 64, 64, 3, 23u);
				const float grain = TileableValueNoise(u, v, 160, 160, 29u);	// 表面のざらつき (6 テクセル)
				const float mottle = mottleLow.Sample(u, v);					// 風化のむら (中間スケール、縦にやや長い)
				const float wu = u + 0.045f * (warpU.Sample(u, v) - 0.5f) + 0.02f * (warpU2.Sample(u, v) - 0.5f) + 0.004f * (micro - 0.5f);
				const float wv = v + 0.03f * (warpV.Sample(u, v) - 0.5f) + 0.015f * (warpV2.Sample(u, v) - 0.5f);
				FFurrowNetwork::FSample s;
				furrows.Eval(wu, wv, s);
				const int id = s.Gap * BarkMaxLinks + s.Piece;
				const FRidgeParams& own = params[static_cast<size_t>(id)];
				// 消えかけの溝の向こうの畝と混ぜる (溝が深ければ底の中心で切り替わるだけ、浅いほど広く混ぜる)
				const FRidgeParams& pl = params[static_cast<size_t>(s.GapL * BarkMaxLinks + s.PieceL)];
				const FRidgeParams& pr = params[static_cast<size_t>(s.GapR * BarkMaxLinks + s.PieceR)];
				const float wlW = FFurrowNetwork::BlendWidth(s.PL), wrW = FFurrowNetwork::BlendWidth(s.PR);
				const float wl = 1.0f - SmoothStep(-wlW, wlW, s.SL), wr = 1.0f - SmoothStep(-wrW, wrW, s.SR);
				const float wo = fmaxf(1.0f - wl - wr, 0.0f);
				auto Mix = [&](float FRidgeParams::* M) { return (own.*M) * wo + (pl.*M) * wl + (pr.*M) * wr; };
				FRidgeParams p = own;
				p.Height = Mix(&FRidgeParams::Height);
				p.Tint = Mix(&FRidgeParams::Tint);
				p.Flaky = Mix(&FRidgeParams::Flaky);
				p.Lichen = Mix(&FRidgeParams::Lichen);
				p.Warm = Mix(&FRidgeParams::Warm);
				p.Lean = own.Lean * (1.0f - 2.0f * fmaxf(wl, wr));	// 溝の中心で 0 (両側で連続)

				// 中心線からの距離 (x 2) -> 溝の底 / 壁 / 天辺。壁は広い V 字の斜面で上端に縁、天辺は丸いドーム
				const float edgeN = edgeLow.Sample(u, v);	// 縁のぎざぎざ (縦に長い)
				const float en = s.En + BarkEdgeNoise * 2.0f * (0.6f * (edgeN - 0.5f) + 0.4f * (micro - 0.5f));
				const float tw = Clamp((en - BarkFurrowFloor) / BarkFurrowWall, 0.0f, 1.0f);
				const float ridge = Lerp(tw, tw * tw * (3.0f - 2.0f * tw), 0.3f);		// 0 = 溝の底、1 = 畝の天辺 (V 字の壁)
				const float shallow = SmoothStep(0.55f, 0.8f, shallowLow.Sample(u, v));	// 浅い溝の帯 (底が上がり色も薄い)
				const float floorH = BarkFloor + 0.3f * shallow;

				// ---- 横の割れ目 (天辺を段々のブロックに): 畝ごとの位相 + 行ごとに 0..1 の乱数位置 (隣の畝と揃わない)、
				//      4 割は無い、横にわずかに傾き / うねり、途中で途切れる (ノイズで欠ける)。断面は V 字 ----
				const float by = wv * static_cast<float>(BarkBlockRows) + p.BlockPhase + 0.3f * s.DX * (LatticeHash(id, 21, 11u) - 0.5f) + 0.15f * (micro - 0.5f);
				const int bi = FloorI(by);
				float blockD = 1.0e9f, blockW = BarkBlockCrackW;
				int blockId = bi;
				for (int k = bi - 1; k <= bi + 1; ++k)
				{
					const int wk = WrapInt(k, BarkBlockRows);
					if (LatticeHash(id, wk, 23u) < 0.4f) continue;		// 4 割の割れ目は無い (長いブロック)
					const float cy = static_cast<float>(k) + LatticeHash(id, wk, 25u);
					const float d = fabsf(by - cy);
					if (d < blockD) { blockD = d; blockW = BarkBlockCrackW * (0.6f + 0.8f * LatticeHash(id, wk, 29u)); }
					if (by >= cy) blockId = k;
				}
				// 消えかけの溝の線 (両側で畝の番号が変わる所) ではブロックの明暗 / 割れ目を消す (継ぎ目を出さない)
				const float blockAmt = 1.0f - 2.0f * fmaxf(wl, wr);
				const float crackRun = SmoothStep(0.15f, 0.35f, crackLow.Sample(u, v));	// 割れ目がまれに途切れる
				const float crackT = Clamp(1.0f - blockD / fmaxf(blockW + 0.03f * (micro - 0.5f), 0.01f), 0.0f, 1.0f);
				const float blockCrack = blockAmt * crackRun * SmoothStep(0.2f, 0.5f, ridge) * crackT * crackT * (3.0f - 2.0f * crackT);	// 1 = 割れ目の底 (V 字、壁の上から天辺まで)
				const float blockTone = 1.0f + blockAmt * (0.16f * LatticeHash(id, WrapInt(blockId, BarkBlockRows), 27u) - 0.08f);

				// ---- 天辺の縦の細い裂け目 (畝の上をさらに細かく割る浅い筋。リッジノイズを細く切る) ----
				// ---- 天辺のもろい表面: 欠け (中くらいの FBM を閾値で切った窪み) + 細かな穴 ----
				const float chipN = TileableFBM(u, v, 48, 24, 2, 37u);
				const float chip = SmoothStep(0.56f, 0.70f, chipN) * SmoothStep(0.5f, 0.85f, ridge);
				const float pit = SmoothStep(0.70f, 0.85f, TileableValueNoise(u, v, 160, 112, 43u)) * SmoothStep(0.4f, 0.8f, ridge);

				// ---- 薄片 (天辺の縦長の小さなセル、薄片ごとに明暗。畝ごとに強さが違う) ----
				const float flaky = p.Flaky * SmoothStep(0.55f, 0.9f, ridge);
				float flakeMask = 1.0f, flakeTone = 1.0f;	// flaky = 0 (壁 / 溝) では効かないので評価しない
				if (flaky > 0.0f)
				{
					FPlateLattice::FSample fl;
					flakes.Eval(wu + 0.01f * (micro - 0.5f), wv, fl);
					flakeMask = SmoothStep(0.0f, 0.3f, (fl.F2 - fl.F1) + 0.06f * (micro - 0.5f));	// 0 = 薄片の縁
					flakeTone = flakeTones[static_cast<size_t>(fl.Id1)];
				}

				// ---- 高さ: 断面 (底 -> 壁 -> 天辺 = 畝ごと x うねり x 丸み x 傾き) は法線用に BarkFurrowRelief 倍、
				//      細部 (ブロック / 割れ目 / 裂け目 / 薄片 / 繊維 / 微細) は等倍 ----
				const float swell = 0.88f + 0.24f * swellLow.Sample(u, v);
				const float crest = 1.0f - BarkCrest * (1.0f - SmoothStep(0.4f, 1.1f, en)) + p.Lean * s.DX;
				const float fibre = TileableRidged(u, v, 40, 5, 3, 17u);
				const float flakeH = flaky * (0.03f * (flakeMask - 0.6f) + 0.25f * (flakeTone - 1.0f));
				const float topMacro = p.Height * swell * crest;
				const float topDetail = 0.08f * (blockTone - 1.0f) - 0.28f * blockCrack - 0.10f * chip - 0.05f * pit + flakeH + 0.06f * (fibre - 0.45f) + 0.03f * (micro - 0.5f) + 0.025f * (grain - 0.5f) + 0.03f * (mottle - 0.5f);
				const float floorDetail = 0.03f * (micro - 0.5f) + 0.03f * (fibre - 0.45f) + 0.02f * (grain - 0.5f);
				const float hMacro = Lerp(floorH, topMacro, ridge);
				const float hDetail = Lerp(floorDetail, topDetail, ridge);
				const float h = Clamp(hMacro + hDetail, 0.0f, 1.0f);
				height[t] = h;
				heightN[t] = BarkFurrowRelief * hMacro + hDetail;
				ridgeMap[t] = ridge;
				microMap[t] = micro;

				// ---- 色 (sRGB): 天辺は風化した明るい灰茶、壁は赤みのある茶 (樫の溝の内側)、底は焦げ茶。
				//      天辺 : 底 ≈ 3 : 1 (線形輝度)。環境光だけの幹でも溝の帯が読めるように ----
				const float warm = 0.5f * hueLow.Sample(u, v) + 0.5f * p.Warm;
				const float flakeShade = Lerp(1.0f, Lerp(0.95f, 1.0f, flakeMask) * flakeTone, flaky) * blockTone;
				const float tint = p.Tint * (0.86f + 0.28f * toneLow.Sample(u, v)) * (0.93f + 0.14f * fibre) * (0.86f + 0.28f * grain) * (0.88f + 0.24f * mottle) * flakeShade;
				const float topGrey = SmoothStep(0.25f, 1.0f, ridge) * Lerp(0.5f, 1.0f, mottle);	// 天辺ほど灰色、壁は茶
				float r = Lerp(0.40f, 0.47f, topGrey) * tint * Lerp(0.95f, 1.05f, warm);
				float g = Lerp(0.31f, 0.41f, topGrey) * tint;
				float b = Lerp(0.22f, 0.32f, topGrey) * tint * Lerp(1.06f, 0.90f, warm);
				// 溝: 底から壁の上までなだらかに焦げ茶へ (壁の中ほどで半々、上半分は天辺寄りの明るさ。底は細かな屑で明暗)
				const float fissure = (1.0f - SmoothStep(0.05f, 0.95f, ridge)) * (1.0f - 0.5f * shallow);
				const float floorTone = (0.8f + 0.4f * micro + 0.3f * (grain - 0.5f)) * (0.8f + 0.28f * SmoothStep(0.0f, BarkFurrowFloor + 0.02f, en));	// 底の中心の細い割れ目は暗い (V の奥)
				r = Lerp(r, 0.265f * floorTone, fissure); g = Lerp(g, 0.20f * floorTone, fissure); b = Lerp(b, 0.14f * floorTone, fissure);
				// 割れ目 / 裂け目の中は暗い
				const float crackDark = 1.0f - 0.30f * blockCrack - 0.22f * chip - 0.10f * pit;
				r *= crackDark; g *= crackDark; b *= crackDark;
				// 地衣: まだら x (溝の壁 (風下) + 一部の畝の天辺) x 細かい痂。控えめ
				float lichen = 0.0f;
				const float shelter = 0.8f * SmoothStep(0.15f, 0.4f, ridge) * (1.0f - SmoothStep(0.6f, 0.9f, ridge)) + p.Lichen * SmoothStep(0.7f, 0.95f, ridge);
				const float lichenPatch = SmoothStep(0.47f, 0.63f, lichenLow.Sample(u, v)) * shelter;
				if (lichenPatch > 0.001f) lichen = lichenPatch * SmoothStep(0.44f, 0.6f, 0.5f * TileableFBM(u, v, 64, 32, 2, 77u) + 0.5f * mottle) * SmoothStep(0.3f, 0.6f, grain + 0.5f * micro);
				r = Lerp(r, 0.54f, 0.8f * lichen); g = Lerp(g, 0.58f, 0.8f * lichen); b = Lerp(b, 0.45f, 0.8f * lichen);
				// 苔: 溝の底 / 壁の下のまだら (低周波のまだら x 微細ノイズ)。くすんだオリーブ (緑みは弱く、筋として目立たない)
				const float moss = SmoothStep(0.53f, 0.70f, mossLow.Sample(u, v)) * (1.0f - SmoothStep(0.15f, 0.4f, ridge)) * SmoothStep(0.42f, 0.62f, micro);
				r = Lerp(r, 0.20f, 0.6f * moss); g = Lerp(g, 0.28f, 0.6f * moss); b = Lerp(b, 0.12f, 0.6f * moss);
				lichenMap[t] = lichen;

				diff.Set(x, y, r, g, b, 1.0f);
			}
		}

		// ---- ARM: 溝で暗く (底 BarkAOFloor) x 弱いキャビティ AO (ぼかした高さより低い所。細かい縁と溝全体の 2 スケール)、
		//      ラフネス、メタリック 0 ----
		std::vector<float> blur, blurWide;
		BoxBlurWrap(height, size, BarkAOBlurRadius, blur);
		BoxBlurWrap(height, size, BarkAOBlurRadiusWide, blurWide);
		for (size_t t = 0; t < N; ++t)
		{
			const float ridge = ridgeMap[t];
			const float occlusion = Clamp(((blur[t] > blurWide[t]) ? blur[t] : blurWide[t]) - height[t], 0.0f, 1.0f);
			const float cavity = Clamp(1.0f - 1.2f * occlusion, 0.85f, 1.0f);
			const float ao = Clamp(cavity * Lerp(BarkAOFloor, 1.0f, SmoothStep(0.0f, 0.9f, ridge)), 0.05f, 1.0f);
			float rough = Clamp(Lerp(0.95f, 0.80f, ridge) + 0.04f * (microMap[t] - 0.5f), 0.3f, 0.97f);
			rough = Lerp(rough, 0.88f, lichenMap[t]);
			arm.Set(static_cast<unsigned int>(t % size), static_cast<unsigned int>(t / size), ao, rough, 0.0f, 1.0f);
		}

		// 法線は少しぼかした高さから (ミップ 3〜4 でも畝の斜面の明暗が残るように)
		std::vector<float> heightSoft;
		BoxBlurWrap(heightN, size, BarkNormalBlurRadius, heightSoft);
		HeightToNormal(heightSoft, size, BarkNormalStrength, true, normal);

		return WriteDDS(DiffPath, diff) && WriteDDS(NormalPath, normal) && WriteDDS(ARMPath, arm);
	}

	// ============================================================
	//  小枝の樹皮 (Size², 両方向タイル): 滑らかな若い樹皮 (灰茶 ~ くすんだオリーブ茶の低周波むら。明るさは v5 の幹の
	//  平均 (≈ 0.32) に合わせ、v6 で幹を明るくした後も据え置き (幹よりやや暗い: 明るい棒に見えないように)) + 縦の筋
	//  (繊維、リッジマルチフラクタル 1:6、色と高さの両方にはっきり) + 浅い縦の皺) に皮目 (横長の楕円。Worley の F1 を異方的な距離で取る、半分は欠く) を明るく
	//  わずかに盛り上げて乗せ、周りをわずかに暗く (縁の窪み)。法線は BarkNormalStrength より弱め、ラフネスは皮目で
	//  わずかに低い。幹の畝 / 鱗のような模様は持たない
	// ============================================================
	constexpr int   TwigLenticelsU = 6;			// 皮目の格子 (U = 円周方向)
	constexpr int   TwigLenticelsV = 8;			// 皮目の格子 (V = 軸方向)
	constexpr float TwigLenticelAnisoX = 0.4f;	// 距離の軸倍率 (小さいほどその向きに長い楕円): 256² で約 36 x 11 テクセルの横長
	constexpr float TwigLenticelAnisoY = 1.0f;
	constexpr float TwigFibre = 0.12f;			// 縦の繊維の高さの振幅
	constexpr float TwigNormalStrength = 5.0f;

	bool GenerateTwigTextures(const std::string& DiffPath, const std::string& NormalPath, const std::string& ARMPath, unsigned int Size)
	{
		const unsigned int size = Size;
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

				// 地の色: 灰茶 <-> くすんだオリーブ茶の低周波むら (幹の畝の天辺の平均程度の暗さ)、縦の筋 (U に細かく V に粗い) と
				// 繊維で明暗 (繊維の谷は暗い)
				const float tone = TileableFBM(u, v, 3, 6, 3, 201u);
				const float fibre = TileableRidged(u, v, 24, 4, 3, 209u);
				const float streak = (0.95f + 0.10f * TileableFBM(u, v, 16, 3, 3, 207u)) * (0.84f + 0.32f * fibre);
				float r = Lerp(0.27f, 0.33f, tone) * streak;
				float g = Lerp(0.23f, 0.30f, tone) * streak;
				float b = Lerp(0.17f, 0.22f, tone) * streak;

				// 皮目: 最近の特徴点までの異方的距離が小さい所 (セルの半分は皮目なし)。すぐ外側は少し暗い窪み
				FWorleySample w;
				TileableWorley(u, v, TwigLenticelsU, TwigLenticelsV, 203u, w, TwigLenticelAnisoX, TwigLenticelAnisoY);
				const float present = (LatticeHash(static_cast<int>(w.CellId), 0, 211u) < 0.5f) ? 1.0f : 0.0f;
				const float l = SmoothStep(0.17f, 0.09f, w.F1) * present;
				const float rim = SmoothStep(0.15f, 0.21f, w.F1) * (1.0f - SmoothStep(0.21f, 0.32f, w.F1)) * present;
				r *= 1.0f - 0.10f * rim; g *= 1.0f - 0.10f * rim; b *= 1.0f - 0.10f * rim;
				r = Lerp(r, 0.60f, 0.75f * l); g = Lerp(g, 0.54f, 0.75f * l); b = Lerp(b, 0.43f, 0.75f * l);

				height[static_cast<size_t>(y) * size + x] = 0.5f + 0.15f * l - 0.04f * rim + TwigFibre * (fibre - 0.45f) + 0.06f * (TileableFBM(u, v, 24, 24, 3, 205u) - 0.5f);

				diff.Set(x, y, r, g, b, 1.0f);
				arm.Set(x, y, 0.9f + 0.1f * (1.0f - l), 0.65f - 0.1f * l, 0.0f, 1.0f);
			}
		}

		HeightToNormal(height, size, TwigNormalStrength, true, normal);

		return WriteDDS(DiffPath, diff) && WriteDDS(NormalPath, normal) && WriteDDS(ARMPath, arm);
	}

	// ---- マスク α のランプ (葉 / 房で共用) ----
	// 輪郭までの符号付き距離 dEdge (UV 単位、内側が負) から α = saturate(clip - dEdge / (RampTexels x Texel))。
	// α = LeafCoverageClip の等値線がちょうど輪郭に乗るので、シルエットは作った形のまま、
	// その周りの数テクセル幅のランプがミップ / 拡大時の縁の滑らかさを担う
	inline float RampTexelsForSize(unsigned int Size) { return (Size >= 512) ? 8.0f : 4.0f; }
	inline float AlphaFromEdge(float dEdge, float Texel, float RampTexels)
	{
		return Clamp(FProceduralTreeTextures::LeafCoverageClip - dEdge / (RampTexels * Texel), 0.0f, 1.0f);
	}

	inline int MinI(int A, int B) { return (A < B) ? A : B; }
	inline int MaxI(int A, int B) { return (A > B) ? A : B; }
	inline float Gauss(float D, float W) { const float t = D / fmaxf(W, 1e-5f); return expf(-t * t); }

	// 点 P と線分 A-B の距離。OutT = 線分上の最近点のパラメータ (0..1)
	inline float PointSegmentDistance(float Px, float Py, float Ax, float Ay, float Bx, float By, float& OutT)
	{
		const float dx = Bx - Ax, dy = By - Ay;
		const float len2 = dx * dx + dy * dy;
		const float t = (len2 > 0.0f) ? Clamp(((Px - Ax) * dx + (Py - Ay) * dy) / len2, 0.0f, 1.0f) : 0.0f;
		const float ex = Px - (Ax + dx * t), ey = Py - (Ay + dy * t);
		OutT = t;
		return sqrtf(ex * ex + ey * ey);
	}

	// ============================================================
	//  葉 1 枚の形状 / 模様 (単葉テクスチャと房アトラスで共用)
	//  u in [0,1] (0.5 = 主脈)、v = 0 が葉先 / 1 が葉柄の端 (カード UV と同じ向き)。
	//  形: 樫 (ナラ) の葉。倒卵形 (最大幅は葉先寄り、付け根へ細くなる。葉身 v in [0, BladeEnd]) の半幅に
	//      葉身に沿って LobeCount 周期の丸い裂片を掛ける: 半幅 x (1 - LobeDepth x 深さの揺らぎ x (1 - lobe))、
	//      lobe = 1 - (0.5 - 0.5 cos(2π (vb LobeCount + 位相)))^1.4 (裂片は広く丸く、湾入は狭いが底は丸い)。湾入の深さは
	//      裂片ごとに揺らぎ、葉先 (丸い頂裂片) と付け根では浅い。位相は左右で約 1/4 周期ずらす (互生気味の非対称)。
	//      + 葉柄 (v > PetioleStart の細い帯、葉身の付け根に埋める)。
	//  脈: 主脈 + 片側ごとに裂片の数だけの側脈 (それぞれの裂片の先へ向けて葉先側へ傾き、緩く曲がって縁で終わる)。
	//  Prepare() が輪郭 (行ごとの半幅) と側脈の折れ線 / 外接矩形を前計算し、EvalLeaf は
	//  それらへの正確な 2D 距離を取る (裂片の縁は傾きが急なので「水平距離 / sqrt(1 + 傾き²)」の近似は使わない)。
	//  範囲外の (u, v) も評価できる (房アトラスの逆写像用。輪郭の外として扱う)。
	// ============================================================
	struct FLeafVariant
	{
		static constexpr int   OutlineSamples = 512;	// 輪郭の行サンプル数 (v = k / OutlineSamples、k = 0..OutlineSamples)
		static constexpr int   MaxVeins = 6;			// 片側の側脈の最大数 (裂片ごとに 1 本)
		static constexpr int   VeinSamples = 24;		// 側脈 1 本の折れ線の点数
		static constexpr int   SearchRows = 48;			// 輪郭距離の探索窓 (± 行)。これより遠い輪郭は色 / 高さの帯域 (<= 0.08) の外なので正確さは要らない
		static constexpr float BladeEnd = 0.90f;		// 葉身の終わり (v)。以降は葉柄だけ
		static constexpr float PetioleStart = 0.88f;	// 葉柄の始まり (葉身の付け根に少し埋めて継ぎ目を消す)
		static constexpr float PetioleHalfWidth = 0.016f;
		static constexpr float LobeCount = 4.5f;		// 葉身に沿った裂片の周期数 (葉先と付け根で浅くなるので片側 3〜4 個)
		static constexpr float LobePeak = 0.38f;		// 湾入から次の湾入までのうち山の位置 (0.5 で対称。小さいほど裂片が葉先の方を向く)
		static constexpr float LobeDepth = 0.42f;		// 湾入の深さ (半幅に対する比。裂片ごとに x 0.75..1.15)
		static constexpr float VeinCurve = 0.20f;		// 側脈の曲がり (s² の係数、葉先側へ)
		static constexpr float VeinCullMargin = 0.10f;	// 側脈の外接矩形をこれだけ広げた外のテクセルではその脈の距離を取らない

		// ---- 入力 ----
		unsigned int Seed = 41u;				// 非対称 / 裂片 / 側脈角のゆらぎの種 (房アトラスはインスタンスごとに変える)
		XMFLOAT3 Tint = { 1.0f, 1.0f, 1.0f };	// 最終色に掛ける倍率 (房アトラスの個体差用)
		float WidthScale = 1.0f;				// 輪郭 (葉身 / 裂片 / 葉柄) の幅の倍率。房アトラスは LeafAspect 相当の 0.65 で等方に描く
		float VeinScale = 1.0f;					// 側脈の幅の倍率 (小さく描くインスタンスで脈が 1 テクセル未満の点々にならないように)

		// ---- Prepare() の結果 ----
		std::vector<float> Outline[2];			// [side][k] 半幅 (side 0 = u < 0.5、1 = u > 0.5)
		std::vector<float> OutlineWinMin[2];	// [side][k] 行 k ± SearchRows の半幅の最小 / 最大 (遠いテクセルの探索を省く)
		std::vector<float> OutlineWinMax[2];
		int NumVeins[2] = { 0, 0 };				// 片側の側脈の数
		XMFLOAT2 VeinPts[2][MaxVeins][VeinSamples] = {};	// (du, v)、s = SMax x k / (VeinSamples - 1)
		float VeinSMax[2][MaxVeins] = {};
		float VeinBox[2][MaxVeins][3] = {};		// 側脈の外接矩形 (du の最大、v の最小、v の最大)

		// 裂片の位相 (周期単位)。左右で約 1/4 ずらす
		float LobePhase(int Side) const { return 0.25f * static_cast<float>(Side) + 0.12f * (LatticeHash(Side, 3, Seed) - 0.5f); }

		// 葉身の半幅 (v in (0, BladeEnd) の外は 0)
		float HalfWidth(float V, int Side) const
		{
			if (V <= 0.0f || V >= BladeEnd) return 0.0f;
			const float vb = V / BladeEnd;
			// 倒卵形: sin のべき乗プロファイル、指数 < 1 で最大幅を葉先寄り (vb ≈ 0.4) に置く。葉先は丸く (べき 0.55)、
			// 付け根へは細く絞る (べき 0.9)
			const float hw0 = 0.44f * powf(fmaxf(sinf(PT_PI * powf(vb, 0.76f)), 0.0f), Lerp(0.55f, 0.9f, vb));
			// 左右非対称 (低周波ノイズで片側を太らせ、反対側を細らせる)
			const float asym = 1.0f + 0.06f * (TileableValueNoise(0.0f, V, 1, 6, Seed) - 0.5f);
			float hw = (Side == 0) ? hw0 * asym : hw0 / asym;
			// 裂片: 湾入 (x が半整数) から次の湾入までを t = 0..1 とし、山を t = LobePeak (< 0.5) に寄せる
			// (葉先側の縁が短く急、付け根側が長く緩い = 裂片が外 / 葉先の方を向く)。山は広く平たく丸く、湾入は狭いが
			// 底は丸い (1 - (1 - c)^1.4 は c = 0 で傾きが有限)。深さは山ごとの値を滑らかに補間
			const float x = vb * LobeCount + LobePhase(Side);
			const float t = x + 0.5f - static_cast<float>(FloorI(x + 0.5f));
			const float ts = (t < LobePeak) ? 0.5f * t / LobePeak : 0.5f + 0.5f * (t - LobePeak) / (1.0f - LobePeak);
			const float c = 0.5f - 0.5f * cosf(2.0f * PT_PI * ts);	// 0 = 湾入、1 = 山
			const float lobe = 1.0f - powf(1.0f - c, 1.4f);
			const int xi = FloorI(x);
			const float xf = x - static_cast<float>(xi);
			const float d0 = 0.75f + 0.4f * LatticeHash(xi, Side, Seed + 5u), d1 = 0.75f + 0.4f * LatticeHash(xi + 1, Side, Seed + 5u);
			const float depth = LobeDepth * Lerp(d0, d1, xf * xf * (3.0f - 2.0f * xf));
			const float fade = SmoothStep(0.03f, 0.2f, vb) * SmoothStep(1.0f, 0.78f, vb);	// 葉先と付け根では浅い
			hw *= 1.0f - depth * fade * (1.0f - lobe);
			return fmaxf(hw, 0.0f) * WidthScale;
		}

		// 側脈の起点 Vi からの距離 s に対する点 (du, v)。葉先側へ Angle 傾け、s² で更に葉先へ曲げる
		static void VeinPoint(float S, float Vi, float Angle, float& OutDu, float& OutV)
		{
			OutDu = S * sinf(Angle);
			OutV = Vi - S * cosf(Angle) - VeinCurve * S * S;
		}

		void Prepare()
		{
			for (int side = 0; side < 2; ++side)
			{
				Outline[side].resize(OutlineSamples + 1);
				for (int k = 0; k <= OutlineSamples; ++k)
				{
					Outline[side][k] = HalfWidth(static_cast<float>(k) / static_cast<float>(OutlineSamples), side);
				}
				OutlineWinMin[side].resize(OutlineSamples + 1);
				OutlineWinMax[side].resize(OutlineSamples + 1);
				for (int k = 0; k <= OutlineSamples; ++k)
				{
					float mn = 1e9f, mx = -1e9f;
					for (int j = MaxI(k - SearchRows, 0); j <= MinI(k + SearchRows + 1, OutlineSamples); ++j)
					{
						mn = fminf(mn, Outline[side][j]);
						mx = fmaxf(mx, Outline[side][j]);
					}
					OutlineWinMin[side][k] = mn;
					OutlineWinMax[side][k] = mx;
				}
			}

			// 側脈: 片側の裂片の山ごとに 1 本。裂片の先 (山の v、その半幅) に届くように起点を逆算する
			// (角度は葉先寄りの裂片ほど鋭く 38 度 -> 付け根寄り 62 度、± 6 度のゆらぎ)。縁に届く s は二分で求める
			for (int side = 0; side < 2; ++side)
			{
				NumVeins[side] = 0;
				const float phase = LobePhase(side);
				for (int n = 1; n <= 8 && NumVeins[side] < MaxVeins; ++n)
				{
					const float vbn = (static_cast<float>(n) - 0.5f + LobePeak - phase) / LobeCount;	// 山の位置
					if (vbn < 0.12f) continue;
					if (vbn > 0.86f) break;
					const int i = NumVeins[side]++;
					const float vLobe = vbn * BladeEnd;
					const float hwLobe = HalfWidth(vLobe, side);
					const float angle = (Lerp(38.0f, 62.0f, vbn) + 6.0f * (2.0f * LatticeHash(n, 1 + side, Seed) - 1.0f)) * (PT_PI / 180.0f);
					const float sReach = hwLobe / sinf(angle);
					const float vi = fminf(vLobe + sReach * cosf(angle) + VeinCurve * sReach * sReach, BladeEnd - 0.03f);

					auto Outside = [&](float s) { float du, v; VeinPoint(s, vi, angle, du, v); return du >= HalfWidth(v, side); };
					float s0 = 0.0f, s1 = 0.0f;
					for (int k = 1; k <= 200; ++k)
					{
						s1 = 0.005f * static_cast<float>(k);
						if (Outside(s1)) break;
						s0 = s1;
					}
					for (int k = 0; k < 12; ++k)
					{
						const float m = 0.5f * (s0 + s1);
						if (Outside(m)) s1 = m; else s0 = m;
					}
					const float sMax = fmaxf(0.5f * (s0 + s1), 0.01f);
					VeinSMax[side][i] = sMax;
					float duMax = 0.0f, vMin = 1e9f, vMax = -1e9f;
					for (int k = 0; k < VeinSamples; ++k)
					{
						float du, v;
						VeinPoint(sMax * static_cast<float>(k) / static_cast<float>(VeinSamples - 1), vi, angle, du, v);
						VeinPts[side][i][k] = XMFLOAT2(du, v);
						duMax = fmaxf(duMax, du); vMin = fminf(vMin, v); vMax = fmaxf(vMax, v);
					}
					VeinBox[side][i][0] = duMax;
					VeinBox[side][i][1] = vMin;
					VeinBox[side][i][2] = vMax;
				}
			}
		}
	};

	struct FLeafSample
	{
		float R = 0.0f, G = 0.0f, B = 0.0f;	// 色 (sRGB)。輪郭の外にも書く (α 0 の下の色はミップで滲むので葉の色のまま)
		float Edge = 0.0f;					// 輪郭までの符号付き距離 (葉 UV 単位、内側が負)。α は AlphaFromEdge で作る
		float Height = 0.0f;				// ハイトフィールド (法線用、おおよそ -0.9..0.45)
		float AO = 1.0f;					// ARM.r
		float Rough = 0.5f;					// ARM.g
		float Vein = 0.0f;					// 脈のマスク (0..1、主脈 + 側脈)
		float Margin = 0.0f;				// 縁 (約 3 テクセル) / 葉柄のマスク (ラフネス 0.70 の帯)
	};

	// 葉 1 点の評価。Var は Prepare() 済みであること
	void EvalLeaf(float u, float v, const FLeafVariant& Var, FLeafSample& Out)
	{
		constexpr int N = FLeafVariant::OutlineSamples;
		constexpr float invN = 1.0f / static_cast<float>(N);
		const int side = (u < 0.5f) ? 0 : 1;
		const float du = fabsf(u - 0.5f);
		const std::vector<float>& outline = Var.Outline[side];

		// ---- 葉身の輪郭までの符号付き距離 ----
		// 同じ行の半幅との差 (水平距離) を上限に、近傍の行の輪郭線分への距離を探す
		float hwRow = 0.0f;
		float dBlade = 1.0f;
		bool bInside = false;
		if (v < 0.0f)
		{
			dBlade = sqrtf(du * du + v * v);	// 葉先の頂点まで
		}
		else if (v <= 1.0f)
		{
			const float fk = v * static_cast<float>(N);
			const int kc = MinI(static_cast<int>(fk), N - 1);
			hwRow = Lerp(outline[kc], outline[kc + 1], fk - static_cast<float>(kc));
			bInside = (du < hwRow);
			float dmin = fabsf(du - hwRow);

			// 窓内の輪郭が帯域より確実に遠ければ水平距離で済ませる
			const float band = static_cast<float>(FLeafVariant::SearchRows) * invN;
			const bool bFar = bInside ? (Var.OutlineWinMin[side][kc] - du > band) : (du - Var.OutlineWinMax[side][kc] > band);
			if (!bFar)
			{
				// 行 kc から外側へ探し、残りの行が今の最小より確実に遠くなったら打ち切る (裂片の内側の点は窓の端まで
				// 回らずに済む。候補の集合は窓 ± rows のままなので結果は端から順に回すのと同じ)
				const int rows = MinI(static_cast<int>(ceilf(dmin * static_cast<float>(N))) + 1, FLeafVariant::SearchRows);
				const int k0 = MaxI(kc - rows, 0), k1 = MinI(kc + rows, N - 1);
				for (int r = 0; r <= rows; ++r)
				{
					if (static_cast<float>(r - 1) * invN > dmin + invN) break;
					for (int sgn = -1; sgn <= 1; sgn += 2)
					{
						if (r == 0 && sgn > 0) continue;
						const int k = kc + sgn * r;
						if (k < k0 || k > k1) continue;
						const float vk = static_cast<float>(k) * invN;
						if (fabsf(v - vk) > dmin + invN) continue;	// この線分は今の最小より確実に遠い
						float t;
						dmin = fminf(dmin, PointSegmentDistance(du, v, outline[k], vk, outline[k + 1], vk + invN, t));
					}
				}
			}
			dBlade = bInside ? -dmin : dmin;
		}

		// ---- 葉柄 (矩形の SDF) との和集合 ----
		const float petioleHW = FLeafVariant::PetioleHalfWidth * Var.WidthScale;
		float dPetiole;
		{
			const float dx = du - petioleHW;
			const float dy = fmaxf(FLeafVariant::PetioleStart - v, v - 1.0f);
			dPetiole = (dx > 0.0f && dy > 0.0f) ? sqrtf(dx * dx + dy * dy) : fmaxf(dx, dy);
		}
		const float dEdge = fminf(dBlade, dPetiole);
		const float vc = Clamp(v, 0.0f, 1.0f);

		// ---- 脈: 主脈は |du|、側脈は起点を推定して近い 3 本だけ折れ線距離を取る ----
		const float dMid = du;
		const float wM = 0.012f * (1.0f - 0.6f * (1.0f - vc));
		float dVein = 1.0f, dVein2 = 1.0f, wV = 0.004f;
		if (dBlade < 0.02f && v > 0.0f && v < FLeafVariant::BladeEnd)
		{
			// 側脈は外接矩形 (+ VeinCullMargin) に入るものだけ折れ線距離を取る (裂片ごとに 1 本なので片側 3〜4 本)
			for (int i = 0; i < Var.NumVeins[side]; ++i)
			{
				const float* box = Var.VeinBox[side][i];
				if (du > box[0] + FLeafVariant::VeinCullMargin || v < box[1] - FLeafVariant::VeinCullMargin || v > box[2] + FLeafVariant::VeinCullMargin) continue;
				const XMFLOAT2* p = Var.VeinPts[side][i];
				float best = 1e9f, bestS = 0.0f;
				for (int k = 0; k + 1 < FLeafVariant::VeinSamples; ++k)
				{
					float t;
					const float d = PointSegmentDistance(du, v, p[k].x, p[k].y, p[k + 1].x, p[k + 1].y, t);
					if (d < best)
					{
						best = d;
						bestS = (static_cast<float>(k) + t) / static_cast<float>(FLeafVariant::VeinSamples - 1);	// s / sMax
					}
				}
				if (best < dVein)
				{
					dVein2 = dVein;
					dVein = best;
					wV = fmaxf(0.004f * (1.0f - bestS), 0.0015f) * Var.VeinScale;	// 縁へ向かって細くなる (1 テクセル未満にはしない: 末端が点々になる)
				}
				else if (best < dVein2)
				{
					dVein2 = best;
				}
			}
		}
		const float gM = Gauss(dMid, wM);
		const float gV = Gauss(dVein, wV);
		const float veinMask = Clamp(gM + 0.6f * gV, 0.0f, 1.0f);

		// ---- 高さ: 葉身のドーム - 主脈 / 側脈の溝 + 脈間の膨らみ + 細かな凹凸 - 縁の垂れ ----
		const float hwEff = (v > FLeafVariant::PetioleStart) ? fmaxf(hwRow, petioleHW) : hwRow;
		const float ratio = (hwEff > 1e-4f) ? Clamp(du / hwEff, 0.0f, 1.0f) : 1.0f;
		const float dome = 1.0f - ratio * ratio;
		const float between = sinf(PT_PI * dVein / fmaxf(dVein + dVein2, 1e-5f));	// 0 = 脈上、1 = 隣の脈との中間
		const float inBlade = SmoothStep(0.0f, -0.02f, dEdge);
		const float h = 0.30f * dome - 0.55f * gM - 0.30f * gV
			+ 0.12f * between * between * inBlade
			+ 0.05f * (TileableFBM(u, v, 32, 32, 3, 61u) - 0.5f)
			- 0.35f * SmoothStep(-0.06f, 0.0f, dEdge);

		// ---- ARM: 脈の溝と付け根で AO を落とす。ラフネスは葉身 0.55、脈 0.65、縁 (約 3 テクセル) と葉柄 0.70
		//      (カードがつやつやの銀色に光らないように、葉身もやや粗め) ----
		const float ao = Clamp(1.0f - 0.25f * Gauss(dMid, 2.0f * wM) - 0.15f * Gauss(dVein, 2.0f * wV) - 0.2f * SmoothStep(0.75f, 1.0f, vc), 0.05f, 1.0f);
		const float marginMask = SmoothStep(-4.0f / 512.0f, -2.0f / 512.0f, dEdge);
		const float petioleMask = SmoothStep(FLeafVariant::PetioleStart, FLeafVariant::BladeEnd + 0.01f, v);
		const float rough = Lerp(Lerp(0.55f, 0.65f, veinMask), 0.70f, fmaxf(marginMask, petioleMask));

		// ---- 色 (sRGB) ----
		const float shade = 0.92f + 0.14f * TileableFBM(u, v, 6, 6, 3, 31u);
		float r = Lerp(0.27f, 0.36f, dome) * shade;
		float g = Lerp(0.44f, 0.52f, dome) * shade;
		float b = Lerp(0.13f, 0.17f, dome) * shade;
		// 黄色いまだら
		const float mottle = 0.35f * SmoothStep(0.6f, 0.75f, TileableFBM(u, v, 9, 9, 2, 73u));
		r = Lerp(r, 0.55f, mottle); g = Lerp(g, 0.58f, mottle); b = Lerp(b, 0.20f, mottle);
		// 脈は明るく (黄みがかる)
		r *= Lerp(1.0f, 1.14f, veinMask); g *= Lerp(1.0f, 1.08f, veinMask); b *= Lerp(1.0f, 0.82f, veinMask);
		// 縁: 内側の帯は明るい黄緑、際は茶。茶は輪郭の外へ数テクセルで葉の色へ戻す (ミップで滲んでも茶の縞にならない)
		const float band = 0.4f * SmoothStep(-0.08f, -0.012f, dEdge);
		r = Lerp(r, 0.46f, band); g = Lerp(g, 0.54f, band); b = Lerp(b, 0.18f, band);
		const float rim = SmoothStep(-0.012f, 0.0f, dEdge) * SmoothStep(0.03f, 0.006f, dEdge);
		r = Lerp(r, 0.40f, rim); g = Lerp(g, 0.30f, rim); b = Lerp(b, 0.12f, rim);
		// 葉柄
		r = Lerp(r, 0.45f, petioleMask); g = Lerp(g, 0.38f, petioleMask); b = Lerp(b, 0.22f, petioleMask);

		Out.R = r * Var.Tint.x;
		Out.G = g * Var.Tint.y;
		Out.B = b * Var.Tint.z;
		Out.Edge = dEdge;
		Out.Height = h;
		Out.AO = ao;
		Out.Rough = rough;
		Out.Vein = veinMask;
		Out.Margin = fmaxf(marginMask, petioleMask);
	}

	constexpr float LeafNormalStrength = 3.0f;	// 脈の起伏の法線の強さ (強いとカードの照り返しがぎらつく)

	// α < 0.05 のテクセルの法線を平坦にする (葉の外の高さの揺らぎを法線に出さない)
	void FlattenNormalUnderAlpha(FImageRGBA& Normal, const FImageRGBA& Diff)
	{
		for (size_t i = 0; i < Normal.Pixels.size() / 4; ++i)
		{
			if (Diff.Pixels[i * 4 + 3] < 13)	// 0.05 x 255
			{
				Normal.Pixels[i * 4 + 0] = 128;
				Normal.Pixels[i * 4 + 1] = 128;
				Normal.Pixels[i * 4 + 2] = 255;
			}
		}
	}

	// 正方の画像 1 枚分を EvalLeaf で埋める。(u, v) = ((x + 0.5) / Size - UVOffset) / UVSpan、
	// 距離は葉 UV -> 画像 UV へ UVSpan 倍して α にする。法線は高さから (端はクランプ)、α 0 の所は平坦
	void RasterizeLeaf(const FLeafVariant& Var, unsigned int Size, float UVOffset, float UVSpan, FImageRGBA& Diff, FImageRGBA& Normal, FImageRGBA& ARM)
	{
		const float texel = 1.0f / static_cast<float>(Size);
		const float ramp = RampTexelsForSize(Size);
		std::vector<float> height(static_cast<size_t>(Size) * Size);

		for (unsigned int y = 0; y < Size; ++y)
		{
			for (unsigned int x = 0; x < Size; ++x)
			{
				const float u = ((static_cast<float>(x) + 0.5f) * texel - UVOffset) / UVSpan;
				const float v = ((static_cast<float>(y) + 0.5f) * texel - UVOffset) / UVSpan;

				FLeafSample s;
				EvalLeaf(u, v, Var, s);
				Diff.Set(x, y, s.R, s.G, s.B, AlphaFromEdge(s.Edge * UVSpan, texel, ramp));
				ARM.Set(x, y, s.AO, s.Rough, 0.0f, 1.0f);
				height[static_cast<size_t>(y) * Size + x] = s.Height;
			}
		}

		HeightToNormal(height, Size, LeafNormalStrength, false, Normal);
		FlattenNormalUnderAlpha(Normal, Diff);
	}

	// ---- 葉 (512²: BaseColor α = 輪郭の SDF ランプ、法線 = 脈のハイトフィールド、ARM = AO / ラフネス / 0) ----
	bool GenerateLeafTextures(const std::string& DiffPath, const std::string& NormalPath, const std::string& ARMPath, unsigned int Size)
	{
		FImageRGBA diff(Size, Size);
		FImageRGBA normal(Size, Size);
		FImageRGBA arm(Size, Size);

		FLeafVariant leaf;
		leaf.Prepare();
		RasterizeLeaf(leaf, Size, 0.0f, 1.0f, diff, normal, arm);

		// BaseColor の α だけカバレッジ保存ミップ (clip = マテリアルの OpacityMaskClipValue)
		return WriteDDS(DiffPath, diff, FProceduralTreeTextures::LeafCoverageClip) && WriteDDS(NormalPath, normal) && WriteDDS(ARMPath, arm);
	}

	// ============================================================
	//  葉の房アトラス (1024², 2x2 セル、セル c は ((c & 1), (c >> 1)) x 512)
	//  各セル: 下中央から上へ伸びる仮想の小枝に 5 + (c & 1) 枚の葉を左右交互に付ける。
	//  テクセルを各インスタンスの葉 UV へ逆写像して EvalLeaf を評価し、画家のアルゴリズムで
	//  合成する (後の番号が手前: α > 0.5 なら上書き、それ以外は α で lerp、α は max)。
	//  実装は手前から奥へ回り、α > 0.5 のインスタンスに当たったら残りの評価を省く。
	//  法線は所有インスタンス (手前で α > 0.5、無ければ手前で α >= 0.05) の高さ場から取り、
	//  差分は同じインスタンスのテクセルだけで組む (重なりの境目に段差を出さない) +
	//  インスタンスごとの傾き (TiltX, TiltY) を勾配に足す = 房カードを立体に見せる偽の傾き。
	//  セルの縁 24 テクセルは透明なガター。葉はランプ込みでガターの内側に収まるように
	//  長さをクランプする。種は 7000 + c で Tree.Seed とは無関係。
	//  カード側の UV: uv = cellOrigin + AtlasCellInset + (u, v) x (0.5 - 2 AtlasCellInset)、V = 0 が房の先 (セル上端)
	// ============================================================
	constexpr unsigned int ClusterAtlasSize = 1024;
	constexpr unsigned int ClusterCellSize = ClusterAtlasSize / 2;
	constexpr unsigned int ClusterGutterTexels = 24;
	constexpr float ClusterLeafWidthScale = 0.65f;	// LeafAspect 相当 (単葉カードに貼った葉と同じ縦横比)
	constexpr float ClusterTwigHalfWidthTexels = 1.5f;	// 小枝の線は 3 テクセル幅

	struct FClusterInstance
	{
		FLeafVariant Var;
		XMFLOAT2 Base = { 0.5f, 1.0f };		// 葉柄の端 (セル UV)。小枝の上に乗る
		XMFLOAT2 Dir = { 0.0f, -1.0f };		// 葉先の向き (単位、画像座標: y は下向き)
		XMFLOAT2 Perp = { 1.0f, 0.0f };		// 葉 UV の +u の向き
		float Scale = 0.5f;					// 葉の長さ (セル UV)。葉 UV は等方にこの倍率で写る
		float TiltX = 0.0f, TiltY = 0.0f;	// 法線の傾き (接線空間、アトラスの x / y)
		float AOLayer = 1.0f;				// 下の層 (先に描く葉) ほど暗くする AO
		float MinX = 0.0f, MinY = 0.0f, MaxX = 0.0f, MaxY = 0.0f;	// 外接矩形 (セル UV、ランプ込み)
	};

	// セル c のインスタンスと小枝の線分 (セル UV) を決める
	void BuildClusterInstances(unsigned int Cell, std::vector<FClusterInstance>& Out, XMFLOAT2& OutTwigBase, XMFLOAT2& OutTwigEnd)
	{
		const unsigned int seed = 7000u + Cell;
		const int n = 5 + static_cast<int>(Cell & 1u);
		const float cellTexel = 1.0f / static_cast<float>(ClusterCellSize);
		const float gutter = static_cast<float>(ClusterGutterTexels) * cellTexel;
		const float margin = (RampTexelsForSize(ClusterCellSize) + 2.0f) * cellTexel;	// α ランプ + 余裕
		const float lo = gutter + margin, hi = 1.0f - gutter - margin;
		const float deg = PT_PI / 180.0f;

		// 小枝: 下中央から上へ。セルごとにわずかに傾ける。葉は s = 0.15..0.95 の位置に付く
		const float lean = (LatticeHash(0, 9, seed) - 0.5f) * 12.0f * deg;
		const XMFLOAT2 twigDir(sinf(lean), -cosf(lean));
		const XMFLOAT2 twigBase(0.5f, 1.0f - gutter - 0.02f);
		const float twigLen = 0.50f;
		const int flip = static_cast<int>((Cell >> 1) & 1u);	// 最初の葉の側をセルの行で入れ替える

		Out.clear();
		Out.resize(static_cast<size_t>(n));
		float sMax = 0.0f;
		for (int i = 0; i < n; ++i)
		{
			FClusterInstance& inst = Out[static_cast<size_t>(i)];
			auto H = [&](int k) { return LatticeHash(i, k, seed); };
			const float t = (n > 1) ? static_cast<float>(i) / static_cast<float>(n - 1) : 0.0f;	// 0 = 根元、1 = 先端
			const float s = Clamp(0.15f + 0.8f * static_cast<float>(i) / static_cast<float>(n) + (H(0) - 0.5f) * 0.08f, 0.05f, 0.98f);
			sMax = fmaxf(sMax, s);
			const float side = ((i + flip) & 1) ? -1.0f : 1.0f;
			// 根元の葉は横 (55 度)、先端の葉はほぼ上向き (18 度)。±8 度のゆらぎ
			const float angle = side * (Lerp(55.0f, 18.0f, t) + (H(1) - 0.5f) * 16.0f) * deg + lean;

			inst.Base = XMFLOAT2(twigBase.x + twigDir.x * s * twigLen, twigBase.y + twigDir.y * s * twigLen);
			inst.Dir = XMFLOAT2(sinf(angle), -cosf(angle));
			inst.Perp = XMFLOAT2(cosf(angle), sinf(angle));

			inst.Var.Seed = seed * 16u + static_cast<unsigned int>(i);
			inst.Var.WidthScale = ClusterLeafWidthScale;
			inst.Var.VeinScale = 2.2f;	// 葉が約 0.45 倍で描かれるので側脈の幅を補う
			// 色相の倍率 (R を上げ B を下げると黄緑、逆で青緑) と、下の層ほど暗い明るさ
			const float hue = Lerp(0.92f, 1.08f, H(3));
			const float bright = Lerp(0.84f, 1.0f, t);
			inst.Var.Tint = XMFLOAT3(hue * bright, bright, (2.0f - hue) * bright);
			inst.Var.Prepare();

			inst.TiltX = -side * Lerp(0.15f, 0.35f, H(4));				// 外側 (葉先の側) へ向く
			inst.TiltY = Lerp(-0.20f, 0.35f, t) + (H(5) - 0.5f) * 0.10f;	// 上の葉は上向き、下の葉は下向き
			inst.AOLayer = Lerp(0.88f, 1.0f, t);

			// 長さ: 希望値をガター + ランプの内側に収まる最大値でクランプ (輪郭を粗くサンプルして各軸で制限)
			float scale = Lerp(0.42f, 0.52f, H(2));
			float lmax = 4.0f;
			auto Limit = [&](float b, float q)
				{
					if (q > 1e-4f) lmax = fminf(lmax, (hi - b) / q);
					else if (q < -1e-4f) lmax = fminf(lmax, (lo - b) / q);
				};
			constexpr int step = 16;
			for (int k = 0; k <= FLeafVariant::OutlineSamples; k += step)
			{
				const float v = static_cast<float>(k) / static_cast<float>(FLeafVariant::OutlineSamples);
				for (int sd = 0; sd < 2; ++sd)
				{
					const float du = (sd == 0 ? -1.0f : 1.0f) * inst.Var.Outline[sd][static_cast<size_t>(k)];
					const float qx = (1.0f - v) * inst.Dir.x + du * inst.Perp.x;
					const float qy = (1.0f - v) * inst.Dir.y + du * inst.Perp.y;
					Limit(inst.Base.x, qx);
					Limit(inst.Base.y, qy);
				}
			}
			inst.Scale = fmaxf(fminf(scale, lmax), 0.05f);

			// 外接矩形 (ランプ分を足す)
			inst.MinX = inst.MaxX = inst.Base.x;
			inst.MinY = inst.MaxY = inst.Base.y;
			for (int k = 0; k <= FLeafVariant::OutlineSamples; k += step)
			{
				const float v = static_cast<float>(k) / static_cast<float>(FLeafVariant::OutlineSamples);
				for (int sd = 0; sd < 2; ++sd)
				{
					const float du = (sd == 0 ? -1.0f : 1.0f) * fmaxf(inst.Var.Outline[sd][static_cast<size_t>(k)], FLeafVariant::PetioleHalfWidth * ClusterLeafWidthScale);
					const float px = inst.Base.x + inst.Scale * ((1.0f - v) * inst.Dir.x + du * inst.Perp.x);
					const float py = inst.Base.y + inst.Scale * ((1.0f - v) * inst.Dir.y + du * inst.Perp.y);
					inst.MinX = fminf(inst.MinX, px); inst.MaxX = fmaxf(inst.MaxX, px);
					inst.MinY = fminf(inst.MinY, py); inst.MaxY = fmaxf(inst.MaxY, py);
				}
			}
			inst.MinX -= margin; inst.MaxX += margin; inst.MinY -= margin; inst.MaxY += margin;
		}

		OutTwigBase = twigBase;
		OutTwigEnd = XMFLOAT2(twigBase.x + twigDir.x * sMax * twigLen, twigBase.y + twigDir.y * sMax * twigLen);
	}

	// セル 1 枚 (512²) を描く
	void RasterizeClusterCell(unsigned int Cell, FImageRGBA& Diff, FImageRGBA& Normal, FImageRGBA& ARM)
	{
		const unsigned int S = ClusterCellSize;
		const float texel = 1.0f / static_cast<float>(S);
		const float ramp = RampTexelsForSize(S);
		const size_t N = static_cast<size_t>(S) * S;

		std::vector<FClusterInstance> inst;
		XMFLOAT2 twigA, twigB;
		BuildClusterInstances(Cell, inst, twigA, twigB);
		const int n = static_cast<int>(inst.size());

		// 作業バッファ。背景色は葉身の平均色 (α 0 の下の色がミップで滲んでも灰色の縁にならない)
		std::vector<float> rgb(N * 3), alpha(N, 0.0f), height(N, 0.0f), ao(N, 1.0f), rough(N, 0.55f);
		std::vector<int> id(N, -1);	// -1 = 無し、-2 = 小枝、>= 0 = インスタンス
		for (size_t t = 0; t < N; ++t) { rgb[t * 3] = 0.30f; rgb[t * 3 + 1] = 0.46f; rgb[t * 3 + 2] = 0.15f; }

		// ---- 小枝の線 (3 テクセル幅、茶、α 1、外側 1.5 テクセルで落とす)。葉より先に描く (葉が上に重なる) ----
		{
			const float brown[3] = { 0.30f, 0.22f, 0.14f };
			for (unsigned int y = 0; y < S; ++y)
			{
				for (unsigned int x = 0; x < S; ++x)
				{
					float tt;
					const float d = PointSegmentDistance((static_cast<float>(x) + 0.5f) * texel, (static_cast<float>(y) + 0.5f) * texel, twigA.x, twigA.y, twigB.x, twigB.y, tt) * static_cast<float>(S);
					const float a = Clamp(1.0f - (d - ClusterTwigHalfWidthTexels) / ClusterTwigHalfWidthTexels, 0.0f, 1.0f);
					if (a <= 0.0f) continue;
					const size_t t = static_cast<size_t>(y) * S + x;
					for (int c = 0; c < 3; ++c) rgb[t * 3 + c] = (a > 0.5f) ? brown[c] : Lerp(rgb[t * 3 + c], brown[c], a);
					alpha[t] = fmaxf(alpha[t], a);
					if (a > 0.5f) { id[t] = -2; ao[t] = 0.80f; rough[t] = 0.65f; }
				}
			}
		}

		// ---- 葉: 手前 (大きい番号) から奥へ ----
		struct FPartial { float R, G, B, A; };
		for (unsigned int y = 0; y < S; ++y)
		{
			for (unsigned int x = 0; x < S; ++x)
			{
				const size_t t = static_cast<size_t>(y) * S + x;
				const float px = (static_cast<float>(x) + 0.5f) * texel;
				const float py = (static_cast<float>(y) + 0.5f) * texel;

				FPartial parts[8];
				int np = 0;
				int ownerOpaque = -1, ownerPartial = -1;
				FLeafSample sOpaque, sPartial;
				float lvOpaque = 0.0f, lvPartial = 0.0f;
				float aMax = alpha[t];
				for (int i = n - 1; i >= 0; --i)
				{
					const FClusterInstance& I = inst[static_cast<size_t>(i)];
					if (px < I.MinX || px > I.MaxX || py < I.MinY || py > I.MaxY) continue;
					const float qx = px - I.Base.x, qy = py - I.Base.y;
					const float lv = 1.0f - (qx * I.Dir.x + qy * I.Dir.y) / I.Scale;
					const float lu = 0.5f + (qx * I.Perp.x + qy * I.Perp.y) / I.Scale;
					const float m = (ramp + 2.0f) * texel / I.Scale;	// 葉 UV でのランプ幅
					if (lu < -m || lu > 1.0f + m || lv < -m || lv > 1.0f + m) continue;

					FLeafSample s;
					EvalLeaf(lu, lv, I.Var, s);
					const float a = AlphaFromEdge(s.Edge * I.Scale, texel, ramp);
					if (a <= 0.0f) continue;
					aMax = fmaxf(aMax, a);
					if (a > 0.5f)
					{
						ownerOpaque = i; sOpaque = s; lvOpaque = lv;
						break;	// これより奥は見えない
					}
					if (a >= 0.05f && ownerPartial < 0) { ownerPartial = i; sPartial = s; lvPartial = lv; }
					if (np < 8) parts[np++] = { s.R, s.G, s.B, a };
				}

				// 奥から手前へ合成: 土台 (α > 0.5 の葉、無ければ小枝 / 背景) に半透明の縁を lerp で重ねる
				float r = rgb[t * 3], g = rgb[t * 3 + 1], b = rgb[t * 3 + 2];
				if (ownerOpaque >= 0) { r = sOpaque.R; g = sOpaque.G; b = sOpaque.B; }
				for (int j = np - 1; j >= 0; --j)
				{
					r = Lerp(r, parts[j].R, parts[j].A); g = Lerp(g, parts[j].G, parts[j].A); b = Lerp(b, parts[j].B, parts[j].A);
				}
				rgb[t * 3] = r; rgb[t * 3 + 1] = g; rgb[t * 3 + 2] = b;
				alpha[t] = aMax;

				// 高さ / ARM の所有者: 手前で α > 0.5 の葉、無ければ手前で α >= 0.05 の葉 (小枝の上では小枝のまま)
				const int owner = (ownerOpaque >= 0) ? ownerOpaque : (id[t] != -2) ? ownerPartial : -1;
				if (owner >= 0)
				{
					const FLeafSample& s = (ownerOpaque >= 0) ? sOpaque : sPartial;
					const float lv = (ownerOpaque >= 0) ? lvOpaque : lvPartial;
					const FClusterInstance& I = inst[static_cast<size_t>(owner)];
					id[t] = owner;
					height[t] = s.Height * I.Scale;	// 葉が小さい分だけ起伏も浅く (単葉と同じ程度の法線の傾きに)
					// AO: 葉の AO x 葉柄側 0.85 -> 葉先 1 x 層 (下ほど暗い)
					ao[t] = Clamp(s.AO * Lerp(0.85f, 1.0f, 1.0f - Clamp(lv, 0.0f, 1.0f)) * I.AOLayer, 0.10f, 1.0f);
					// ラフネス: 葉身 0.55 / 脈 0.65 / 縁と葉柄 0.70 (単葉と同じ)
					rough[t] = Lerp(Lerp(0.55f, 0.65f, s.Vein), 0.70f, s.Margin);
				}
			}
		}

		// ---- ガター: セルの縁 24 テクセルは透明、法線平坦、ARM (1, 0.55, 0) ----
		for (unsigned int y = 0; y < S; ++y)
		{
			for (unsigned int x = 0; x < S; ++x)
			{
				if (x >= ClusterGutterTexels && x < S - ClusterGutterTexels && y >= ClusterGutterTexels && y < S - ClusterGutterTexels) continue;
				const size_t t = static_cast<size_t>(y) * S + x;
				alpha[t] = 0.0f; id[t] = -1; ao[t] = 1.0f; rough[t] = 0.55f;
			}
		}

		// ---- α < 0.05 の RGB を最寄りの描かれたテクセルから膨張 (8 近傍 x 8 回)。ミップで滲む色を葉の縁の色にする ----
		{
			std::vector<unsigned char> filled(N), next(N);
			for (size_t t = 0; t < N; ++t) filled[t] = (alpha[t] >= 0.05f) ? 1 : 0;
			std::vector<float> src(N * 3);
			for (int it = 0; it < 8; ++it)
			{
				src = rgb;
				next = filled;
				for (int y = 0; y < static_cast<int>(S); ++y)
				{
					for (int x = 0; x < static_cast<int>(S); ++x)
					{
						const size_t t = static_cast<size_t>(y) * S + static_cast<size_t>(x);
						if (filled[t]) continue;
						float sr = 0.0f, sg = 0.0f, sb = 0.0f;
						int cnt = 0;
						for (int dy = -1; dy <= 1; ++dy)
						{
							for (int dx = -1; dx <= 1; ++dx)
							{
								const int nx = x + dx, ny = y + dy;
								if ((dx == 0 && dy == 0) || nx < 0 || ny < 0 || nx >= static_cast<int>(S) || ny >= static_cast<int>(S)) continue;
								const size_t u = static_cast<size_t>(ny) * S + static_cast<size_t>(nx);
								if (!filled[u]) continue;
								sr += src[u * 3]; sg += src[u * 3 + 1]; sb += src[u * 3 + 2];
								++cnt;
							}
						}
						if (cnt > 0)
						{
							const float inv = 1.0f / static_cast<float>(cnt);
							rgb[t * 3] = sr * inv; rgb[t * 3 + 1] = sg * inv; rgb[t * 3 + 2] = sb * inv;
							next[t] = 1;
						}
					}
				}
				filled.swap(next);
			}
		}

		// ---- 法線: 所有インスタンス内だけで高さの差分を取り (無い側は片側差分)、インスタンスの傾きを勾配に足す ----
		for (unsigned int y = 0; y < S; ++y)
		{
			for (unsigned int x = 0; x < S; ++x)
			{
				const size_t t = static_cast<size_t>(y) * S + x;
				const int k = id[t];
				float nx = 0.0f, ny = 0.0f;
				if (k >= 0)
				{
					auto Neighbour = [&](int xx, int yy, float& OutH) -> bool
						{
							if (xx < 0 || yy < 0 || xx >= static_cast<int>(S) || yy >= static_cast<int>(S)) return false;
							const size_t u = static_cast<size_t>(yy) * S + static_cast<size_t>(xx);
							if (id[u] != k) return false;
							OutH = height[u];
							return true;
						};
					auto Gradient = [&](int ax, int ay) -> float
						{
							float hm, hp;
							const bool bm = Neighbour(static_cast<int>(x) - ax, static_cast<int>(y) - ay, hm);
							const bool bp = Neighbour(static_cast<int>(x) + ax, static_cast<int>(y) + ay, hp);
							if (bm && bp) return 0.5f * (hp - hm);
							if (bp) return hp - height[t];
							if (bm) return height[t] - hm;
							return 0.0f;
						};
					const FClusterInstance& I = inst[static_cast<size_t>(k)];
					nx = -(Gradient(1, 0) * LeafNormalStrength + I.TiltX);
					ny = -(Gradient(0, 1) * LeafNormalStrength + I.TiltY);
				}
				const float len = sqrtf(nx * nx + ny * ny + 1.0f);
				Normal.Set(x, y, 0.5f + 0.5f * nx / len, 0.5f + 0.5f * ny / len, 0.5f + 0.5f / len, 1.0f);
				Diff.Set(x, y, rgb[t * 3], rgb[t * 3 + 1], rgb[t * 3 + 2], alpha[t]);
				ARM.Set(x, y, ao[t], rough[t], 0.0f, 1.0f);
			}
		}
	}

	// ---- 葉の房アトラス (1024², 2x2 セル。BaseColor はセルごとにカバレッジ保存ミップ) ----
	bool GenerateClusterTextures(const std::string& DiffPath, const std::string& NormalPath, const std::string& ARMPath)
	{
		const unsigned int size = ClusterAtlasSize;
		const unsigned int cell = ClusterCellSize;

		FImageRGBA diff(size, size);
		FImageRGBA normal(size, size);
		FImageRGBA arm(size, size);
		for (unsigned int c = 0; c < 4; ++c)
		{
			FImageRGBA cellDiff(cell, cell);
			FImageRGBA cellNormal(cell, cell);
			FImageRGBA cellARM(cell, cell);
			RasterizeClusterCell(c, cellDiff, cellNormal, cellARM);

			const unsigned int ox = (c & 1u) * cell;
			const unsigned int oy = (c >> 1) * cell;
			for (unsigned int y = 0; y < cell; ++y)
			{
				const size_t src = static_cast<size_t>(y) * cell * 4;
				const size_t dst = (static_cast<size_t>(oy + y) * size + ox) * 4;
				memcpy(&diff.Pixels[dst], &cellDiff.Pixels[src], static_cast<size_t>(cell) * 4);
				memcpy(&normal.Pixels[dst], &cellNormal.Pixels[src], static_cast<size_t>(cell) * 4);
				memcpy(&arm.Pixels[dst], &cellARM.Pixels[src], static_cast<size_t>(cell) * 4);
			}
		}
		FlattenNormalUnderAlpha(normal, diff);

		// アトラスは 2x2 のセルごとにカバレッジを保存する
		return WriteDDS(DiffPath, diff, FProceduralTreeTextures::LeafCoverageClip, 2, 2) && WriteDDS(NormalPath, normal) && WriteDDS(ARMPath, arm);
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
std::string FProceduralTreeTextures::GetLeafNormalPath() { return TexturePath("Leaf_nor_dx"); }
std::string FProceduralTreeTextures::GetLeafARMPath() { return TexturePath("Leaf_arm"); }
std::string FProceduralTreeTextures::GetClusterBaseColorPath() { return TexturePath("Cluster_diff"); }
std::string FProceduralTreeTextures::GetClusterNormalPath() { return TexturePath("Cluster_nor_dx"); }
std::string FProceduralTreeTextures::GetClusterARMPath() { return TexturePath("Cluster_arm"); }
std::string FProceduralTreeTextures::GetTwigBaseColorPath() { return TexturePath("Twig_diff"); }
std::string FProceduralTreeTextures::GetTwigNormalPath() { return TexturePath("Twig_nor_dx"); }
std::string FProceduralTreeTextures::GetTwigARMPath() { return TexturePath("Twig_arm"); }
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
	// 生成を実行して所要時間をログに出す (初回起動時のみ走る)
	auto Timed = [](const char* Name, auto&& Generate) -> bool
		{
			const auto t0 = std::chrono::steady_clock::now();
			const bool bOk = Generate();
			const float ms = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - t0).count();
			char buffer[160];
			sprintf_s(buffer, "[ProceduralTree] baked %s textures in %.1f ms%s\n", Name, ms, bOk ? "" : " (FAILED)");
			OutputDebugStringA(buffer);
			return bOk;
		};

	bool ok = true;

	if (!Exists(GetBarkBaseColorPath()) || !Exists(GetBarkNormalPath()) || !Exists(GetBarkARMPath()))
	{
		ok &= Timed("bark", [&] { return GenerateBarkTextures(GetBarkBaseColorPath(), GetBarkNormalPath(), GetBarkARMPath(), 1024); });
	}
	if (!Exists(GetLeafBaseColorPath()) || !Exists(GetLeafNormalPath()) || !Exists(GetLeafARMPath()))
	{
		ok &= Timed("leaf", [&] { return GenerateLeafTextures(GetLeafBaseColorPath(), GetLeafNormalPath(), GetLeafARMPath(), 512); });
	}
	if (!Exists(GetClusterBaseColorPath()) || !Exists(GetClusterNormalPath()) || !Exists(GetClusterARMPath()))
	{
		ok &= Timed("cluster", [&] { return GenerateClusterTextures(GetClusterBaseColorPath(), GetClusterNormalPath(), GetClusterARMPath()); });
	}
	if (!Exists(GetTwigBaseColorPath()) || !Exists(GetTwigNormalPath()) || !Exists(GetTwigARMPath()))
	{
		ok &= Timed("twig", [&] { return GenerateTwigTextures(GetTwigBaseColorPath(), GetTwigNormalPath(), GetTwigARMPath(), 256); });
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
