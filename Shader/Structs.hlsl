#ifndef STRUCTS_HLSL
#define STRUCTS_HLSL

struct VS_INPUT
{
    float3 Position : POSITION;
    float3 Normal : NORMAL;
    float3 Tangent : TANGENT;
    float2 TexCoord : TEXCOORD;
    float4 Color : COLOR;
};

struct PS_INPUT
{
    float4 Position : SV_POSITION;
    float4 WorldPosition : POSITION;
    float4 Normal : NORMAL;
    float3 Tangent : TANGENT;
    float2 TexCoord : TEXCOORD;
    float4 Color : COLOR;
};


// ベースパス MRT 出力。ワールド座標は書かない
// (ライティング側で深度から再構築)。
//   RT0 = GBufferC (BaseColor / Substrate では DiffuseAlbedo),
//   RT1 = GBufferA (Normal),
//   RT2 = GBufferB (Metallic/Specular/Roughness/AO),
//   RT3 = SubstrateMaterial0 (Slab パック 0。x = ヘッダ,
//         非 Substrate ピクセルは 0 = レガシー経路),
//   RT4 = SubstrateMaterial1 (Slab パック 1)
struct PS_OUTPUT_GEOMETRY
{
    float4 Color : SV_TARGET0;
    float4 Normal : SV_TARGET1;
    float4 MSRA : SV_TARGET2;
    uint4 SubstrateData0 : SV_TARGET3;
    uint4 SubstrateData1 : SV_TARGET4;
};

struct PS_OUTPUT
{
    float4 Color : SV_TARGET0;
};

// =============================================================
//  Lights
// =============================================================

// ---- ライト種別 (C++ ELightType と 1:1) ----
#define LIGHT_TYPE_DIRECTIONAL 0
#define LIGHT_TYPE_POINT       1
#define LIGHT_TYPE_SPOT        2
#define LIGHT_TYPE_RECT        3

// ---- ライトフラグ (C++ LIGHT_FLAG_* と 1:1) ----
#define LIGHT_FLAG_INVERSE_SQUARED (1u << 0)

// FLightShaderParameters。
// C++ 側 (LightSceneProxy.h) と 1:1 ミラー必須 (逐次パック 96 bytes)。
// StructuredBuffer 用なので cbuffer の 16 バイト境界規則は適用されない。
// Direction は「発光方向」(受光面 -> ライトではない) 点に注意。
struct FLightShaderParameters
{
    float3 Position; // ワールド位置 [m]
    float InvRadius; // 1 / AttenuationRadius

    float3 Color; // 線形色 x 強度 (cd 相当)
    float FalloffExponent; // 逆二乗無効時の指数フォールオフ

    float3 Direction; // 発光方向 (正規化)
    float SpecularScale; // スペキュラ寄与スケール

    float3 Tangent; // 幅軸 (Rect の幅 / チューブの軸)
    float SourceRadius; // 球光源半径 (Rect では半幅) [m]

    float2 SpotAngles; // x = cos(Outer), y = 1 / (cos(Inner) - cos(Outer))
    float SoftSourceRadius; // 見かけだけ柔らかくする追加半径 [m]
    float SourceLength; // チューブ長 (Rect では半高) [m]

    float RectLightBarnCosAngle; // バーンドア開き角の cos
    float RectLightBarnLength; // バーンドア長 [m]
    uint Type; // LIGHT_TYPE_*
    uint Flags; // LIGHT_FLAG_*
};

