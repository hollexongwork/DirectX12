// =============================================================
//  TemporalAA.hlsl (UE Gen4 TemporalAA.usf の FTAAStandaloneCS 相当)
//  全順列 (TemporalAA_{Main,Upsampling}_{Low,Low_Downsample,Medium,High,MediumHigh}_CS /
//  TemporalAA_SuperSampling_CS) の本体。各ラッパは TAA_PASS_CONFIG / TAA_QUALITY /
//  TAA_DOWNSAMPLE を #define してからこのヘッダを include する。
//
//  コンピュートルートシグネチャ (C++ FDefaultTemporalUpscaler::Init, §3.7):
//    b0 TemporalAAParameters (C++ FTemporalAAParameters, 336 B)
//    t0 InputSceneColor (R)        t1 SceneLinearDepth (R, ビュー Z)   t2 SceneVelocity (R)
//    t3 HistoryBuffer (Hp)         t4 EyeAdaptationBuffer              t5 ResponsiveAAMask (R)
//    u0 OutComputeTex (H)          u1 OutComputeTexDownsampled         u2 DebugOutput (H)
//    s0 ポイントクランプ           s1 リニアクランプ
//
//  規約 (§0.2): 標準 Z (手前 = 小さい)、ScreenPos = NDC.xy (+y 上)、行ベクトル mul(v, M)。
//  UE (反転 Z) と比較が逆になる箇所は TemporalAACommon.hlsl の SelectClosestDepthCross に
//  集約し "// UE: >" / "// UE: max" を付けている。
// =============================================================

// ---- 近傍ボックスの種類 ----
#define HISTORY_CLAMPING_BOX_MIN_MAX         0
#define HISTORY_CLAMPING_BOX_VARIANCE        1
#define HISTORY_CLAMPING_BOX_SAMPLE_DISTANCE 2

// ---- 全順列共通 [M]: AA_CROSS 2 / AA_BICUBIC 1 / AA_MANUALLY_CLAMP_HISTORY_UV 1 / AA_YCOCG 1 /
//      AA_CLAMP 1 / AA_NAN 1 / AA_DYNAMIC 1 (後者 6 つは本体に常時組み込み) ----
#define AA_CROSS 2

// ---- パス構成 (ETAAPassConfig) ----
#if TAA_PASS_CONFIG == 0            // Main
  #define AA_UPSAMPLE 0
  #define AA_HISTORY_CLAMPING_BOX HISTORY_CLAMPING_BOX_MIN_MAX
  #define AA_ROUND 1
  #define AA_UPSAMPLE_ADAPTIVE_FILTERING 0
#elif TAA_PASS_CONFIG == 1          // MainUpsampling
  #define AA_UPSAMPLE 1
  #define AA_HISTORY_CLAMPING_BOX HISTORY_CLAMPING_BOX_SAMPLE_DISTANCE
  #define AA_ROUND 0
  #define AA_UPSAMPLE_ADAPTIVE_FILTERING 0
#else                               // MainSuperSampling
  #define AA_UPSAMPLE 1
  #define AA_HISTORY_CLAMPING_BOX HISTORY_CLAMPING_BOX_VARIANCE
  #define AA_ROUND 0
  #define AA_UPSAMPLE_ADAPTIVE_FILTERING 1
#endif

// ---- 品質 (ETAAQuality) ----
#if TAA_QUALITY == 0                // Low
  #define AA_FILTERED 0
  #define AA_SAMPLES 5
  #define AA_DYNAMIC_ANTIGHOST 0
#elif TAA_QUALITY == 1              // Medium
  #define AA_FILTERED 1
  #define AA_SAMPLES 5
  #define AA_DYNAMIC_ANTIGHOST 0
#elif TAA_QUALITY == 2              // High
  #define AA_FILTERED 1
  #define AA_SAMPLES 9
  #define AA_DYNAMIC_ANTIGHOST 1
#else                               // MediumHigh
  #define AA_FILTERED 1
  #define AA_SAMPLES 5
  #define AA_DYNAMIC_ANTIGHOST 1
#endif

