#ifndef RESOURCES_HLSL
#define RESOURCES_HLSL

#include "Structs.hlsl"

// =============================================================
//  SRV レジスタレイアウト (C++ TEXTURE_TYPE と 1:1 ミラー必須)
//  ワールド座標 (深度 + InvViewProjection から再構築する)。
// =============================================================

// ---- GBuffer / シーンリソース ----
// t0..t2 はベースパスではマテリアルテクスチャ (BaseColor/Normal/ARM)、
// ライティングパスでは G-Buffer (GBufferC/GBufferA/GBufferB) として読む。
Texture2D<float4> TextureBaseColor : register(t0); // GBufferC: BaseColor (Tonemap/Bloom では HDR scene)
Texture2D<float4> TextureNormal : register(t1); // GBufferA: World Normal (.w = ライティングマーカー)
Texture2D<float4> TextureMSRA : register(t2); // GBufferB: R=Metallic G=Specular B=Roughness A=AO
Texture2D<float> TextureDepth : register(t3); // 非線形深度
Texture2D<float2> TextureLinearDepth : register(t4); // R=view距離 G=正規化0..1
Texture2D<float4> TextureEnvironment : register(t5); // 環境マップ (equirect)

// ---- IBL ----
TextureCube<float4> IrradianceCube : register(t6); // 拡散 irradiance (畳み込み済)
TextureCube<float4> PrefilterCube : register(t7); // roughness 別 specular (ミップ畳み込み済)
Texture2D<float2> BRDFLut : register(t8); // (scale, bias) 統合 LUT

// ---- PostProcess ----
Texture2D<float4> TextureBloom : register(t9); // ブルーム結果 / 2nd ブルーム入力
Texture3D<float4> ColorGradingLUT : register(t10); // 3D グレーディング LUT

// ---- Auto Exposure (Eye Adaptation) ----
// [0] = 適応済み露出スケール (Tonemap で乗算)
// [1] = 計測した平均輝度 (デバッグ表示用)
Buffer<float> AutoExposureBuffer : register(t11);

// ---- Depth of Field ----
// RGB = プリマルチプライ済みブラー色, A = 正規化 CoC 重み (合成係数)
Texture2D<float4> TextureDOFBlur : register(t12); // ハーフ解像度 DOF ブラー

// ---- Lights ----
// ローカルライト (Point/Spot/Rect)。有効数は b3 (ForwardLightData) の
// NumLocalLights。毎フレーム FSceneRenderer::SetupLightConstants が詰め直す。
StructuredBuffer<FLightShaderParameters> ForwardLocalLights : register(t13);

// ---- Shadow maps ----
// CSM (t14) はディレクショナル、ローカルアトラス (t15) は
// Spot/Rect が 1 スライス、Point がキューブ 6 面で 6 スライスを使う。
// LocalShadowParams (t16) はライトバッファ (t13) と同じインデックスで
// 1:1 対応する (FShadowSceneRenderer が毎フレーム詰め直す)。
Texture2DArray<float> DirectionalShadowCascades : register(t14); // CSM カスケード深度
Texture2DArray<float> LocalLightShadows : register(t15); // ローカルライトシャドウアトラス
StructuredBuffer<FLocalShadowParameters> LocalShadowParams : register(t16); // ローカルシャドウ投影パラメータ

// ---- Distance Field Shadows (t17-t18) ----
// アトラスはメッシュ単位の SDF (64^3) を X 方向にスロット配置した
// Texture3D (DistanceFieldAtlas.h)。DFObjects は可視キャスターから
// 毎フレーム詰め直される (FShadowSceneRenderer::UpdateDistanceFieldObjects)。
Texture3D<float> DistanceFieldAtlasTexture : register(t17); // メッシュ SDF アトラス
StructuredBuffer<FDFObjectData> DFObjects : register(t18); // DF オブジェクトデータ

// ---- Light Grid (タイルドライトカリング, t19-t20) ----
// LightGridInjection_CS / LightGridCompact_CS が毎フレーム構築する
// クラスタードライトグリッド。セルレイアウトは b3 (ForwardLightData)
// のグリッドパラメータで決まり、受光側は LightGridCommon.hlsl の
// ヘルパで参照する。格納されるインデックスはライトバッファ (t13) /
// ローカルシャドウパラメータ (t16) と共通。
StructuredBuffer<uint> NumCulledLightsGrid : register(t19); // セルごとの [ライト数, データ開始] (stride 2)
StructuredBuffer<uint> CulledLightDataGrid : register(t20); // カリング済みライトインデックス列

// ---- Refraction (t21) ----
// 屈折用シーンカラーコピー。半透明パス直前に SceneColor から
// CopyResource され、半透明 PS が屈折オフセット付きで読む
// (RefractionCommon.hlsl)。
Texture2D<float4> SceneColorCopyTexture : register(t21);

// ---- Substrate (t22-t23) ----
// Slab BSDF のパックデータ (ベースパス SV_TARGET3/4 の書き先)。
// UINT テクスチャなのでライティングパスは Load でピクセル読みする。
// x = ヘッダ (0 = 非 Substrate ピクセル -> レガシー経路)。
// スロット割りは SubstrateDefinitions.hlsl 参照。
Texture2D<uint4> SubstrateMaterial0 : register(t22);
Texture2D<uint4> SubstrateMaterial1 : register(t23);

// ---- Lumen Surface Cache (t24-t27) ----
// FLumenSceneData (LumenScene.h) が毎フレーム更新する。
// スクリーン GI (DeferredPS) はオブジェクトの SDF (t17 と共用) を
// コーントレースし、ヒット先の FinalLighting アトラス (直接光 +
// Radiosity 間接光 + Emissive 合成済み) を採光する。
// 深度アトラスはカードから見えないテクセルの棄却 (リーク防止) 用。
StructuredBuffer<FLumenSceneObject> LumenSceneObjects : register(t24); // Lumen オブジェクト列
StructuredBuffer<FLumenCardData> LumenCardBuffer : register(t25); // Lumen カード列
Texture2D<float4> LumenFinalLightingAtlas : register(t26); // Surface Cache FinalLighting (a=有効率)
Texture2D<float> LumenDepthAtlas : register(t27); // カードキャプチャ深度 (0..1)

// ---- Lumen Final Gather / Reflections / Radiance Cache (t28-t32) ----
// t28: Screen Probe Gather の積分結果 (rgb=平均入射ラディアンス, a=スカイ可視率)
// t29: 反射ラディアンス (rgb=プレフィルタ差し替え値, a=差し替え率)
// t30-t32: Radiance Cache SH L1 ボリューム (トロイダル。WRAP サンプラで
//          トライリニア採光。b6 の LumenRadianceCacheParams 参照)
Texture2D<float4> LumenDiffuseIndirectTexture : register(t28);
Texture2D<float4> LumenReflectionTexture : register(t29);
Texture3D<float4> LumenRCSH_R : register(t30);
Texture3D<float4> LumenRCSH_G : register(t31);
Texture3D<float4> LumenRCSH_B : register(t32);

// ---- サンプラー ----
SamplerState Sampler : register(s0); // ANISOTROPIC, WRAP
SamplerState Sampler2 : register(s1); // LINEAR, CLAMP
SamplerComparisonState ShadowSampler : register(s2); // 比較 (LESS_EQUAL), BORDER = 白 (マップ外は影なし)

#endif
