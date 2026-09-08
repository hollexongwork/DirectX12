#ifndef LUMEN_SCENE_LIGHTING_COMMON_HLSL
#define LUMEN_SCENE_LIGHTING_COMMON_HLSL

// =============================================================
//  LumenSceneLightingCommon
//  Lumen コンピュートパス群の共通レイアウト。
//  C++ 側 FLumenSceneData の独立コンピュートルートシグネチャと
//  1:1 ミラー必須 (LumenScene.cpp InitComputePipelines):
//
//    [0]      b0      : FLumenPassParams (ルート CBV)
//    [1..28]  t0..t27 : SRV テーブル
//    [29..36] u0..u7  : UAV テーブル
//    [37]     t28     : TLAS (ルート SRV, HWRT バリアントのみ参照)
//    s0               : リニアクランプ
//
//  ---- SRV レジスタ規約 ----
//  t0..t14  : 全パス共通 (Lumen シーン / アトラス / IBL / Global SDF)
//  t15..t18 : スクリーン系パス共通 (G-Buffer / 深度 / 履歴)
//  t19..t27 : パス固有 (各シェーダが用途を宣言する)
//
//  ---- UAV レジスタ規約 ----
//  u0..u3   : Direct / Indirect / Final / Global SDF クリップマップ
//  u4..u7   : パス固有出力
// =============================================================

#include "Structs.hlsl"

#ifndef LUMEN_PI
#define LUMEN_PI 3.14159265358979323846f
#endif

// アトラスのタイル列数 (C++ LUMEN_ATLAS_TILES_X と 1:1)
#define LUMEN_ATLAS_TILES_X 16u

// Global Distance Field クリップマップ解像度 (C++ LUMEN_GLOBAL_SDF_RESOLUTION と 1:1)
#define LUMEN_GLOBAL_SDF_RESOLUTION 128.0f

// -------------------------------------------------------------
//  b0 : FLumenPassParams (C++ LumenScene.h と 1:1 ミラー必須, 512B)
// -------------------------------------------------------------
cbuffer LumenPassParams : register(b0)
{
    uint CardStartIndex; // 先頭カード (GDF ビルドではクリップマップ番号)
    uint NumCardsToProcess; // 処理カード数 (GroupID.z の上限)
    uint PassNumLumenObjects; // 有効 Lumen オブジェクト数
    uint PassNumLocalLights; // ローカルライト有効数

    float4 PassDirectionalLightDirection; // xyz=受光面->ライト, w=有効 (0/1)
    float4 PassDirectionalLightColor; // rgb=線形色 x 強度 (lux)
    float4 PassAtlasParams; // xy=1/アトラスサイズ, z=カード解像度, w=フレーム番号
    float4 PassTraceParams; // x=最大トレース距離[m], y=面バイアス[m], z=Radiosityレイ数, w=Emissiveブースト
    float4 PassGlobalSDF0; // xyz=クリップマップ0中心, w=半径 [m] (0 = 無効)
    float4 PassGlobalSDF1; // xyz=クリップマップ1中心, w=半径 [m]
    float4 PassProbeParams0; // x=プローブ数X, y=プローブ数Y, z=ダウンサンプル(16), w=octa解像度(8)
    float4 PassProbeParams1; // x=画面幅, y=画面高, z=テンポラルα, w=履歴有効(0/1)
    float4 PassCameraOrigin; // xyz=カメラ位置, w=ディテールトレース距離 [m]
    float4 PassPrevCameraOrigin; // xyz=前フレームカメラ位置, w=スクリーントレース厚み [m]
    float4 PassRCParams0; // xyz=Radiance Cache 最小コーナー, w=プローブ間隔 [m]
    float4 PassRCParams1; // x=プローブ数/軸, y=更新開始プローブ, z=更新プローブ数, w=スカイサンプルミップ
    float4 PassReflectionParams; // x=最大ラフネス, y=フェード開始, z=強度, w=スクリーントレース有効
    float4 PassRadiosityParams; // x=Radiosity テンポラルα, y=フル解像度 GI テンポラルα (1 = 蓄積なし), zw=予約

    float4x4 PassViewProjection; // ワールド -> クリップ (転置済み)
    float4x4 PassInvViewProjection; // クリップ -> ワールド (転置済み)
    float4x4 PassPrevViewProjection; // 前フレームのワールド -> クリップ (転置済み)
    float4x4 PassPrevInvViewProjection; // 前フレームのクリップ -> ワールド (転置済み)

    float4 PassProbeJitter; // xy=今フレームのプローブ配置ジッタ [px], zw=前フレーム (履歴のリプロジェクション用)
};

// -------------------------------------------------------------
//  共通リソース (t0..t14 / u0..u3)
// -------------------------------------------------------------
Texture3D<float> LumenDistanceFieldAtlas : register(t0);
StructuredBuffer<FLumenSceneObject> LumenSceneObjects : register(t1);
StructuredBuffer<FLumenCardData> LumenCardBuffer : register(t2);
StructuredBuffer<FLightShaderParameters> LumenLocalLights : register(t3);

Texture2D<float4> LumenAlbedoAtlas : register(t4);
Texture2D<float4> LumenNormalAtlas : register(t5);
Texture2D<float4> LumenEmissiveAtlas : register(t6);
Texture2D<float> LumenDepthAtlas : register(t7);

Texture2D<float4> LumenDirectLightingSRV : register(t8);
Texture2D<float4> LumenIndirectLightingSRV : register(t9);
Texture2D<float4> LumenFinalLightingAtlas : register(t10);
TextureCube<float4> LumenSkyIrradiance : register(t11);