// FLocalShadowParameters。
// C++ 側 (ShadowRendering.h) と 1:1 ミラー必須 (逐次パック 96 bytes)。
// ライトバッファ (t13) と同じインデックスで 1:1 対応する。
struct FLocalShadowParameters
{
    float4x4 WorldToShadow; // Spot/Rect: ワールド -> シャドウクリップ (転置済み)。Point は未使用
    int ShadowSliceIndex; // -1 = 影なし。Point は 6 面の先頭スライス
    float ShadowNearPlane; // Point のデバイス深度再構築用
    float ShadowFarPlane; // Point のデバイス深度再構築用 (= 減衰半径)
    float DepthBiasNDC; // 受光側深度バイアス (NDC)
    float InvShadowResolution; // 1 / LOCAL_SHADOW_RESOLUTION (PCF オフセット)
    float NormalOffsetWorld; // 受光側法線オフセット [m] (DF ライトは SlopeBias 由来の値で上書き)
    float DFShadow; // 1 = シャドウマップの代わりにメッシュ SDF をレイマーチ
    float DFSelfShadowBias; // DF 用レイ方向自己遮蔽オフセット [m] (ShadowBias 由来)
};

// -------------------------------------------------------------
//  FDFObjectData (C++ ShadowRendering.h と 1:1 ミラー, 96 bytes)
//  Distance Field オブジェクト 1 つ分。StructuredBuffer<FDFObjectData>
//  (t18, DFObjects) として毎フレーム詰め直される。
// -------------------------------------------------------------
struct FDFObjectData
{
    float4x4 WorldToVolume; // ワールド -> ボリューム空間 [-1,1] (転置済み)
    float4 VolumeUVScaleAndDistance; // xyz=アトラス UV スケール, w=距離値 -> ワールド距離 [m]
    float4 VolumeUVAdd; // xyz=アトラス UV オフセット, w=未使用
};

// =============================================================
//  Lumen Surface Cache (LumenScene.h と 1:1 ミラー必須)
// =============================================================

// -------------------------------------------------------------
//  FLumenSceneObject (112 bytes)
//  Lumen シーンのオブジェクト 1 つ分 (FLumenPrimitiveGroup 相当)。
//  メッシュ SDF (トレース用) + カード範囲 (Surface Cache 参照用) を
//  持つ。StructuredBuffer<FLumenSceneObject> (t24, LumenSceneObjects)
//  として FLumenSceneData が毎フレーム詰め直す。
//  SDF フィールドの意味は FDFObjectData と同一だが、VolumeUVAdd.w に
//  「SDF 1 ボクセルのワールド幅 [m]」(レイマーチの歩幅 / バイアス基準)
//  が入る点だけ異なる。
// -------------------------------------------------------------
struct FLumenSceneObject
{
    float4x4 WorldToVolume; // ワールド -> SDF ボリューム空間 [-1,1] (転置済み)
    float4 VolumeUVScaleAndDistance; // xyz=アトラス UV スケール, w=距離値 -> ワールド距離 [m]
    float4 VolumeUVAdd; // xyz=アトラス UV オフセット, w=SDF 1 ボクセルのワールド幅 [m]
    uint CardOffset; // LumenCardBuffer 内の先頭カードインデックス
    uint NumCards; // カード数 (LUMEN_CARDS_PER_OBJECT)
    uint bValid; // 0 = 空スロット (スキップ)
    uint Pad0;
};

// -------------------------------------------------------------
//  FLumenCardData (176 bytes)
//  Lumen カード 1 枚分 (FLumenCard 相当)。
//  カード空間 = キャプチャビュー空間:
//    原点 = カードカメラ位置, XY = カード平面, +Z = 面へ向かう奥行き
//    (深度レンジは [0, 2 * CardExtent.z])。
//  StructuredBuffer<FLumenCardData> (t25, LumenCardBuffer)。
// -------------------------------------------------------------
struct FLumenCardData
{
    float4x4 WorldToCard; // ワールド -> カード空間 (転置済み)
    float4x4 CardToWorld; // カード空間 -> ワールド (転置済み)
    float4 CardExtentAndValid; // xyz=カード半幅 (x,y=平面, z=半深度), w=有効 (0/1)
    float4 AtlasUVScaleBias; // カード UV [0,1] -> アトラス UV (xy=スケール, zw=オフセット)
    float4 CardDirection; // xyz=ワールド空間カード法線 (面の外向き), w=未使用
};

#endif
