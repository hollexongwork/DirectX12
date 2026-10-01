#ifndef BASE_PASS_VERTEX_COMMON_HLSL
#define BASE_PASS_VERTEX_COMMON_HLSL

// =============================================================
//  BasePassVertexCommon.hlsl
//  GeometryVS / VelocityVS 共通 (UE INVARIANT 相当)。ベロシティ / Responsive マスクの
//  LESS_EQUAL がベースパス / 半透明プリパス深度とビット一致するよう、SV_Position は
//  必ずこの関数で求めること (precise で式の並べ替え / 融合を禁止する)。
//  Common.hlsl (b0 View / Projection, b1 LocalToWorld) の後に include する。
// =============================================================

float4 GetBasePassClipPosition(float3 LocalPosition)
{
    precise float4x4 wvp = mul(mul(LocalToWorld, View), Projection);   // 既存 GeometryVS と同じ式・同じ順序
    precise float4 clip = mul(float4(LocalPosition, 1.0f), wvp);
    return clip;
}

#endif
