#ifndef REFRACTION_COMMON_HLSL
#define REFRACTION_COMMON_HLSL

#include "Common.hlsl"

// =============================================================
//  RefractionCommon
//  屈折 (Refraction) 実装。半透明マテリアル
//  (BLEND_Translucent) がシーンカラーコピー (t21) を屈折オフセット
//  付きでサンプルし、背景を置換合成する。
//
//  屈折方式 (ドキュメント / ERefractionMode と 1:1):
//    NONE                : 屈折なし (従来のハードウェアブレンド)
//    INDEX_OF_REFRACTION : 屈折率。ビュー空間法線 x (IOR - 1) の
//                          画面空間オフセット (スクリーン空間近似)
//    PIXEL_NORMAL_OFFSET : 頂点法線を基準に、ピクセル法線との差分
//                          から屈折 (平面ガラス向き。法線マップの
//                          摂動だけがオフセットになる)
//    2D_OFFSET           : Data.xy をピクセル単位オフセットとして
//                          直接使用
//
//  Refraction Depth Bias:
//    屈折先のシーン深度が「面の深度 + バイアス」より手前なら
//    オフセットを棄却して元 UV に戻す (面より手前のオブジェクトが
//    屈折に巻き込まれるのを防ぐ)。
//
//  意図的乖離 (MANIFEST 参照):
//    - 半透明パス内で直接シーンカラーを サンプルする方式 (シーンカラーコピー + 深度棄却)。
//
//    - 画角スケール定数 (REFRACTION_IOR_DISTORTION_SCALE)はDistortionParams 相当の調整定数。
// =============================================================

// ---- 屈折方式 (C++ Material.h の REFRACTION_METHOD_* と 1:1) ----
#define REFRACTION_METHOD_NONE                0u
#define REFRACTION_METHOD_INDEX_OF_REFRACTION 1u
#define REFRACTION_METHOD_PIXEL_NORMAL_OFFSET 2u
#define REFRACTION_METHOD_2D_OFFSET           3u

// 画角スケール定数 (DistortionParams 相当の調整値)
static const float REFRACTION_IOR_DISTORTION_SCALE = 0.25f;
// 画面 UV オフセットの上限 (遠すぎるサンプルの抑制)
static const float REFRACTION_OFFSET_CLAMP = 0.1f;
// 粗い屈折の最大ブラー半径 [texel]
static const float REFRACTION_ROUGH_BLUR_RADIUS = 12.0f;

// 粗い屈折用ポアソンタップ
static const float2 RefractionPoissonTaps[4] =
{
    float2(-0.94201624f, -0.39906216f),
    float2( 0.94558609f, -0.76890725f),
    float2(-0.09418410f, -0.92938870f),
    float2( 0.34495938f,  0.29387760f)
};

// -------------------------------------------------------------
//  FMaterialRefractionData
//  マテリアルテンプレートの同名構造体と 1:1:
//    Data.x  = IOR (INDEX_OF_REFRACTION) / 法線強度 (PIXEL_NORMAL_OFFSET)
//    Data.xy = 画面オフセット [pixel] (2D_OFFSET)
// -------------------------------------------------------------
struct FMaterialRefractionData
{
    float2 Data;
    float RefractionDepthBias;
};

FMaterialRefractionData GetMaterialRefraction()
{
    FMaterialRefractionData RefractionData;
    RefractionData.Data = Material.RefractionData;
    RefractionData.RefractionDepthBias = Material.RefractionDepthBias;
    return RefractionData;
}

float GetMaterialRefractionIOR(in FMaterialRefractionData RefractionData)
{
    return RefractionData.Data.x;
}

float2 GetMaterialRefraction2DOffset(in FMaterialRefractionData RefractionData)
{
    return RefractionData.Data.xy;
}

// ワールドベクトル -> ビュー空間 (行ベクトル規約: mul(v, View))
float3 TransformWorldVectorToView(float3 WorldVector)
{
    return mul(float4(WorldVector, 0.0f), View).xyz;
}

