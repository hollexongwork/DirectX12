#ifndef TEMPORAL_AA_COMMON_HLSL
#define TEMPORAL_AA_COMMON_HLSL

// =============================================================
//  TemporalAACommon.hlsl
//  Temporal AA / TAAU (TemporalAA.hlsl) と一次空間アップスケール
//  (PostProcessUpscale.hlsl)、Mitchell-Netravali / GPU 自己テストの
//  コンピュートシェーダが共有する純粋関数のみのヘッダ。
//  レジスタ (cbuffer / テクスチャ / サンプラ) は一切宣言しない
//  (各シェーダが自前のルートシグネチャに合わせて宣言する)。
//  C++ 側の CPU 鏡像 (自己テスト) と同じ式を保つこと。
// =============================================================

// TAA / Mitchell-Netravali のスレッドグループの一辺 (== C++ kTAATileSize)
#define TAA_TILE_SIZE 8

static const int2 kOffsets3x3[9] = { int2(-1,-1), int2(0,-1), int2(1,-1), int2(-1,0), int2(0,0), int2(1,0), int2(-1,1), int2(0,1), int2(1,1) };
static const uint kPlusIndexes3x3[5] = { 1, 3, 4, 5, 7 };

// ---- YCoCg (非正規化: Y = R + 2G + B = Luma4) ----
float3 RGBToYCoCg(float3 c) { return float3(dot(c, float3(1, 2, 1)), dot(c, float3(2, 0, -2)), dot(c, float3(-1, 2, -1))); }
float3 YCoCgToRGB(float3 c) { const float Y = c.x * 0.25f, Co = c.y * 0.25f, Cg = c.z * 0.25f; return float3(Y + Co - Cg, Y + Cg, Y - Co - Cg); }

// ---- HDR 重み (Karis 1/(1+L)。Y = 4L なので +4 は HdrWeight4 と同じ膝; 定数倍は相殺) ----
float  HdrWeightY(float Y, float Exposure) { return rcp(Y * Exposure + 4.0f); }
float2 WeightedLerpFactors(float WeightA, float WeightB, float Blend)
{
    const float A = (1.0f - Blend) * WeightA, B = Blend * WeightB;
    const float R = rcp(A + B);                   // 両重みとも正 (HdrWeightY > 0) なので 0 除算しない
    return float2(A * R, B * R);
}

// ---- TAAU 空間重み (Blackman-Harris 近似, 半径 1 出力 px, 下限 0.005) ----
float ComputeSampleWeigth(float2 PixelDelta /*入力 px*/, float UpscaleFactor)
{
    const float x2 = saturate(UpscaleFactor * UpscaleFactor * dot(PixelDelta, PixelDelta));
    return (0.905f * x2 - 1.9f) * x2 + 1.0f;
}

// ---- NaN / 負 / half 上限ガード ----
float3 SanitizeColor(float3 c, float MaxValue) { return min(-min(-c, 0.0f), MaxValue); }

// ---- 標準 Z: ビュー Z -> デバイス Z (LinearDepthPS の厳密逆)。遠方 (>= DepthParams.z) は無限遠 d = Q ----
//      DepthParams = (Q, -Q*n, 0.999*f, 0)
float ViewZToDeviceZ(float ViewZ, float4 DepthParams)
{
    return (ViewZ >= DepthParams.z) ? DepthParams.x : (DepthParams.x + DepthParams.y / ViewZ);
}

// ---- 最近傍深度 (X パターン ±Cross)。標準 Z なので「手前 = 小さい」: 反転 Z の max/> を min/< へ反転 ----
//      Z = (x:(-C,-C), y:(+C,-C), z:(-C,+C), w:(+C,+C)) のビュー Z。Offset = (0,0) なら中心が最近傍。
void SelectClosestDepthCross(float Z0, float4 Z, int Cross, out int2 Offset, out float ClosestZ)
{
    int2 DepthOffset = int2(Cross, Cross); int DepthOffsetXx = Cross;
    if (Z.x < Z.y) DepthOffsetXx = -Cross;                      // 反転 Z では >
    if (Z.z < Z.w) DepthOffset.x = -Cross;                      // 反転 Z では >
    const float ZXY = min(Z.x, Z.y), ZZW = min(Z.z, Z.w);       // 反転 Z では max
    if (ZXY < ZZW) { DepthOffset.y = -Cross; DepthOffset.x = DepthOffsetXx; }
    const float ZXYZW = min(ZXY, ZZW);
    Offset = int2(0, 0); ClosestZ = Z0;
    if (ZXYZW < Z0) { Offset = DepthOffset; ClosestZ = ZXYZW; }
}

