#include "Common.hlsl"
#include "VertexFactory.hlsl"
#include "BasePassVertexCommon.hlsl"

// =============================================================
//  GeometryVS (ベースパス / トランスルーセンシー / Responsive AA / Lumen カード)
//  頂点ファクトリ (VertexFactory.hlsl) 経由でローカル空間の
//  位置 / 法線 / 接線を取る。GeometryVS_GPUSkin.hlsl が
//  GPUSKIN_VERTEX_FACTORY を定義して本ファイルを include する。
// =============================================================

PS_INPUT main(FVertexFactoryInput input)
{
    PS_INPUT output;

    const FVertexFactoryIntermediates vfi = GetVertexFactoryIntermediates(input);
    const float3 localPosition = VertexFactoryGetLocalPosition(input, vfi);

    // SV_Position はベロシティ / Responsive マスクのパスとビット一致させるため共通ヘルパで求める
    // (BasePassVertexCommon.hlsl, precise = INVARIANT)
    output.Position = GetBasePassClipPosition(localPosition);

    float4 position = float4(localPosition, 1.0f);
    output.WorldPosition = mul(position, LocalToWorld);

    float4 normal = float4(VertexFactoryGetLocalNormal(input, vfi), 0.0f);
    output.Normal = mul(normal, LocalToWorld);

    float4 tangent = float4(VertexFactoryGetLocalTangent(input, vfi), 0.0f);
    output.Tangent = mul(tangent, LocalToWorld).xyz;

    output.TexCoord = input.TexCoord;

    output.Color = input.Color;

    return output;
}
