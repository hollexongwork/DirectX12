#include "Common.hlsl"
#include "VelocityCommon.hlsl"
#include "TemporalAACommon.hlsl"   // ViewZToDeviceZ

// =============================================================
//  VisualizeTemporalAAPS (VisualizeMotionVectors / VisualizeTemporalUpscaler / AA_DEBUG 相当)
//  Temporal AA のデバッグ表示 (ETemporalAADebugView)。DeferredVS のフルスクリーンクアッドで
//  バックバッファ (出力解像度 O, ビューポート O) へ上書きする。
//  FSceneRenderer::AddVisualizeTemporalAAPass がバインドする:
//    b0  : カメラ (ClipToPrevClip / NearFar / ViewSizeAndInvSize = レンダー解像度 R)
//    b4  : VisualizeMode (= ETemporalAADebugView) / VisualizeScale / SceneTexelSize = 1/O
//    t0  : レンダー解像度の SceneColor (HDR)
//    t4  : LinearDepth (R = ビュー Z, 不透明のみ)
//    t11 : AutoExposure 結果 (PP_FLAG_AUTO_EXPOSURE の時だけ読む)
//    t35 : Velocity (今フレーム描いていなければ 1x1 ダミー = RG 0 = 未書き込み)
//    t36 : TAA DebugOutput (モード 5..13, 出力解像度 H) / 後段の入力 = TAA 出力 (モード 4) / それ以外はダミー
//
//  実装済みのモード:
//    1 MotionVectors    : 動きの向き = 色相 / 大きさ = 明度 (オブジェクト速度は彩度 1、カメラモーションは 0.5)
//                         + 0.15 x シーン輝度。24 px セルごとにセル中心から「前フレームの位置」への白線
//    2 VelocityMask     : ベロシティを書いた画素 = 緑、それ以外 = 0.3 x シーン輝度
//    3 InputOutputSplit : 左半分 = TAA 入力 (C++ が 2 回目のトーンマップをシザーで描く)、
//                         この PS は中央の 2 px の赤い分割線だけを描く
//    4 TemporalUpscalerIO (VisualizeTemporalUpscaler): 2x2 グリッド。各象限に画面全体を縮小表示
//                         左上 = 入力 (レンダー解像度 R のジッタ込み SceneColor, 象限 UV でバイリニア)
//                         右上 = 深度 (LinearDepth.G = 表示用の正規化深度)
//                         左下 = モーション (モード 1 の色, 矢印無し)
//                         右下 = 出力 (t36 = TAA 出力, 象限 UV でバイリニア)
//                         ラベル (解像度 / パス / 品質) は ImGui の AA ウィンドウが前面描画リストへ書く
//    5..13 (TAA CS の DebugOutput, AA_DEBUG 相当): t36 をそのまま表示 (値は CS 側で表示用に
//                         整形済み。BlendFinal / Rejection / HistoryClamp / ReprojectionError /
//                         FilteredTemporalWeight / ClosestDepthOffset / ResponsiveMask /
//                         DynamicAntiGhost / InputSampleAlignment)
//  未実装のモードは discard (通常画像のまま)。
// =============================================================

#define VISUALIZE_MOTION_VECTORS   1
#define VISUALIZE_VELOCITY_MASK    2
#define VISUALIZE_INPUT_OUTPUT     3
#define VISUALIZE_UPSCALER_IO      4
#define VISUALIZE_CS_DEBUG_FIRST   5    // 5..13: TAA CS の DebugOutput
#define VISUALIZE_CS_DEBUG_LAST    13

static const float kMotionVectorCellSize = 24.0f;   // 矢印 (白線) を描くセルの大きさ [出力 px]

// ---- HDR 値の表示 (§6.8): LinearToSRGB(saturate(c Ex / (1 + c Ex))) ----
float GetDisplayExposure()
{
    return (PostProcess.Flags & PP_FLAG_AUTO_EXPOSURE) ? AutoExposureBuffer[0] : PostProcess.Exposure;
}

float3 DisplayHDR(float3 c)
{
    const float3 e = max(c, 0.0f) * GetDisplayExposure();
    return LinearToSRGB(saturate(e / (1.0f + e)));
}

float3 HSVToRGB(float3 HSV)
{
    const float3 k = saturate(abs(frac(HSV.x + float3(1.0f, 2.0f / 3.0f, 1.0f / 3.0f)) * 6.0f - 3.0f) - 1.0f);
    return HSV.z * lerp(float3(1.0f, 1.0f, 1.0f), k, HSV.y);
}