// -------------------------------------------------------------
//  ComputeRefractionOffsetUV
//  屈折による画面 UV オフセットを返す。
//    Projection._11/._22 = ビュー空間 xy -> NDC のスケール
//    (XMMatrixPerspectiveFovLH の対角成分。転置アップロードでも
//     対角は不変)。ビュー空間 +Y は上、UV +v は下なので Y 反転。
// -------------------------------------------------------------
float2 ComputeRefractionOffsetUV(
    uint Method,
    FMaterialRefractionData RefractionData,
    float3 WorldNormal,
    float3 WorldVertexNormal,
    float2 SceneTexelSize)
{
    float2 OffsetUV = float2(0.0f, 0.0f);

    [branch]
    if (Method == REFRACTION_METHOD_INDEX_OF_REFRACTION)
    {
        // ビュー空間法線の xy を IOR-1 でスケール
        float IOR = max(GetMaterialRefractionIOR(RefractionData), 1.0f);
        float3 ViewNormal = normalize(TransformWorldVectorToView(WorldNormal));

        OffsetUV = ViewNormal.xy * (IOR - 1.0f) * REFRACTION_IOR_DISTORTION_SCALE
                 * float2(Projection._11, -Projection._22);
    }
    else if (Method == REFRACTION_METHOD_PIXEL_NORMAL_OFFSET)
    {
        // 頂点法線とピクセル法線の差分のみをオフセットにする
        // (法線マップのない平面では歪まない)
        float Strength = GetMaterialRefractionIOR(RefractionData);
        float3 DeltaViewNormal = TransformWorldVectorToView(WorldNormal)
                               - TransformWorldVectorToView(WorldVertexNormal);

        OffsetUV = DeltaViewNormal.xy * Strength * REFRACTION_IOR_DISTORTION_SCALE
                 * float2(Projection._11, -Projection._22);
    }
    else if (Method == REFRACTION_METHOD_2D_OFFSET)
    {
        // ピクセル単位の直接オフセット
        OffsetUV = GetMaterialRefraction2DOffset(RefractionData) * SceneTexelSize;
    }

    return clamp(OffsetUV, -REFRACTION_OFFSET_CLAMP, REFRACTION_OFFSET_CLAMP);
}

// -------------------------------------------------------------
//  ResolveRefractedSceneUV
//  屈折 UV を確定する。屈折先が「面 + バイアス」より手前の
//  オブジェクトなら棄却して元 UV に戻す (前景の巻き込み防止)。
//  シーン深度は LinearDepth (t4, R = ビュー深度 [m])。半透明は
//  深度を書かないので面自身は棄却判定に現れない。
// -------------------------------------------------------------
float2 ResolveRefractedSceneUV(
    float2 SceneUV,
    float2 OffsetUV,
    float SurfaceViewDepth,
    float RefractionDepthBias,
    float2 SceneTexelSize)
{
    float2 RefractedUV = clamp(
        SceneUV + OffsetUV,
        SceneTexelSize * 0.5f,
        1.0f - SceneTexelSize * 0.5f);

    float RefractedSceneDepth = TextureLinearDepth.SampleLevel(Sampler2, RefractedUV, 0).r;

    [flatten]
    if (RefractedSceneDepth < SurfaceViewDepth + RefractionDepthBias)
    {
        RefractedUV = SceneUV;
    }
    return RefractedUV;
}

// -------------------------------------------------------------
//  SampleRefractedSceneColor
//  シーンカラーコピー (t21) のサンプル。粗い屈折 (Substrate の
//  Rough Refraction 相当) はラフネス比例の小タップブラーで近似
//  (シーンカラーは単一ミップのため。意図的乖離)。
// -------------------------------------------------------------
float3 SampleRefractedSceneColor(float2 UV, float Roughness, float2 SceneTexelSize)
{
    float3 Center = SceneColorCopyTexture.SampleLevel(Sampler2, UV, 0).rgb;

    [branch]
    if (Roughness <= 0.02f)
    {
        return Center;
    }

    float2 Radius = Roughness * REFRACTION_ROUGH_BLUR_RADIUS * SceneTexelSize;
    float3 Sum = Center;

    [unroll]
    for (int i = 0; i < 4; ++i)
    {
        Sum += SceneColorCopyTexture.SampleLevel(
            Sampler2, UV + RefractionPoissonTaps[i] * Radius, 0).rgb;
    }
    return Sum * (1.0f / 5.0f);
}

#endif