// ---- CB Flags (C++ ETAAFlags と同値) ----
#define TAA_FLAG_UPSAMPLE_FILTERED     (1u << 0)
#define TAA_FLAG_RESPONSIVE_MASK_VALID (1u << 1)
#define TAA_FLAG_EYE_ADAPTATION_BUFFER (1u << 2)
#define TAA_FLAG_HISTORY_HAS_ALPHA     (1u << 3)
#define TAA_FLAG_FTW_MODE_SHIFT        4u
#define TAA_FLAG_DOWNSAMPLE_OUTPUT     (1u << 6)

#include "TemporalAACommon.hlsl"
#include "VelocityCommon.hlsl"

// =============================================================
//  定数バッファ / リソース (§6.5.2)
// =============================================================
cbuffer TemporalAAParameters : register(b0)       // C++ FTemporalAAParameters (§3.4, 336 B)
{
    float4   InputSceneColorSize;          //   0 (R.x, R.y, 1/R.x, 1/R.y)
    int4     InputMinMaxPixelCoord;        //  16 (0, 0, R.x-1, R.y-1)
    float4   OutputViewportSize;           //  32 (H.x, H.y, 1/H.x, 1/H.y)
    float4   HistoryBufferSize;            //  48 (Hp.x, Hp.y, 1/Hp.x, 1/Hp.y)
    float4   HistoryBufferUVMinMax;        //  64
    float4   ScreenPosToHistoryBufferUV;   //  80 (0.5, -0.5, 0.5, 0.5)
    float4x4 ClipToPrevClip;               //  96 NoAA x NoAA (行ベクトル: PrevClip = ThisClip * C2P)
    float2   TemporalJitterPixels;         // 160 レンダー px (+y 下)
    float2   ScreenPosAbsMax;              // 168
    float    ScreenPercentage;             // 176 R.x / H.x
    float    UpscaleFactor;                // 180 H.x / R.x
    float    CurrentFrameWeight;           // 184
    float    HistoryPreExposureCorrection; // 188
    uint     bCameraCut;                   // 192
    uint     Flags;                        // 196
    float    ManualExposure;               // 200
    uint     DebugMode;                    // 204 (ETemporalAADebugView 5..13, それ以外 0)
    float4   SampleWeights[3];             // 208 Main: 3x3 重み [i >> 2][i & 3]
    float4   PlusWeights[2];               // 256 Main: プラス 5 重み
    float4   OutputQuantizationError;      // 288 (w = 最大有限値)
    float4   DepthParams;                  // 304 (Q, -Q*n, 0.999*f, 0)
    uint     StateFrameIndexMod8;          // 320
    float    DebugScale;                   // 324
    float    SampleDistanceThreshold;      // 328
    uint     Pad0;                         // 332
};
Texture2D<float4>   InputSceneColor          : register(t0);   // R (PSR -> NPSR)
Texture2D<float2>   SceneLinearDepth         : register(t1);   // R: ビュー Z [m] (不透明のみ)
Texture2D<float2>   SceneVelocity            : register(t2);   // R16G16_UNORM エンコード (0 = 未書き込み)
Texture2D<float4>   HistoryBuffer            : register(t3);   // Hp (カット時はダミー。読まない)
Buffer<float>       EyeAdaptationBuffer      : register(t4);   // [0] = 前フレーム露出
Texture2D<float>    ResponsiveAAMask         : register(t5);   // R8_UNORM
RWTexture2D<float4> OutComputeTex            : register(u0);   // H (新しい履歴 = 後段の SceneColor)
RWTexture2D<float4> OutComputeTexDownsampled : register(u1);   // ceil(H/2)
RWTexture2D<float4> DebugOutput              : register(u2);   // H
SamplerState        PointClampSampler        : register(s0);
SamplerState        LinearClampSampler       : register(s1);

