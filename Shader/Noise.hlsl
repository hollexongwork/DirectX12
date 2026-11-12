#ifndef NOISE_HLSL
#define NOISE_HLSL

// =============================================================
//  Noise
//  FBM Perlin ノイズ (マテリアル関数 "Fbm Perlin noise" の本エンジン版)。
//  レジスタ非依存。
//
//  FBMNoise:
//    PcgHash          : 整数ハッシュ (PCG)
//    Perlin2D/3D/4D   : 改良 Perlin (5 次 fade) の 2 / 3 / 4 次元版。
//                       勾配は格子点ハッシュから選ぶ (2D: 8 方向,
//                       3D: 12 方向, 4D: (0,±1,±1,±1) の並べ替え 32 方向)。
//                       戻り値はおおよそ [-1, 1]
//    fbm(float2/3/4)  : オクターブ合成 (Octaves / Lacunarity / Gain)。
//                       オクターブごとに格子を少しずらして重なりを避け、
//                       4D では w (Evolution) にも Lacunarity を掛ける
//                       (細かい成分ほど速く変化)。戻り値は振幅で正規化
//                       した [-1, 1]
//    FbmPerlinNoise   : マテリアル関数の本体。ノイズ空間の Pos から
//                       2D / 3D / 4D を選んで 0..1 で返す
//    Worley3D         : セルラーノイズの F1 距離 (27 セル探索)
//    worleyFbm        : 反転 Worley (1 - F1) のオクターブ合成。丸い塊 (ビロー)。
//                       雲の密度ベイク (HeterogeneousVolumeCloudBake_CS) が使う
//
//  単位: Pos はノイズ空間 (格子 1 セル = 1.0)。呼び出し側がワールド座標に
//  Scale [1/m] を掛ける。Evolution は無次元の 4 軸目。
// =============================================================

#define FBM_NOISE_DIMENSION_2D 2
#define FBM_NOISE_DIMENSION_3D 3
#define FBM_NOISE_DIMENSION_4D 4

struct FBMNoise
{
    uint PcgHash(uint v)
    {
        uint state = v * 747796405u + 2891336453u;
        uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
        return (word >> 22u) ^ word;
    }

    float2 Fade(float2 t) { return t * t * t * (t * (t * 6.0 - 15.0) + 10.0); }
    float3 Fade(float3 t) { return t * t * t * (t * (t * 6.0 - 15.0) + 10.0); }
    float4 Fade(float4 t) { return t * t * t * (t * (t * 6.0 - 15.0) + 10.0); }

    // ---- 格子点ハッシュ ----
    uint Hash2(int2 c)
    {
        return PcgHash(uint(c.x) ^ PcgHash(uint(c.y)));
    }

    uint Hash3(int3 c)
    {
        return PcgHash(uint(c.x) ^ PcgHash(uint(c.y) ^ PcgHash(uint(c.z))));
    }

    uint Hash4(int4 c)
    {
        return PcgHash(uint(c.x) ^ PcgHash(uint(c.y) ^ PcgHash(uint(c.z) ^ PcgHash(uint(c.w)))));
    }

    // ---- 勾配 ----
    // 2D 勾配: (±1,±1) と (±1,0)/(0,±1) の 8 方向
    float Grad2(uint h, float2 f)
    {
        h &= 7u;
        if (h < 4u)
        {
            return ((h & 1u) ? -f.x : f.x) + ((h & 2u) ? -f.y : f.y);	// (±1, ±1)
        }
        float a = (h < 6u) ? f.x : f.y;	// (±1, 0) / (0, ±1)
        return (h & 1u) ? -a : a;
    }

    // 3D 勾配: (0,±1,±1) の並べ替え 12 方向 (Perlin 改良版)
    float Grad3(uint h, float3 f)
    {
        h &= 15u;
        float a = h < 8u ? f.x : f.y;
        float b = h < 4u ? f.y : ((h == 12u || h == 14u) ? f.x : f.z);
        return ((h & 1u) ? -a : a) + ((h & 2u) ? -b : b);
    }