// ---- 5 タップ Catmull-Rom (角除去)。UV と重みのみ返す (サンプリングは呼び出し側) ----
//      BufferSize = (W, H, 1/W, 1/H)。バイリニア 1 タップで中央 2 タップ (w1 + w2) を合成する
void CatmullRom5Taps(float2 UV, float4 BufferSize, out float2 TapUV[5], out float TapW[5])
{
    const float2 UVp = UV * BufferSize.xy;
    const float2 tc  = floor(UVp - 0.5f) + 0.5f;
    const float2 f = UVp - tc, f2 = f * f, f3 = f2 * f;
    const float2 w0 = f2 - 0.5f * (f3 + f);
    const float2 w1 = 1.5f * f3 - 2.5f * f2 + 1.0f;
    const float2 w3 = 0.5f * (f3 - f2);
    const float2 w2 = 1.0f - w0 - w1 - w3;
    const float2 W0 = w0, W1 = w1 + w2, W2 = w3;
    const float2 S0 = (tc - 1.0f) * BufferSize.zw;
    const float2 S1 = (tc + w2 / W1) * BufferSize.zw;
    const float2 S2 = (tc + 2.0f) * BufferSize.zw;
    TapUV[0] = float2(S1.x, S0.y); TapW[0] = W1.x * W0.y;
    TapUV[1] = float2(S0.x, S1.y); TapW[1] = W0.x * W1.y;
    TapUV[2] = float2(S1.x, S1.y); TapW[2] = W1.x * W1.y;
    TapUV[3] = float2(S2.x, S1.y); TapW[3] = W2.x * W1.y;
    TapUV[4] = float2(S1.x, S2.y); TapW[4] = W1.x * W2.y;
}

// ---- Mitchell-Netravali (B = C = 1/3) [L] ----
//      (出口を 1 つにしている: 途中 return は Debug (/Od) の fxc で X4000 の誤検出になる)
float MitchellNetravali(float x)
{
    x = abs(x);
    const float Inner = (7.0f * x * x * x - 12.0f * x * x + 16.0f / 3.0f) / 6.0f;                    // |x| < 1
    const float Outer = (-7.0f / 3.0f * x * x * x + 12.0f * x * x - 20.0f * x + 32.0f / 3.0f) / 6.0f;  // 1 <= |x| < 2
    return (x < 1.0f) ? Inner : ((x < 2.0f) ? Outer : 0.0f);
}

// ---- 乱数 ----
uint3 Rand3DPCG16(int3 p)
{
    uint3 v = uint3(p);
    v = v * 1664525u + 1013904223u;
    v.x += v.y * v.z; v.y += v.z * v.x; v.z += v.x * v.y;
    v.x += v.y * v.z; v.y += v.z * v.x; v.z += v.x * v.y;
    return v >> 16u;
}
float2 Hammersley16(uint Index, uint NumSamples, uint2 Random)
{
    const float E1 = frac((float)Index / (float)NumSamples + float(Random.x) * (1.0f / 65536.0f));
    const float E2 = float((reversebits(Index) >> 16) ^ Random.y) * (1.0f / 65536.0f);
    return float2(E1, E2);
}

// ---- 確率的量子化 (QuantizeForFloatRenderTarget; E in [0,1), 誤差 = 出力フォーマットの 1 ULP を 2 の冪へ切り下げ) ----
float3 QuantizeForFloatRenderTarget(float3 Color, float E, float3 QuantizationError)
{
    float3 Error = Color * QuantizationError;
    Error = asfloat(asuint(Error) & ~0x007FFFFFu);
    return Color + Error * E;
}

// ---- InterleavedGradientNoise (DeferredPS の LumenScreenGather のフレーム項と自己テスト Out[36] が使う) ----
float InterleavedGradientNoise(float2 uv, float FrameId)
{
    uv += FrameId * (float2(47.0f, 17.0f) * 0.695f);
    return frac(52.9829189f * frac(dot(uv, float2(0.06711056f, 0.00583715f))));
}

// ---- デバッグ用ヒートマップ (0->黒, 0.04->暗青, 0.2->緑, 0.25->黄, 1->赤) ----
//      (区間の判定は小さい側を後に上書き = 途中 return と同じ結果。出口 1 つで X4000 を避ける)
float3 BlendFinalHeat(float b)
{
    float3 c = lerp(float3(1, 1, 0), float3(1, 0, 0), saturate((b - 0.25f) / 0.75f));
    if (b <= 0.25f) c = lerp(float3(0, 1, 0), float3(1, 1, 0), (b - 0.2f) / 0.05f);
    if (b <= 0.2f)  c = lerp(float3(0, 0, 0.5f), float3(0, 1, 0), (b - 0.04f) / 0.16f);
    if (b <= 0.04f) c = lerp(float3(0, 0, 0), float3(0, 0, 0.5f), b / 0.04f);
    return c;
}
#endif
