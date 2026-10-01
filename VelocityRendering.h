#pragma once
#include <DirectXMath.h>

using namespace DirectX;

struct FViewInfo;
class FPrimitiveSceneProxy;

// ============================================================
//  VelocityRendering (UE VelocityRendering.cpp)
//  ベロシティパスの判定と、HLSL VelocityCommon.hlsl のエンコードの CPU 鏡像。
//  FSceneRenderer::RenderVelocities と FSceneVelocityData の実装も
//  VelocityRendering.cpp にある。
//
//  ベロシティパス (ベースパスの後の別パス = UE4 既定 / UE5 r.VelocityOutputPass=2 相当) は
//  前フレームから LocalToWorld が変わったプリミティブの Opaque / Masked サブセットだけを
//  R16G16_UNORM の Velocity へ描く。書かなかった画素は 0 (未書き込み) のままで、
//  TAA / 可視化は深度と ClipToPrevClip からカメラモーションを再構築する。
// ============================================================

// UE FPostProcessSettings::MotionBlurPerObjectSize の既定 [M]。画面上で小さ過ぎる
// (境界半径 < 距離の 1 %) プリミティブは速度を描かずカメラモーションに任せる (リスク R10)
constexpr float kMotionBlurPerObjectSize = 0.5f;

// UE PrimitiveHasVelocityForView: カメラカット中は描かない + 小物体スキップ
// (bDisableSmallObjectCull = FTemporalAADebugSettings::bDisableVelocitySmallObjectCull)
bool PrimitiveHasVelocityForView(const FViewInfo& View, const FPrimitiveSceneProxy& Proxy, bool bDisableSmallObjectCull);

// VelocityCommon.hlsl と同式の CPU 版 (自己テスト T6 用)
//   Encode: clamp(V, -2, 2) * 0.2495 + 32767/65535  (R16G16_UNORM の正規化値)
//   Decode: (E - 32767/65535) / 0.2495
XMFLOAT2 EncodeVelocityToTextureCPU(XMFLOAT2 V);
XMFLOAT2 DecodeVelocityFromTextureCPU(XMFLOAT2 E);