    // 4D 勾配: (0,±1,±1,±1) の並べ替え 32 方向
    float Grad4(uint h, float4 f)
    {
        h &= 31u;
        float a = h < 24u ? f.x : f.y;
        float b = h < 16u ? f.y : f.z;
        float c = h < 8u ? f.z : f.w;
        return ((h & 1u) ? -a : a) + ((h & 2u) ? -b : b) + ((h & 4u) ? -c : c);
    }

    float Corner(int2 i, float2 f, int2 o) { return Grad2(Hash2(i + o), f - float2(o)); }
    float Corner(int3 i, float3 f, int3 o) { return Grad3(Hash3(i + o), f - float3(o)); }
    float Corner(int4 i, float4 f, int4 o) { return Grad4(Hash4(i + o), f - float4(o)); }

    // ---- Perlin ----
    float Perlin2D(float2 p)
    {
        float2 pi = floor(p);
        float2 f = p - pi;
        int2 i = int2(pi);
        float2 u = Fade(f);

        float x0 = lerp(Corner(i, f, int2(0, 0)), Corner(i, f, int2(1, 0)), u.x);
        float x1 = lerp(Corner(i, f, int2(0, 1)), Corner(i, f, int2(1, 1)), u.x);
        return lerp(x0, x1, u.y) * 1.41; // おおよそ [-1, 1] に正規化 (最大 ~0.71)
    }

    float Perlin3D(float3 p)
    {
        float3 pi = floor(p);
        float3 f = p - pi;
        int3 i = int3(pi);
        float3 u = Fade(f);

        float x00 = lerp(Corner(i, f, int3(0, 0, 0)), Corner(i, f, int3(1, 0, 0)), u.x);
        float x10 = lerp(Corner(i, f, int3(0, 1, 0)), Corner(i, f, int3(1, 1, 0)), u.x);
        float x01 = lerp(Corner(i, f, int3(0, 0, 1)), Corner(i, f, int3(1, 0, 1)), u.x);
        float x11 = lerp(Corner(i, f, int3(0, 1, 1)), Corner(i, f, int3(1, 1, 1)), u.x);
        return lerp(lerp(x00, x10, u.y), lerp(x01, x11, u.y), u.z) * 1.15; // おおよそ [-1, 1] に正規化 (最大 ~0.87)
    }

    float Perlin4D(float4 p)
    {
        float4 pi = floor(p);
        float4 f = p - pi;
        int4 i = int4(pi);
        float4 u = Fade(f);

        // w = 0 側の 3D キューブ
        float x000 = lerp(Corner(i, f, int4(0, 0, 0, 0)), Corner(i, f, int4(1, 0, 0, 0)), u.x);
        float x100 = lerp(Corner(i, f, int4(0, 1, 0, 0)), Corner(i, f, int4(1, 1, 0, 0)), u.x);
        float x010 = lerp(Corner(i, f, int4(0, 0, 1, 0)), Corner(i, f, int4(1, 0, 1, 0)), u.x);
        float x110 = lerp(Corner(i, f, int4(0, 1, 1, 0)), Corner(i, f, int4(1, 1, 1, 0)), u.x);
        float w0 = lerp(lerp(x000, x100, u.y), lerp(x010, x110, u.y), u.z);

        // w = 1 側の 3D キューブ
        float x001 = lerp(Corner(i, f, int4(0, 0, 0, 1)), Corner(i, f, int4(1, 0, 0, 1)), u.x);
        float x101 = lerp(Corner(i, f, int4(0, 1, 0, 1)), Corner(i, f, int4(1, 1, 0, 1)), u.x);
        float x011 = lerp(Corner(i, f, int4(0, 0, 1, 1)), Corner(i, f, int4(1, 0, 1, 1)), u.x);
        float x111 = lerp(Corner(i, f, int4(0, 1, 1, 1)), Corner(i, f, int4(1, 1, 1, 1)), u.x);
        float w1 = lerp(lerp(x001, x101, u.y), lerp(x011, x111, u.y), u.z);

        return lerp(w0, w1, u.w) * 0.87; // おおよそ [-1, 1] に正規化
    }