int2   ClampInputPixel(int2 p) { return clamp(p, InputMinMaxPixelCoord.xy, InputMinMaxPixelCoord.zw); }
float3 LoadInputYCoCg(int2 p)  { return RGBToYCoCg(SanitizeColor(InputSceneColor.Load(int3(ClampInputPixel(p), 0)).rgb, 65504.0f)); }  // [PORT] 入力もサニタイズ
float  LoadViewZ(int2 p)       { return SceneLinearDepth.Load(int3(ClampInputPixel(p), 0)).r; }
bool   IsDynamicAt(int2 p)     { return IsVelocityWritten(SceneVelocity.Load(int3(ClampInputPixel(p), 0))); }

// =============================================================
//  現フレームフィルタと近傍ボックス (§6.5.3)
// =============================================================

// 現フレームの再構成フィルタ。InvFilterScale < 1 でカーネルを広げる (適応フィルタのみ)
void FilterCurrentFrame(float3 C[9], float2 dKO, float E, float InvFilterScale, out float3 Filtered, out float FTW)
{
#if AA_FILTERED
  #if AA_UPSAMPLE
    // (早期 return にしない: out 引数が未初期化扱いになり Debug (/Od) の fxc が X4000 を出すため if / else)
    if ((Flags & TAA_FLAG_UPSAMPLE_FILTERED) == 0u)                       // r.TemporalAAUpsampleFiltered = 0
    {
        Filtered = C[4];
        FTW = ComputeSampleWeigth(-dKO * InvFilterScale, UpscaleFactor);
    }
    else
    {
  #endif
    float3 Acc = 0.0f;
    float WAcc = 0.0f, WS = 0.0f;
    [unroll] for (uint s = 0; s < AA_SAMPLES; ++s)
    {
  #if AA_SAMPLES == 9
        const uint i = s;
  #else
        const uint i = kPlusIndexes3x3[s];
  #endif
  #if AA_UPSAMPLE
        const float ws = ComputeSampleWeigth((float2(kOffsets3x3[i]) - dKO) * InvFilterScale, UpscaleFactor);   // dPP = o - dKO (入力 px)
  #elif AA_SAMPLES == 9
        const float ws = SampleWeights[i >> 2][i & 3];                         // CPU: exp(-2.29|o - J|^2 / FS^2) 正規化
  #else
        const float ws = PlusWeights[s >> 2][s & 3];
  #endif
        const float wh = HdrWeightY(C[i].x, E);
        Acc += C[i] * (ws * wh);
        WAcc += ws * wh;
        WS += ws;
    }
    Filtered = Acc * rcp(WAcc);
  #if AA_UPSAMPLE
    FTW = WS;                                                                  // [L] 非正規化空間重みの総和 (出力画素あたりの時間サンプル密度を一定に)
    }
  #else
    FTW = 1.0f;
  #endif
#else
    Filtered = C[4];
  #if AA_UPSAMPLE
    FTW = ComputeSampleWeigth(-dKO * InvFilterScale, UpscaleFactor);          // [L] 最近傍サンプルの重み
  #else
    FTW = 1.0f;
  #endif
#endif
}

