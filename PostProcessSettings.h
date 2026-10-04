#pragma once
#include <cstddef>
#include <DirectXMath.h>
using namespace DirectX;

// ============================================================
//  FPostProcessSettings 相当の純データヘッダ。
//  GPU 側 POSTPROCESS と 1:1 ミラーなのでレイアウト変更は両側同時に行うこと。
// ============================================================

// Flags - MUST match PP_FLAG_* in ConstantBuffers.hlsl.
enum PP_FLAG : unsigned int
{
    PP_FLAG_BLOOM = 1u << 0,
    PP_FLAG_VIGNETTE = 1u << 1,
    PP_FLAG_CHROMATIC = 1u << 2,
    PP_FLAG_GRAIN = 1u << 3,
    PP_FLAG_COLOR_GRADING = 1u << 4,
    PP_FLAG_WHITE_BALANCE = 1u << 5,
    PP_FLAG_ARTIST_LUT = 1u << 6,
    PP_FLAG_AUTO_EXPOSURE = 1u << 7,
    PP_FLAG_DOF = 1u << 8,
};

enum class TONEMAPPER : unsigned int
{
    ACES_Narkowicz = 0,
    ACES_Hill = 1,
    None = 2,
};

// 16-byte-aligned, 1:1 with HLSL POSTPROCESS (size: see static_assert below).
struct PP_SETTINGS
{
    // --- group 0 : Exposure / Tonemapper ---
    float        Exposure = 1.0f;   // 2^EV linear multiplier
    unsigned int TonemapperMode = (unsigned int)TONEMAPPER::ACES_Hill;
    float        BloomIntensity = 0.6f;
    float        BloomThreshold = 1.0f;

    // --- group 1 : White Balance ---
    float WhiteTemp = 6500.0f;
    float WhiteTint = 0.0f;
    float ChromaticAberration = 0.3f;
    float VignetteIntensity = 0.4f;

    // --- group 2 : Color Grading (global) ---
    XMFLOAT4 ColorSaturation = { 1, 1, 1, 1 };
    XMFLOAT4 ColorContrast = { 1, 1, 1, 1 };
    XMFLOAT4 ColorGamma = { 1, 1, 1, 1 };
    XMFLOAT4 ColorGain = { 1, 1, 1, 1 };
    XMFLOAT4 ColorOffset = { 0, 0, 0, 1 };

    // --- group 3 : misc ---
    float        FilmGrainIntensity = 0.05f;
    float        FilmGrainTime = 0.0f;
    float        SceneTexelSizeX = 1.0f / 1920.0f;
    float        SceneTexelSizeY = 1.0f / 1080.0f;

    // --- group 4 : Depth of Field (Gaussian) ---
    float FocalDistance = 30.0f;   // ピントの合うビュー空間距離 (m)
    float FocalRegion = 8.0f;      // 完全にシャープな帯の幅 (m)。焦点面を中心に前後へ広がる
    float NearTransitionRange = 20.0f;// NearTransitionRange / FarTransitionRange : シャープ帯
    float FarTransitionRange = 60.0f; //の縁から最大ボケに到達するまでの距離 (m)。Near/Far Transition Range 相当

    float MaxBlurSize = 16.0f;    // CoC=1 のときのブラー半径 (ハーフ解像度テクセル単位)
    float NearBlurScale = 1.0f;    // 手前ボケの強さ倍率 
    float FarBlurScale = 1.0f;    // 奥ボケの強さ倍率
    float DofPad = 0.0f;

    // --- Flag ---
    unsigned int Flags = PP_FLAG_BLOOM | PP_FLAG_AUTO_EXPOSURE |
        PP_FLAG_COLOR_GRADING | PP_FLAG_WHITE_BALANCE;

    // --- レンダラ専有 (旧パディング。永続化しない: Write/ReadPostProcess は触れない) ---
    // FSceneRenderer がパスの直前に書く (ボリューム側の値は使われない)
    float        UpscaleUnsharpAmount = 0.0f;   // 一次空間アップスケール (mode 5) のアンシャープ量
                                                // = UpscaleSoftness x max(0, 1 - (In.x*In.y)/(O.x*O.y))
    unsigned int VisualizeMode = 0;             // Temporal AA デバッグ表示 (ETemporalAADebugView)
    float        VisualizeScale = 1.0f;         // デバッグ表示の増幅 (FTemporalAADebugSettings::VisualizeScale)
};
static_assert(offsetof(PP_SETTINGS, UpscaleUnsharpAmount) == 164, "b4 UpscaleUnsharpAmount offset (was _pp_pad0)");
static_assert(offsetof(PP_SETTINGS, VisualizeMode) == 168, "b4 VisualizeMode offset (was _pp_pad1)");
static_assert(offsetof(PP_SETTINGS, VisualizeScale) == 172, "b4 VisualizeScale offset (was _pp_pad2)");

// Compile-time guard: HLSL POSTPROCESS block layout must match.
//   group0 16 + group1 16 + grading 5*16=80 + misc 16 + DOF 2*16=32 + flags-block 16
//   = 16 + 16 + 80 + 16 + 32 + 16 = 176 bytes.
static_assert(sizeof(PP_SETTINGS) == 176, "PP_SETTINGS must be 176 bytes / 16-byte aligned to match HLSL POSTPROCESS");