    // ---- FBM (オクターブ合成) ----
    float fbm(float2 p, int octaves, float lacunarity, float gain)
    {
        float sum = 0.0, amp = 0.5, norm = 0.0;
        [loop]
        for (int k = 0; k < octaves; k++)
        {
            // オクターブごとに少しずらして格子の重なりを避ける
            sum += amp * Perlin2D(p + float2(17.13, 31.71) * k);
            norm += amp;
            p *= lacunarity;
            amp *= gain;
        }
        return sum / max(norm, 1e-5);
    }

    float fbm(float3 p, int octaves, float lacunarity, float gain)
    {
        float sum = 0.0, amp = 0.5, norm = 0.0;
        [loop]
        for (int k = 0; k < octaves; k++)
        {
            sum += amp * Perlin3D(p + float3(17.13, 31.71, 47.37) * k);
            norm += amp;
            p *= lacunarity;
            amp *= gain;
        }
        return sum / max(norm, 1e-5);
    }

    float fbm(float4 p, int octaves, float lacunarity, float gain)
    {
        float sum = 0.0, amp = 0.5, norm = 0.0;
        [loop]
        for (int k = 0; k < octaves; k++)
        {
            sum += amp * Perlin4D(p + float4(17.13, 31.71, 47.37, 59.93) * k);
            norm += amp;
            p *= lacunarity; // w にも倍率を掛ける -> 細かい成分ほど速く変化
            amp *= gain;
        }
        return sum / max(norm, 1e-5);
    }

    // ---- Worley (セルラー) ----
    // セル内の特徴点 (0..1)^3
    float3 CellPoint3(int3 c)
    {
        uint h0 = Hash3(c);
        uint h1 = PcgHash(h0);
        uint h2 = PcgHash(h1);
        return float3(h0 & 0xFFFFu, h1 & 0xFFFFu, h2 & 0xFFFFu) * (1.0 / 65535.0);
    }

    // F1 距離 (最近傍の特徴点までの距離)。0 .. ~1 (セル 1 辺 = 1.0)
    float Worley3D(float3 p)
    {
        float3 pi = floor(p);
        float3 f = p - pi;
        int3 i = int3(pi);

        float d2 = 1.0e9;
        for (int z = -1; z <= 1; z++)
        {
            for (int y = -1; y <= 1; y++)
            {
                for (int x = -1; x <= 1; x++)
                {
                    int3 o = int3(x, y, z);
                    float3 r = float3(o) + CellPoint3(i + o) - f;
                    d2 = min(d2, dot(r, r));
                }
            }
        }
        return sqrt(d2);
    }

    // 反転 Worley (1 - F1) の FBM。特徴点で高く、セル境界で低い = 丸い塊 (ビロー)。0..1
    float worleyFbm(float3 p, int octaves, float lacunarity, float gain)
    {
        float sum = 0.0, amp = 0.5, norm = 0.0;
        [loop]
        for (int k = 0; k < octaves; k++)
        {
            sum += amp * saturate(1.0 - Worley3D(p + float3(17.13, 31.71, 47.37) * k));
            norm += amp;
            p *= lacunarity;
            amp *= gain;
        }
        return sum / max(norm, 1e-5);
    }
};

// -------------------------------------------------------------
//  FbmPerlinNoise : マテリアル関数 "Fbm Perlin noise" の本体
//    Pos       : ノイズ空間の座標 (呼び出し側でスケール / オフセット済み。
//                格子 1 セル = 1.0)
//    Dimension : 2 = XZ 平面 (水平) の 2D, 3 = 3D, 4 = 3D + Evolution (4 軸目)
//    Octaves   : 1..8
//  戻り値は 0..1
// -------------------------------------------------------------
float FbmPerlinNoise(float3 Pos, float Evolution,
    int Octaves, float Lacunarity, float Gain, int Dimension)
{
    FBMNoise N;

    int oct = clamp(Octaves, 1, 8);

    float n;
    [branch]
    if (Dimension == FBM_NOISE_DIMENSION_2D)
    {
        n = N.fbm(Pos.xz, oct, Lacunarity, Gain);
    }
    else if (Dimension == FBM_NOISE_DIMENSION_3D)
    {
        n = N.fbm(Pos, oct, Lacunarity, Gain);
    }
    else
    {
        n = N.fbm(float4(Pos, Evolution), oct, Lacunarity, Gain);
    }

    return saturate(n * 0.5 + 0.5);
}

#endif
