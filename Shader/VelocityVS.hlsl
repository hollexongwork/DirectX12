#include "Common.hlsl"
#include "VertexFactory.hlsl"
#include "BasePassVertexCommon.hlsl"
#include "VelocityCommon.hlsl"

// =============================================================
//  VelocityVS (FVelocityVS)
//  ベロシティパス (FSceneRenderer::RenderVelocities) の頂点シェーダ。
//  SV_Position はベースパス (GeometryVS) と同じ GetBasePassClipPosition で求め、
//  深度 LESS_EQUAL (書き込み無し) がベースパス深度とビット一致するようにする。
//  前フレーム位置 = 頂点ファクトリの前フレームローカル位置 (GPU スキンでは前フレームの
//  ボーン行列 t40) -> b1 PreviousLocalToWorld -> b0 PrevViewProjection (前フレームのジッタ込み)。
//  ジッタは PS が TemporalAAJitter.xy / .zw で取り除く。
//  VelocityVS_GPUSkin.hlsl が GPUSKIN_VERTEX_FACTORY を定義して本ファイルを include する。
// =============================================================

VELOCITY_VS_OUTPUT main(FVertexFactoryInput input)
{
    VELOCITY_VS_OUTPUT o;

    const FVertexFactoryIntermediates vfi = GetVertexFactoryIntermediates(input);
    const float3 localPosition = VertexFactoryGetLocalPosition(input, vfi);
    const float3 previousLocalPosition = VertexFactoryGetPreviousLocalPosition(input, vfi);

    o.Position        = GetBasePassClipPosition(localPosition);                   // ベースパスとビット一致
    o.PackedVelocityA = o.Position;
    const float4 prevWorld = mul(float4(previousLocalPosition, 1.0f), PreviousLocalToWorld);
    o.PackedVelocityC = mul(prevWorld, PrevViewProjection);                        // 前フレームのジッタ込み
    o.TexCoord = input.TexCoord;
    o.Color    = input.Color;
    return o;
}