// ビューポート UV -> レンダー画素 k = floor(uv R) (範囲内へクランプ)
int2 UVToRenderPixel(float2 uv)
{
    const int2 k = int2(floor(uv * ViewSizeAndInvSize.xy));
    return clamp(k, int2(0, 0), int2(ViewSizeAndInvSize.xy) - 1);
}

// 出力 px 座標 (SV_Position.xy と同じく画素中心 = p + 0.5) -> レンダー画素
int2 OutputToRenderPixel(float2 OutputPos, float2 InvOutputSize)
{
    return UVToRenderPixel(OutputPos * InvOutputSize);
}

// ---- レンダー画素 k の動き (TAA 手順 5 のディレーション無し版, §6.8) ----
//  V = s_cur - s_prev [ScreenPos]。ベロシティを書いた画素はそれを、書いていない画素は
//  深度 (LinearDepth -> デバイス Z, 遠方は無限遠 d = Q) と ClipToPrevClip (NoAA x NoAA) から求める
float2 ComputeMotionAtRenderPixel(int2 k, out bool bWritten)
{
    const float n = NearFar.x, f = NearFar.y;
    const float Q = f / (f - n);
    const float4 DepthParams = float4(Q, -Q * n, 0.999f * f, 0.0f);

    const float ViewZ = TextureLinearDepth.Load(int3(k, 0)).r;
    const float DeviceZ = ViewZToDeviceZ(ViewZ, DepthParams);
    const float2 uv = (float2(k) + 0.5f) * ViewSizeAndInvSize.zw;
    const float2 ScreenPos = float2(2.0f * uv.x - 1.0f, 1.0f - 2.0f * uv.y);
    const float4 PrevClip = mul(float4(ScreenPos, DeviceZ, 1.0f), ClipToPrevClip);
    float2 V = ScreenPos - PrevClip.xy / max(PrevClip.w, 1.0e-6f);   // w <= 0 ガード (TAA と同じ)

    const float2 Encoded = SceneVelocityTexture.Load(int3(k, 0));    // 1x1 ダミーの範囲外 Load は 0 (= 未書き込み)
    bWritten = IsVelocityWritten(Encoded);
    if (bWritten)
    {
        V = DecodeVelocityFromTexture(Encoded);
    }
    return V;
}

// ScreenPos の変位 -> 出力 px の変位 (+y 下): (V.x O.x / 2, -V.y O.y / 2)
float2 MotionToOutputPixels(float2 V, float2 OutputSize)
{
    return float2(V.x * OutputSize.x * 0.5f, -V.y * OutputSize.y * 0.5f);
}

// モード 1 の背景色 (矢印を除く): 色相 = 動きの向き, 彩度 = オブジェクト速度 1 / カメラモーション 0.5,
// 明度 = |Δ| x VisualizeScale / 16 [出力 px]。+ 0.15 x シーン輝度
float3 MotionVectorColor(int2 k, float2 OutputSize, float3 SceneGray)
{
    bool bWritten;
    const float2 Delta = MotionToOutputPixels(ComputeMotionAtRenderPixel(k, bWritten), OutputSize);
    const float Hue = frac(atan2(-Delta.y, Delta.x) * (1.0f / TWO_PI) + 1.0f);
    const float Val = saturate(length(Delta) * PostProcess.VisualizeScale / 16.0f);
    return HSVToRGB(float3(Hue, bWritten ? 1.0f : 0.5f, Val)) + 0.15f * SceneGray;
}

float DistanceToSegment(float2 P, float2 A, float2 B)
{
    const float2 AB = B - A, AP = P - A;
    const float t = saturate(dot(AP, AB) / max(dot(AB, AB), 1.0e-8f));
    return length(AP - AB * t);
}

