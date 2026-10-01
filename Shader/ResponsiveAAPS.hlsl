// ResponsiveAAPS.hlsl
// Responsive AA マスク (FSceneRenderer::RenderResponsiveAAMask)。
// UE はトランスルーセンシーの描画でステンシル bit 3 (STENCIL_TEMPORAL_RESPONSIVE_AA) を立て、
// TAA の Responsive パスがそれを読む。本エンジンの深度バッファは D32_FLOAT (ステンシル無し) なので、
// bEnableResponsiveAA のマテリアルの半透明サブセットをレンダー解像度の R8_UNORM マスクへ 1 で描く [PORT]。
// VS は半透明と同じ GeometryVS (GetBasePassClipPosition = precise) なので、半透明深度プリパスの
// 深度に対する LESS_EQUAL がビット一致で通り、最前面の Translucent 層 (+ その手前の Additive) だけが残る。
// TAA (TemporalAA.hlsl) は入力画素 K でマスクを読み、0.5 を超えれば BlendFinal = 0.25 にする
#include "Common.hlsl"

float4 main(PS_INPUT input) : SV_TARGET0
{
    return float4(1.0f, 0.0f, 0.0f, 1.0f);   // R8_UNORM に 1 (UE: ステンシル bit 3)
}