void ComputeNeighborhoodBoundingbox(float3 C[9], float2 dKO, float3 Filtered, out float3 NeighborMin, out float3 NeighborMax)
{
#if AA_HISTORY_CLAMPING_BOX == HISTORY_CLAMPING_BOX_MIN_MAX
    float3 PlusMin = C[1], PlusMax = C[1];
    [unroll] for (uint p = 1; p < 5; ++p)
    {
        PlusMin = min(PlusMin, C[kPlusIndexes3x3[p]]);
        PlusMax = max(PlusMax, C[kPlusIndexes3x3[p]]);
    }
  #if AA_SAMPLES == 9
    const float3 SquareMin = min(PlusMin, min(min(C[0], C[2]), min(C[6], C[8])));
    const float3 SquareMax = max(PlusMax, max(max(C[0], C[2]), max(C[6], C[8])));
    #if AA_ROUND
    NeighborMin = 0.5f * (SquareMin + PlusMin);                                // 丸めた箱 (UE AA_ROUND)
    NeighborMax = 0.5f * (SquareMax + PlusMax);
    #else
    NeighborMin = SquareMin;
    NeighborMax = SquareMax;
    #endif
  #else
    NeighborMin = PlusMin;
    NeighborMax = PlusMax;
  #endif
#elif AA_HISTORY_CLAMPING_BOX == HISTORY_CLAMPING_BOX_VARIANCE
    float3 m1 = 0.0f, m2 = 0.0f;
    [unroll] for (uint s = 0; s < AA_SAMPLES; ++s)                             // SuperSampling は 9 サンプル
    {
        m1 += C[s];
        m2 += C[s] * C[s];
    }
    m1 *= (1.0f / AA_SAMPLES);
    m2 *= (1.0f / AA_SAMPLES);
    const float3 StdDev = sqrt(abs(m2 - m1 * m1));
    NeighborMin = m1 - 1.25f * StdDev;                                         // [H] 1.25 sigma
    NeighborMax = m1 + 1.25f * StdDev;
  #if !AA_UPSAMPLE_ADAPTIVE_FILTERING
    NeighborMin = min(NeighborMin, Filtered);
    NeighborMax = max(NeighborMax, Filtered);
  #endif
#else // SAMPLE_DISTANCE
    NeighborMin = C[4];                                                        // K は |dKO| <= 0.707 なので常に内側
    NeighborMax = C[4];
    const float Thr2 = SampleDistanceThreshold * SampleDistanceThreshold;      // lerp(1.51, 1.3, UF - 1) [H], 入力 px [M]
    [unroll] for (uint s = 0; s < AA_SAMPLES; ++s)
    {
  #if AA_SAMPLES == 9
        const uint i = s;
  #else
        const uint i = kPlusIndexes3x3[s];
  #endif
        const float2 dPP = float2(kOffsets3x3[i]) - dKO;
        if (dot(dPP, dPP) < Thr2)
        {
            NeighborMin = min(NeighborMin, C[i]);
            NeighborMax = max(NeighborMax, C[i]);
        }
    }
#endif
}

// =============================================================
//  本体 (§6.5.4)
// =============================================================
#if TAA_DOWNSAMPLE
groupshared float4 GSDownsample[TAA_TILE_SIZE * TAA_TILE_SIZE];
#endif

