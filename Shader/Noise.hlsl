#ifndef NOISE_HLSL
#define NOISE_HLSL

// =============================================================
//  Noise
//  Random.ush の GradientNoise3D_ALU / MaterialExpressionNoise
//  (Noise マテリアルノード) の本エンジン版。レジスタ非依存。
//
//  対応している Noise ノードの設定:
//    Function     : Gradient - Computational (Perlin 勾配ノイズ, ALU ハッシュ)
//    Levels       : オクターブ数 (1..8)
//    Level Scale  : オクターブ間の周波数比 (既定 2)
//    Turbulence   : |noise| を足す (尖った雲)
//    Output Min / Max
//    Filter Width : ピクセル (froxel) 幅より細かいオクターブを減衰
//  [PORT] Simplex / Fast Gradient (テクスチャ) / Voronoi / Tiling は無い。
//  Quality は 1 固定 (トリリニア補間、2 次補間の fade)。
// =============================================================

// -------------------------------------------------------------
//  整数ハッシュ (PCG 系)。float3 セル座標 -> [0,1) の 3 値
// -------------------------------------------------------------
uint3 NoiseHashUInt3(uint3 v)
{
    v = v * 1664525u + 1013904223u;
    v.x += v.y * v.z;
    v.y += v.z * v.x;
    v.z += v.x * v.y;
    v ^= v >> 16u;
    v.x += v.y * v.z;
    v.y += v.z * v.x;
    v.z += v.x * v.y;
    return v;
}

float3 NoiseHash3(int3 Cell)
{
    uint3 h = NoiseHashUInt3(asuint(Cell));
    return float3(h) * (1.0f / 4294967296.0f);
}

// セル格子点の勾配ベクトル (単位球面上, ハッシュから生成)
float3 NoiseGradient(int3 Cell)
{
    float3 h = NoiseHash3(Cell);
    float z = h.x * 2.0f - 1.0f;
    float r = sqrt(saturate(1.0f - z * z));
    float phi = h.y * 6.28318530718f;
    return float3(r * cos(phi), r * sin(phi), z);
}

// 5 次の fade (Perlin 改良版)
float3 NoiseFade(float3 t)
{
    return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
}

// -------------------------------------------------------------
//  GradientNoise3D_ALU : Perlin 勾配ノイズ。戻り値はおよそ [-1, 1]
// -------------------------------------------------------------
float GradientNoise3D_ALU(float3 Position)
{
    float3 pf = floor(Position);
    int3 pi = (int3) pf;
    float3 f = Position - pf;
    float3 w = NoiseFade(f);

    float n000 = dot(NoiseGradient(pi + int3(0, 0, 0)), f - float3(0, 0, 0));
    float n100 = dot(NoiseGradient(pi + int3(1, 0, 0)), f - float3(1, 0, 0));
    float n010 = dot(NoiseGradient(pi + int3(0, 1, 0)), f - float3(0, 1, 0));
    float n110 = dot(NoiseGradient(pi + int3(1, 1, 0)), f - float3(1, 1, 0));
    float n001 = dot(NoiseGradient(pi + int3(0, 0, 1)), f - float3(0, 0, 1));
    float n101 = dot(NoiseGradient(pi + int3(1, 0, 1)), f - float3(1, 0, 1));
    float n011 = dot(NoiseGradient(pi + int3(0, 1, 1)), f - float3(0, 1, 1));
    float n111 = dot(NoiseGradient(pi + int3(1, 1, 1)), f - float3(1, 1, 1));

    float nx00 = lerp(n000, n100, w.x);
    float nx10 = lerp(n010, n110, w.x);
    float nx01 = lerp(n001, n101, w.x);
    float nx11 = lerp(n011, n111, w.x);
    float nxy0 = lerp(nx00, nx10, w.y);
    float nxy1 = lerp(nx01, nx11, w.y);

    // 勾配ノイズの振幅は最大 ~0.87 なので [-1,1] へ正規化
    return lerp(nxy0, nxy1, w.z) * 1.15f;
}

// -------------------------------------------------------------
//  MaterialExpressionNoise : Noise ノード (Gradient - Computational)
//    Position    : 既に Scale を掛けた座標 (ノイズ空間)
//    Levels      : オクターブ数 (1..8)
//    LevelScale  : オクターブ間の周波数比 (> 1)
//    bTurbulence : |noise| を足す
//    OutputMin/Max : 出力レンジ
//    FilterWidth : ノイズ空間でのサンプル幅。波長が 2 x FilterWidth を
//                  下回るオクターブを減衰させる (froxel 幅よりも細かい
//                  模様をジッタ / テンポラル蓄積で散らさないため) [M]
// -------------------------------------------------------------
float MaterialExpressionNoise(float3 Position, uint Levels, float LevelScale, bool bTurbulence,
    float OutputMin, float OutputMax, float FilterWidth)
{
    Levels = clamp(Levels, 1u, 8u);
    LevelScale = max(LevelScale, 1.0001f);

    float result = 0.0f;
    float normalize = 0.0f;
    float amplitude = 1.0f;
    float frequency = 1.0f;

    [loop]
    for (uint level = 0; level < Levels; ++level)
    {
        // このオクターブの波長 1/frequency が 2 x FilterWidth を下回ると消す
        float filter = saturate(1.0f - 2.0f * FilterWidth * frequency);

        float value = GradientNoise3D_ALU(Position * frequency);
        value = bTurbulence ? abs(value) : value;

        result += value * amplitude * filter;
        normalize += amplitude;	// 減衰分は 0 寄り (平均値) へ畳む

        frequency *= LevelScale;
        amplitude /= LevelScale;
    }

    result /= max(normalize, 1e-4f);

    // 非 Turbulence は [-1,1] -> [0,1]、Turbulence は [0,1] のまま
    float unit = bTurbulence ? saturate(result) : saturate(result * 0.5f + 0.5f);
    return lerp(OutputMin, OutputMax, unit);
}

#endif
