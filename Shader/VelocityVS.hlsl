#include "Common.hlsl"
#include "BasePassVertexCommon.hlsl"
#include "VelocityCommon.hlsl"

// =============================================================
//  VelocityVS (UE FVelocityVS)
//  ベロシティパス (FSceneRenderer::RenderVelocities) の頂点シェーダ。
//  SV_Position はベースパス (GeometryVS) と同じ GetBasePassClipPosition で求め、
//  深度 LESS_EQUAL (書き込み無し) がベースパス深度とビット一致するようにする。
//  前フレーム位置 = b1 PreviousLocalToWorld -> b0 PrevViewProjection (前フレームのジッタ込み。
//  UE PrevTranslatedWorldToClip)。ジッタは PS が TemporalAAJitter.xy / .zw で取り除く。
// =============================================================

VELOCITY_VS_OUTPUT main(VS_INPUT input)
{
    VELOCITY_VS_OUTPUT o;
    o.Position        = GetBasePassClipPosition(input.Position);                  // ベースパスとビット一致
    o.PackedVelocityA = o.Position;
    const float4 prevWorld = mul(float4(input.Position, 1.0f), PreviousLocalToWorld);
    o.PackedVelocityC = mul(prevWorld, PrevViewProjection);                        // 前フレームのジッタ込み (UE PrevTranslatedWorldToClip)
    o.TexCoord = input.TexCoord;
    o.Color    = input.Color;
    return o;
}