Texture3D<float> LumenGlobalSDF0 : register(t12); // クリップマップ 0 (近距離 / 高精細)
Texture3D<float> LumenGlobalSDF1 : register(t13); // クリップマップ 1 (遠距離)
TextureCube<float4> LumenSkyPrefilter : register(t14); // prefilter 環境 (ミップ付き)

// ---- スクリーン系パス共通 (t15..t18) ----
Texture2D<float4> LumenGBufferNormal : register(t15); // GBufferA (World Normal)
Texture2D<float> LumenSceneDepth : register(t16); // 非線形デバイス深度
Texture2D<float2> LumenLinearDepth : register(t17); // R=ビュー距離
Texture2D<float4> LumenPrevSceneColor : register(t18); // 前フレーム SceneColor (線形 HDR)
Texture2D<float2> LumenPrevLinearDepth : register(t25); // 前フレーム LinearDepth (R=ビュー距離。履歴検証用)

RWTexture2D<float4> RWDirectLighting : register(u0);
RWTexture2D<float4> RWIndirectLighting : register(u1);
RWTexture2D<float4> RWFinalLighting : register(u2);
RWTexture3D<float> RWGlobalSDF : register(u3);

SamplerState LumenTraceSampler : register(s0);

// このヘッダの利用者は Global SDF / ハイブリッドトレースを使える
#define LUMEN_SUPPORTS_GLOBAL_SDF 1

#include "LumenTracingCommon.hlsl"

#if LUMEN_HWRT
#include "LumenTracingHardware.hlsl"
#endif

// -------------------------------------------------------------
//  トレースエントリポイント (SWRT / HWRT の切替点)
//  SWRT: メッシュ SDF (近距離) + Global SDF (遠距離) のハイブリッド
//  HWRT: RayQuery インライントレース (LumenTracingHardware.hlsl)
// -------------------------------------------------------------
FLumenTraceResult TraceLumenRay(float3 RayStart, float3 RayDir, float MaxT,
    float TanConeAngle, uint NumObjects)
{
#if LUMEN_HWRT
    return TraceLumenSceneHardware(RayStart, RayDir, MaxT, NumObjects);
#else
    return TraceLumenSceneHybrid(RayStart, RayDir, MaxT, TanConeAngle, NumObjects,
        PassCameraOrigin.w, PassGlobalSDF0, PassGlobalSDF1);
#endif
}

// -------------------------------------------------------------
//  カードインデックス -> アトラスタイル原点 [px]
// -------------------------------------------------------------
uint2 GetLumenCardTileOrigin(uint CardIndex)
{
    uint res = (uint) PassAtlasParams.z;
    return uint2(CardIndex % LUMEN_ATLAS_TILES_X, CardIndex / LUMEN_ATLAS_TILES_X) * res;
}

// -------------------------------------------------------------
//  カードテクセルのサーフェス再構築
// -------------------------------------------------------------
struct FLumenCardTexel
{
    bool bValid; // ジオメトリが焼かれたテクセルか
    uint2 AtlasTexel; // アトラス上のテクセル座標
    float3 WorldPosition;
    float3 WorldNormal;
};

FLumenCardTexel ReconstructLumenCardTexel(FLumenCardData Card, uint CardIndex, uint2 TexelInCard)
{
    FLumenCardTexel result;

    result.AtlasTexel = GetLumenCardTileOrigin(CardIndex) + TexelInCard;

    // 有効判定: キャプチャ済みテクセルは Albedo.a = 1 (クリア値 0)
    float validity = LumenAlbedoAtlas.Load(int3(result.AtlasTexel, 0)).a;
    result.bValid = (validity > 0.5f);

    // カードビュー空間位置 (オルソ: NDC xy + 線形深度 z)
    float res = PassAtlasParams.z;
    float2 uv = ((float2) TexelInCard + 0.5f) / res;
    float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);

    float depth = LumenDepthAtlas.Load(int3(result.AtlasTexel, 0));
    float3 ext = Card.CardExtentAndValid.xyz;
    float3 cardPos = float3(ndc.x * ext.x, ndc.y * ext.y, depth * 2.0f * ext.z);

    result.WorldPosition = mul(float4(cardPos, 1.0f), Card.CardToWorld).xyz;

    // カード空間法線 -> ワールド (非均一スケールは正規化で近似)
    float3 cardNormal = LumenNormalAtlas.Load(int3(result.AtlasTexel, 0)).xyz * 2.0f - 1.0f;
    result.WorldNormal = normalize(mul(float4(cardNormal, 0.0f), Card.CardToWorld).xyz);

    return result;
}

// -------------------------------------------------------------
//  レイヒットのラディアンス解決 (共通)
//  ヒット: SDF 勾配法線で Surface Cache を採光
//  ミス  : prefilter 環境キューブをスカイラディアンスとして採光
// -------------------------------------------------------------
float3 ResolveLumenRayRadiance(FLumenTraceResult Trace, float3 RayStart, float3 RayDir,
    float SkySampleMip)
{
    [branch]
    if (Trace.bHit)
    {
        FLumenSceneObject hitObj = LumenSceneObjects[Trace.HitObject];
        float3 hitPos = RayStart + RayDir * Trace.HitT;
        float3 hitNormal = ComputeLumenHitNormal(hitObj, hitPos, RayStart);
        return SampleLumenSurfaceCache(hitObj, hitPos, hitNormal);
    }

    return LumenSkyPrefilter.SampleLevel(LumenTraceSampler, RayDir, SkySampleMip).rgb;
}

#endif