[numthreads(TAA_TILE_SIZE, TAA_TILE_SIZE, 1)]
void main(uint2 GroupThreadId : SV_GroupThreadID, uint2 DispatchThreadId : SV_DispatchThreadID, uint GroupIndex : SV_GroupIndex)
{
    const uint2 PixelPos = DispatchThreadId;                                   // OutputViewportRect.Min = 0 (exact-size)
    const bool  bInside  = all(PixelPos < (uint2)OutputViewportSize.xy);       // 早期 return 禁止 (DOWNSAMPLE の同期のため)

    // ---- 1. 出力画素中心 (未ジッタ) ----
    const float2 ViewportUV = (float2(PixelPos) + 0.5f) * OutputViewportSize.zw;
    const float2 ScreenPos  = float2(2.0f * ViewportUV.x - 1.0f, 1.0f - 2.0f * ViewportUV.y);

    // ---- 2. 最近接入力画素 K と dKO (入力 px) ----
#if AA_UPSAMPLE
    const float2 PPCo = ViewportUV * InputSceneColorSize.xy + TemporalJitterPixels;  // 出力画素中心が写るジッタ済み入力座標
    const float2 PPCk = floor(PPCo) + 0.5f;
    const float2 dKO  = PPCo - PPCk;                                           // [-0.5, 0.5)
    const int2   K    = ClampInputPixel(int2(floor(PPCo)));
#else
    const float2 dKO  = TemporalJitterPixels;                                  // Main: K = 出力画素。重みは CPU (o - J)
    const int2   K    = ClampInputPixel(int2(PixelPos));
#endif

    // ---- 3. 前フレーム露出 / Responsive ----
    float E = ((Flags & TAA_FLAG_EYE_ADAPTATION_BUFFER) != 0u) ? EyeAdaptationBuffer[0] : ManualExposure;
    E = (E > 0.0f && E < 1.0e30f) ? E : ManualExposure;
    const bool bResponsive = ((Flags & TAA_FLAG_RESPONSIVE_MASK_VALID) != 0u) && (ResponsiveAAMask.Load(int3(K, 0)) > 0.5f);

    // ---- 4. 最近傍深度 (X パターン ±AA_CROSS 入力 px, 標準 Z = min) ----
    const float Z0 = LoadViewZ(K);
    const float4 Zc = float4(LoadViewZ(K + int2(-AA_CROSS, -AA_CROSS)), LoadViewZ(K + int2(AA_CROSS, -AA_CROSS)),
                             LoadViewZ(K + int2(-AA_CROSS,  AA_CROSS)), LoadViewZ(K + int2(AA_CROSS,  AA_CROSS)));
    int2 VelocityOffset;
    float ClosestZ;
    SelectClosestDepthCross(Z0, Zc, AA_CROSS, VelocityOffset, ClosestZ);      // PosN.xy は動かさない (UE)
    const float DeviceZ = ViewZToDeviceZ(ClosestZ, DepthParams);               // 遠方 = d = Q (回転のみ再投影)

    // ---- 5. カメラ運動 (NoAA x NoAA) とオブジェクト運動 ----
    const float4 PrevClip = mul(float4(ScreenPos, DeviceZ, 1.0f), ClipToPrevClip);
    bool   bPrevBehind = PrevClip.w <= 1.0e-6f;                                // [PORT] 大移動 / 背面ガード
    float2 BackN = ScreenPos - PrevClip.xy / max(PrevClip.w, 1.0e-6f);
    const float2 EncodedVelocity = SceneVelocity.Load(int3(ClampInputPixel(K + VelocityOffset), 0));
    if (IsVelocityWritten(EncodedVelocity))
    {
        BackN = DecodeVelocityFromTexture(EncodedVelocity);
        bPrevBehind = false;
    }
    const float2 BackTemp = BackN * OutputViewportSize.xy;                     // 単位 = 出力 px の 2 倍 (UE)
    const float  Velocity = sqrt(dot(BackTemp, BackTemp));
    const float2 HistoryScreenPosition = ScreenPos - BackN;
    const bool   OffScreen = max(abs(HistoryScreenPosition.x), abs(HistoryScreenPosition.y)) >= 1.0f || bPrevBehind;

    // ---- 6. 近傍 3x3 (YCoCg) ----
    float3 C[9];
    [unroll] for (uint i = 0; i < 9; ++i)
    {
        C[i] = LoadInputYCoCg(K + kOffsets3x3[i]);                             // 未使用分は fxc が除去
    }

    // ---- 7. 現フレームフィルタ (適応フィルタでなければクランプ前) ----
    float3 Filtered;
    float FTW;
    FilterCurrentFrame(C, dKO, E, 1.0f, Filtered, FTW);

    // ---- 8. 近傍ボックス ----
    float3 NeighborMin, NeighborMax;
    ComputeNeighborhoodBoundingbox(C, dKO, Filtered, NeighborMin, NeighborMax);

    // ---- 9. 履歴 (Catmull-Rom 5 タップ, UV 手動クランプ。カット時は読まない) ----
    float3 HistoryY = Filtered;
    float HistoryAlpha = 0.0f;
    [branch]
    if (bCameraCut == 0u)
    {
        const float2 HistoryUV = HistoryScreenPosition * ScreenPosToHistoryBufferUV.xy + ScreenPosToHistoryBufferUV.zw;
        float2 TapUV[5];
        float TapW[5];
        CatmullRom5Taps(HistoryUV, HistoryBufferSize, TapUV, TapW);
        float4 Acc = 0.0f;
        float WSum = 0.0f;
        [unroll] for (uint t = 0; t < 5; ++t)
        {
            const float2 uv = clamp(TapUV[t], HistoryBufferUVMinMax.xy, HistoryBufferUVMinMax.zw);   // AA_MANUALLY_CLAMP_HISTORY_UV
            Acc += HistoryBuffer.SampleLevel(LinearClampSampler, uv, 0.0f) * TapW[t];
            WSum += TapW[t];
        }
        float4 History = Acc * rcp(WSum);                                     // 角除去で総和 != 1 のため正規化
        History.rgb = SanitizeColor(History.rgb * HistoryPreExposureCorrection, 65504.0f);   // [PORT] リンギング / NaN
        HistoryY = RGBToYCoCg(History.rgb);
        HistoryAlpha = ((Flags & TAA_FLAG_HISTORY_HAS_ALPHA) != 0u) ? History.a : 0.0f;     // [PORT] R11G11B10 は a = 1 を返す
    }

    // ---- 10. 履歴棄却 ----
    bool IgnoreHistory = OffScreen || (bCameraCut != 0u);
    const bool DynamicCenter = IsDynamicAt(K);
    bool AntiGhostReject = false;
#if AA_DYNAMIC_ANTIGHOST
    const bool Dynamic = IsDynamicAt(K + int2(0, -1)) || IsDynamicAt(K + int2(-1, 0)) || DynamicCenter
                      || IsDynamicAt(K + int2(1, 0))  || IsDynamicAt(K + int2(0, 1));
    AntiGhostReject = !Dynamic && HistoryAlpha > 0.0f;                         // 動的だった履歴が静的背景に残るのを消す
    IgnoreHistory = IgnoreHistory || AntiGhostReject;
#endif

    // ---- 11. クランプ (YCoCg AABB) ----
    const float3 HistoryPreClamp = HistoryY;
    HistoryY = clamp(HistoryY, NeighborMin, NeighborMax);
#if AA_UPSAMPLE_ADAPTIVE_FILTERING
    {   // [L] クランプで大きく動いた (棄却された) 画素ほどカーネルを最大 1 入力 px まで広げる
        const float Rejection = saturate(length(HistoryPreClamp - HistoryY) / max(length(NeighborMax - NeighborMin), 1.0e-4f));
        FilterCurrentFrame(C, dKO, E, lerp(1.0f, rcp(max(UpscaleFactor, 1.0f)), Rejection), Filtered, FTW);
    }
#endif

    // ---- 12. FilteredTemporalWeight の定義切替 (デバッグ: 0 総和 / 1 最近傍 / 2 = 1) ----
#if AA_UPSAMPLE
    const uint FTWMode = (Flags >> TAA_FLAG_FTW_MODE_SHIFT) & 3u;
    if (FTWMode == 1u) FTW = ComputeSampleWeigth(-dKO, UpscaleFactor);
    else if (FTWMode == 2u) FTW = 1.0f;
#endif

    // ---- 13. BlendFinal (= 現フレームの重み) ----
    const float LumaFiltered = Filtered.x, LumaHistory = HistoryY.x;          // YCoCg Y = Luma4
    float BlendFinal = FTW * CurrentFrameWeight;
    BlendFinal = lerp(BlendFinal, 0.2f, saturate(Velocity / 40.0f));
    const float BlendFinalPreFloor = BlendFinal;                               // デバッグ view 5 用 (下限適用前。収束した平坦画素は下限で 1 になるため)
    BlendFinal = max(BlendFinal, saturate(0.01f * LumaHistory * rcp(max(abs(LumaFiltered - LumaHistory), 1.0e-8f))));   // UE の停滞防止下限 (そのまま)
    if (bResponsive)   BlendFinal = 0.25f;
    if (IgnoreHistory) BlendFinal = 1.0f;

    // ---- 14. HDR 加重ブレンド + ガード + 確率的量子化 ----
    const float2 Wl = WeightedLerpFactors(HdrWeightY(LumaHistory, E), HdrWeightY(LumaFiltered, E), BlendFinal);
    float3 OutRGB = YCoCgToRGB(HistoryY * Wl.x + Filtered * Wl.y);
    OutRGB = SanitizeColor(OutRGB, OutputQuantizationError.w);                 // AA_NAN: NaN / 負 -> 0, 上限
    {
        const uint2 Rnd = Rand3DPCG16(int3(int2(PixelPos), (int)StateFrameIndexMod8)).xy;
        const float Eq  = Hammersley16(0u, 1u, Rnd).x;                         // [0, 1) (UE Gen4)
        OutRGB = min(QuantizeForFloatRenderTarget(OutRGB, Eq, OutputQuantizationError.xyz), OutputQuantizationError.w);
    }
    float OutAlpha = 0.0f;
#if AA_DYNAMIC_ANTIGHOST
    OutAlpha = DynamicCenter ? 1.0f : 0.0f;                                    // 次フレームのアンチゴースト用
#endif
    if (bInside)
    {
        OutComputeTex[PixelPos] = float4(OutRGB, OutAlpha);
    }

    // ---- 15. デバッグ出力 (DebugMode = ETemporalAADebugView 5..13) ----
    [branch]
    if (DebugMode != 0u && bInside)
    {
        const float Bg = 0.2f * saturate(0.25f * Filtered.x * E);            // 背景 (表示用輝度)
        float3 d = Bg.xxx;
        switch (DebugMode)
        {
        case 5u:  d = BlendFinalHeat(IgnoreHistory ? 1.0f : (bResponsive ? 0.25f : BlendFinalPreFloor)); break; // BlendFinal (下限前, §6.8)
        case 6u:  d = (OffScreen || AntiGhostReject || bCameraCut != 0u)
                      ? float3(OffScreen ? 1.0f : 0.0f, AntiGhostReject ? 1.0f : 0.0f, bCameraCut != 0u ? 1.0f : 0.0f) : Bg.xxx; break; // Rejection
        case 7u:  d = saturate(abs(HistoryPreClamp.x - HistoryY.x) * E * 0.25f * DebugScale).xxx; break;      // HistoryClamp
        case 8u:  d = saturate(abs(HistoryPreClamp.x - Filtered.x) * E * 0.25f * DebugScale).xxx; break;      // ReprojectionError
        case 9u:  d = saturate(FTW / 1.2f).xxx; break;                                                        // FilteredTemporalWeight
        case 10u: d = all(VelocityOffset == 0) ? float3(0, 0, 0)
                    : (VelocityOffset.x < 0 ? (VelocityOffset.y < 0 ? float3(1, 0, 0) : float3(0, 0, 1))
                                            : (VelocityOffset.y < 0 ? float3(0, 1, 0) : float3(1, 1, 0))); break; // ClosestDepthOffset
        case 11u: d = bResponsive ? float3(1, 1, 0) : Bg.xxx; break;                                          // ResponsiveMask
        case 12u: d = float3(saturate(HistoryAlpha), DynamicCenter ? 1.0f : 0.0f, AntiGhostReject ? 1.0f : 0.0f); break; // DynamicAntiGhost
        case 13u: d = float3(dKO.x + 0.5f, dKO.y + 0.5f, saturate(FTW / 1.2f)); break;                         // InputSampleAlignment
        default:  break;
        }
        DebugOutput[PixelPos] = float4(d, 1.0f);
    }

#if TAA_DOWNSAMPLE
    // ---- 16. ハーフ解像度 (2x2 ボックス, 有効画素のみで重み付け) ----
    GSDownsample[GroupIndex] = bInside ? float4(OutRGB, 1.0f) : float4(0.0f, 0.0f, 0.0f, 0.0f);
    GroupMemoryBarrierWithGroupSync();
    if (((GroupThreadId.x | GroupThreadId.y) & 1u) == 0u && (Flags & TAA_FLAG_DOWNSAMPLE_OUTPUT) != 0u)
    {
        const float4 s = GSDownsample[GroupIndex] + GSDownsample[GroupIndex + 1] + GSDownsample[GroupIndex + TAA_TILE_SIZE] + GSDownsample[GroupIndex + TAA_TILE_SIZE + 1];
        const uint2 HalfPos = PixelPos >> 1;
        const uint2 HalfExtent = ((uint2)OutputViewportSize.xy + 1u) >> 1;     // ceil(H/2) = 確保サイズ
        if (s.w > 0.0f && all(HalfPos < HalfExtent))
        {
            OutComputeTexDownsampled[HalfPos] = float4(s.rgb / s.w, 1.0f);
        }
    }
#endif
}
