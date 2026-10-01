#ifndef VELOCITY_PIXEL_SHADER_HLSL
#define VELOCITY_PIXEL_SHADER_HLSL

// =============================================================
//  VelocityPixelShader.hlsl (UE FVelocityPS)
//  ベロシティパスのピクセルシェーダ本体。ラッパが VELOCITY_MASKED (0/1) を
//  定義してから include する:
//    VelocityPS.hlsl       : VELOCITY_MASKED 0 (Opaque / BaseColor 無しの Masked)
//    VelocityMaskedPS.hlsl : VELOCITY_MASKED 1 (Masked: t0 BaseColor + b2 Material で clip)
//
//  出力 RG = EncodeVelocityToTexture(ScreenPos - PrevScreenPos) (R16G16_UNORM)。
//  両 clip 位置から今 / 前フレームの NDC ジッタ (b0 TemporalAAJitter.xy / .zw) を
//  引くのでジッタ無しの速度になる (Appendix A.4)。
// =============================================================

#ifndef VELOCITY_MASKED
#error "VELOCITY_MASKED (0/1) を定義してから VelocityPixelShader.hlsl を include すること"
#endif

#include "Common.hlsl"
#include "VelocityCommon.hlsl"

float4 main(VELOCITY_VS_OUTPUT input) : SV_TARGET0
{
#if VELOCITY_MASKED
    // GeometryPS と同一の被覆判定 (同じ SampleBias / 同じ頂点カラー乗算 / 同じしきい値)。穴に速度を書かない
    const float4 baseColor = TextureBaseColor.SampleBias(Sampler, input.TexCoord, MaterialTextureMipBias) * input.Color;
    if (Material.BlendMode == BLEND_MASKED)
    {
        clip(baseColor.a - Material.OpacityMaskClipValue);
    }
#endif
    if (input.PackedVelocityC.w <= 1.0e-4f)                                        // [PORT] 前フレーム位置がカメラ背後
    {
        return float4(EncodeVelocityToTexture(float2(2.0f, 2.0f)), 0.0f, 0.0f);    // -> HSP = s - 2 が画面外 -> 履歴棄却
    }
    const float2 ScreenPos     = input.PackedVelocityA.xy / input.PackedVelocityA.w - TemporalAAJitter.xy;
    const float2 PrevScreenPos = input.PackedVelocityC.xy / input.PackedVelocityC.w - TemporalAAJitter.zw;
    return float4(EncodeVelocityToTexture(ScreenPos - PrevScreenPos), 0.0f, 0.0f); // RG のみ格納 (R16G16_UNORM)
}

#endif