float4 main(PS_INPUT input) : SV_TARGET0
{
    const uint Mode = PostProcess.VisualizeMode;
    const float2 InvOutputSize = float2(PostProcess.SceneTexelSizeX, PostProcess.SceneTexelSizeY);   // 1/O
    const float2 OutputSize = round(1.0f / InvOutputSize);
    const float2 SVPos = input.Position.xy;                                  // 出力画素中心 (p + 0.5)

    [branch]
    if (Mode == VISUALIZE_INPUT_OUTPUT)
    {
        // 左 = 入力 (ジッタ込み SceneColor) / 右 = TAA 出力。中央に 2 px の赤い分割線のみ
        if (abs(SVPos.x - 0.5f * OutputSize.x) < 1.0f)
        {
            return float4(1.0f, 0.0f, 0.0f, 1.0f);
        }
        discard;
        return float4(0.0f, 0.0f, 0.0f, 0.0f);
    }
    else if (Mode >= VISUALIZE_CS_DEBUG_FIRST && Mode <= VISUALIZE_CS_DEBUG_LAST)
    {
        // CS の DebugOutput (履歴解像度 H) を出力 UV で最近傍参照してそのまま表示
        uint DebugWidth, DebugHeight;
        TemporalAADebugTexture.GetDimensions(DebugWidth, DebugHeight);
        const float2 DebugExtent = float2(DebugWidth, DebugHeight);
        const int2 p = clamp(int2(SVPos * InvOutputSize * DebugExtent), int2(0, 0), int2(DebugExtent) - 1);
        return float4(saturate(TemporalAADebugTexture.Load(int3(p, 0)).rgb), 1.0f);
    }
    else if (Mode == VISUALIZE_UPSCALER_IO)
    {
        // 2x2 グリッド。象限 (qx, qy) の中で画面全体を UV [0, 1] として表示する
        const float2 HalfSize = 0.5f * OutputSize;
        const float2 Quadrant = step(HalfSize, SVPos);                           // 0 = 左 / 上, 1 = 右 / 下
        const float2 QuadUV = saturate((SVPos - Quadrant * HalfSize) / HalfSize);
        float3 Color;
        [branch]
        if (Quadrant.y < 0.5f)
        {
            if (Quadrant.x < 0.5f)
            {
                // 左上: TAA 入力 (レンダー解像度 R, ジッタ込み HDR)
                Color = DisplayHDR(TextureBaseColor.SampleLevel(Sampler2, QuadUV, 0.0f).rgb);
            }
            else
            {
                // 右上: 深度 (LinearDepth.G = 表示用に正規化済み)
                Color = TextureLinearDepth.Load(int3(UVToRenderPixel(QuadUV), 0)).g.xxx;
            }
        }
        else
        {
            if (Quadrant.x < 0.5f)
            {
                // 左下: モーション (モード 1 の色、矢印無し)
                const int2 k = UVToRenderPixel(QuadUV);
                const float3 Gray = DisplayHDR(Luminance(TextureBaseColor.Load(int3(k, 0)).rgb).xxx);
                Color = MotionVectorColor(k, OutputSize, Gray);
            }
            else
            {
                // 右下: TAA 出力 (後段の入力。Main / MainUpsampling = H, MainSuperSampling = MN 後の S)
                Color = DisplayHDR(TemporalAADebugTexture.SampleLevel(Sampler2, QuadUV, 0.0f).rgb);
            }
        }
        return float4(saturate(Color), 1.0f);
    }

    const int2 k = OutputToRenderPixel(SVPos, InvOutputSize);
    const float SceneLuma = Luminance(TextureBaseColor.Load(int3(k, 0)).rgb);
    const float3 SceneGray = DisplayHDR(SceneLuma.xxx);

    [branch]
    if (Mode == VISUALIZE_MOTION_VECTORS)
    {
        float3 Color = MotionVectorColor(k, OutputSize, SceneGray);

        // 24 px セルの中心から「前フレームの位置」(中心 - Δ(中心)) への白線 (幅 < 1 px)。
        // 始点はセル中央の画素中心 (+0.5) に置く (画素境界上だと水平 / 垂直の線が消えるため)。
        // 0.5 px 未満の動きは描かない (静止カメラでは矢印無し)
        const float2 Center = floor(SVPos / kMotionVectorCellSize) * kMotionVectorCellSize + (0.5f * kMotionVectorCellSize + 0.5f);
        bool bCenterWritten;
        const float2 CenterDelta = MotionToOutputPixels(
            ComputeMotionAtRenderPixel(OutputToRenderPixel(Center, InvOutputSize), bCenterWritten), OutputSize);
        if (dot(CenterDelta, CenterDelta) > 0.25f && DistanceToSegment(SVPos, Center, Center - CenterDelta) < 0.5f)
        {
            Color = float3(1.0f, 1.0f, 1.0f);
        }
        return float4(saturate(Color), 1.0f);
    }
    else if (Mode == VISUALIZE_VELOCITY_MASK)
    {
        const bool bWritten = IsVelocityWritten(SceneVelocityTexture.Load(int3(k, 0)));
        return bWritten ? float4(0.0f, 1.0f, 0.0f, 1.0f) : float4(saturate(0.3f * SceneGray), 1.0f);
    }

    discard;
    return float4(0.0f, 0.0f, 0.0f, 0.0f);
}
