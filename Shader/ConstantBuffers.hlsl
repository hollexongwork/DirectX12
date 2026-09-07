#ifndef CONSTANT_BUFFERS_HLSL
#define CONSTANT_BUFFERS_HLSL

// =============================================================
//  定数バッファ
//  C++ 側 (RenderManager.h) の CONSTANT_TYPE と 1:1 ミラー必須:
//    b0 = VIEW / b1 = PRIMITIVE / b2 = MATERIAL /
//    b3 = FORWARD_LIGHT / b4 = POST_PROCESS / b5 = SHADOW
// =============================================================

// ---- PostProcess フラグ (PostProcess.Flags のビットマスク) ----
#define PP_FLAG_BLOOM          (1u << 0)
#define PP_FLAG_VIGNETTE       (1u << 1)
#define PP_FLAG_CHROMATIC      (1u << 2)
#define PP_FLAG_GRAIN          (1u << 3)
#define PP_FLAG_COLOR_GRADING  (1u << 4)
#define PP_FLAG_WHITE_BALANCE  (1u << 5)
#define PP_FLAG_ARTIST_LUT     (1u << 6)
#define PP_FLAG_AUTO_EXPOSURE  (1u << 7)
#define PP_FLAG_DOF            (1u << 8)

// -------------------------------------------------------------
//  b0 : View (FViewUniformShaderParameters 相当)
//  ビュー行列群 + カメラ + 代表ディレクショナルライト。
//  太陽ライトは View ユニフォームに常駐する。
// -------------------------------------------------------------
cbuffer ViewConstantBuffer : register(b0)
{
    float4x4 View;
    float4x4 Projection;
    float4x4 InvViewProjection;
    float4 WorldCameraOrigin; // xyz = カメラワールド位置 [m]
    float4 NearFar; // x=Near, y=Far
    // DirectionalLightDirection.xyz = 受光面からライトへ向かう方向 (発光方向の逆)
    // DirectionalLightColor.rgb     = 線形色 x 強度 (lux)。ライト不在時は 0 (無光)
    float4 DirectionalLightDirection;
    float4 DirectionalLightColor;
};

// -------------------------------------------------------------
//  b1 : Primitive (FPrimitiveUniformShaderParameters 相当)
//  per-draw のローカル→ワールド変換。
// -------------------------------------------------------------
cbuffer PrimitiveConstantBuffer : register(b1)
{
    float4x4 LocalToWorld;
};

// ---- Blend Mode (C++ EBlendMode と 1:1) ----
#define BLEND_OPAQUE      0u
#define BLEND_MASKED      1u
#define BLEND_TRANSLUCENT 2u
#define BLEND_ADDITIVE    3u

// -------------------------------------------------------------
//  b2 : Material (per-material PBR パラメータ, 224byte)
//  C++ 側 (Material.h) の MATERIAL と 1:1 ミラー必須。
//    Opacity              : Translucent / Additive の不透明度
//                           (BaseColor テクスチャ α x 頂点カラー α に乗算)
//    OpacityMaskClipValue : BLEND_MASKED の clip しきい値
//    BlendMode            : BLEND_* (上記)
//    TwoSided             : 裏面の法線反転 (SV_IsFrontFace 判定)
//
//  ---- Substrate Slab BSDF (UE5.8) ----
//    bUseSubstrate = true のときレガシー Metallic/Specular ワーク
//    フローの代わりに Slab (DiffuseAlbedo / F0 / F90 / SSS) で
//    シェーディングする。MFP は TransmittanceColor + Thickness から
//    TransmittanceToMeanFreePath で導出 (Substrate.hlsl)。
//    Thickness は UE 準拠の cm 単位オーサリング。
//
//  ---- Refraction (UE5.8) ----
//    RefractionMethod = REFRACTION_METHOD_* (RefractionCommon.hlsl)。
//    RefractionData.x = IOR / 法線強度, xy = 2D オフセット [pixel]。
// -------------------------------------------------------------
cbuffer MaterialConstantBuffer : register(b2)
{
    struct MATERIAL
    {
        float4 BaseColor;
        float4 EmissionColor;
        float Metallic;
        float Specular;
        float Roughness;
        float NormalWeight;
        bool Unlit;
        float Opacity;
        float OpacityMaskClipValue;
        uint BlendMode;
        bool TwoSided;
        float3 pad;

        // ---- Substrate Slab BSDF (UE5.8) ----
        float4 SubstrateDiffuseAlbedo; // rgb (w 未使用)
        float4 SubstrateF0; // rgb (w 未使用)
        float4 SubstrateF90; // rgb (w 未使用)
        float4 SubstrateTransmittanceColor; // rgb = 透過色 (指定厚での透過率) / w = 予約 (未使用)
        float4 SubstrateFuzzColor; // rgb = ファズ色 / w = FuzzAmount

        float SubstrateAnisotropy; // [-1,1] (評価は等方近似)
        float SubstrateSSSPhaseAnisotropy; // HG の g [-1,1]
        float SubstrateThickness; // スラブ厚 [cm] (UE 準拠)
        uint SubstrateSSSType; // SUBSTRATE_SSS_TYPE_*

        float SubstrateSecondRoughness;
        float SubstrateSecondRoughnessWeight;
        float SubstrateFuzzRoughness;
        bool SubstrateIsThin; // Thin Surface

        bool bUseSubstrate; // Slab ワークフロー有効
        uint RefractionMethod; // REFRACTION_METHOD_*
        float2 RefractionData; // IOR / 法線強度 / 2D オフセット

        float RefractionDepthBias; // [m]
        // Index Of Refraction From F0 (UE5.8):
        // TRUE のとき IOR を手入力値でなく Slab F0 から導出する
        // (DielectricF0ToIor(F0RGBToF0(F0))。Substrate + IOR 方式のみ)
        bool bRefractionUseF0;
        float2 _padSubstrate;
    } Material;
};

// -------------------------------------------------------------
//  b3 : ForwardLightData (FForwardLightData 相当)
//  ローカルライト (Point/Spot/Rect) の有効数 + タイルドライト
//  カリング (ライトグリッド) のパラメータ。ライト本体は
//  StructuredBuffer<FLightShaderParameters> (t13, ForwardLocalLights)、
//  グリッド本体は NumCulledLightsGrid (t19) + CulledLightDataGrid (t20)。
//  C++ 側 FORWARD_LIGHT_CONSTANT (RenderManager.h) と 1:1 ミラー必須。
//  受光側ヘルパは LightGridCommon.hlsl。
// -------------------------------------------------------------
cbuffer ForwardLightData : register(b3)
{
    uint NumLocalLights; // ローカルライト有効数
    uint NumGridCells; // グリッド総セル数 (X*Y*Z)
    uint CulledGridSizeX; // 画面タイル数 X (= ceil(W / LightGridPixelSize))
    uint CulledGridSizeY; // 画面タイル数 Y

    uint CulledGridSizeZ; // Z スライス数 (LIGHT_GRID_SIZE_Z)
    uint LightGridPixelSizeShift; // log2(LightGridPixelSize)
    uint MaxCulledLightsPerCell; // セルあたり保持するライト数上限
    uint LightGridDebugMode; // 0=off 1=複雑度ヒートマップ 2=Zスライス

    float3 LightGridZParams; // (B, O, S): Slice = log2(Depth*B + O) * S
    uint bUseLightGrid; // 0 = 全灯ループ (フォールバック)
};

// -------------------------------------------------------------
//  b4 : PostProcess (パス毎パラメータ)
//  PP_SETTINGSを各パスが b4 に詰め直す方式で対応
//  (テクセルサイズ等はパス内で更新される)。
// -------------------------------------------------------------
cbuffer PostProcessConstantBuffer : register(b4)
{
    struct POSTPROCESS
    {
        // --- group 0 : Exposure / Tonemapper ---
        float Exposure;
        uint TonemapperMode; // 0=ACES(Narkowicz) 1=ACES(Hill) 2=None(clamp)
        float BloomIntensity; 
        float BloomThreshold; 

        // --- group 1 : White Balance ---
        float WhiteTemp; // 1500..15000 K
        float WhiteTint;
        float ChromaticAberration; 
        float VignetteIntensity; 

        // --- group 2 : Color Grading (global) ---
        float4 ColorSaturation; 
        float4 ColorContrast; 
        float4 ColorGamma; 
        float4 ColorGain; 
        float4 ColorOffset; 

        // --- group 3 : misc ---
        float FilmGrainIntensity;
        float FilmGrainTime;
        float SceneTexelSizeX; // 1/width
        float SceneTexelSizeY; // 1/height
        
        // --- group 4 : Depth of Field (Gaussian) ---
        float FocalDistance; // ピント距離 (view空間, m)
        float FocalRegion; // シャープ帯の幅 (m)
        float NearTransitionRange; // 手前トランジション幅 (m)
        float FarTransitionRange; // 奥トランジション幅 (m)

        float MaxBlurSize; // CoC=1 のブラー半径 (halfres texel)
        float NearBlurScale; // 手前ボケ倍率
        float FarBlurScale; // 奥ボケ倍率
        float DofPad;

        uint Flags;
        float _pp_pad0;
        float _pp_pad1;
        float _pp_pad2;
    } PostProcess;
};

// -------------------------------------------------------------
//  b5 : DirectionalShadowData (CSM)
//  C++ 側 DIRECTIONAL_SHADOW_CONSTANT (ShadowRendering.h) と 1:1 ミラー必須。
//  カスケード行列は転置済み (mul(v, M) 規約)。
// -------------------------------------------------------------
cbuffer DirectionalShadowData : register(b5)
{
    float4x4 WorldToShadowCascade[4]; // ワールド -> シャドウクリップ
    float4 CascadeSplits; // 各カスケードのビュー深度遠端 (未使用は 1e9)
    float4 CascadeNormalOffset; // 受光側法線オフセット [m]
    float4 CascadeDepthBias; // 受光側深度バイアス (NDC)
    float4 DirectionalShadowParams; // x=NumCascades, y=ShadowDistance, z=FadeStart, w=1/解像度

    // ---- Distance Field Shadows ----
    float4 DFShadowParams0; // x=NumDFObjects, y=DFShadowDistance [m], z=tan(LightSourceAngle/2), w=TraceDistance [m]
    float4 DFShadowParams1; // x=ディレクショナル DF 有効 (0/1), y=レイ方向オフセット [m] (ShadowBias 由来), z=法線オフセット [m] (ShadowSlopeBias 由来), w=未使用
};

// -------------------------------------------------------------
//  b6 : LumenSceneParameters (Lumen Surface Cache / Final Gather /
//  Reflections / Radiance Cache)
//  C++ 側 LUMEN_CONSTANT (LumenScene.h) と 1:1 ミラー必須。
//  デファードライティング / トランスルーセンシーパスが参照する。
// -------------------------------------------------------------
cbuffer LumenSceneParameters : register(b6)
{
    uint NumLumenObjects; // 有効 Lumen オブジェクト数
    uint bLumenScreenGI; // 1 = ピクセル毎コーントレース経路 (GatherMode==1)
    uint LumenNumScreenCones; // 半球あたりのコーン数 (1..8, ピクセル毎経路)
    uint LumenDebugMode; // 0=off 1=GIのみ 2=スカイ可視率 3=GI(アルベド乗算)

    float LumenGIIntensity; // 拡散 GI の強度スケール
    float LumenMaxTraceDistance; // トレース最大距離 [m]
    float LumenConeTanAngle; // コーン半角の tan (C++ がコーン数から導出)
    float LumenSkyOcclusionStrength; // IBL をスカイ可視率で減衰する率 (0..1)

    float LumenSurfaceBias; // レイ開始の法線方向オフセット [m]
    uint LumenGatherMode; // 0=off 1=ピクセル毎トレース 2=Screen Probe Gather (t28)
    uint bLumenReflections; // 1 = 反射テクスチャ (t29) を合成
    float LumenReflectionMaxRoughness; // これ以上のラフネスは IBL のみ

    float LumenReflectionIntensity; // 反射合成の強度
    uint bLumenTranslucencyGI; // 1 = 半透明パスで Radiance Cache を採光
    float LumenTranslucencyGIIntensity; // 半透明 GI の強度
    float LumenPadA;

    // ---- Radiance Cache (カメラ周囲ワールドプローブ, t30-t32) ----
    // トロイダルアドレッシング: probeIndex = wrap(floor(worldPos / spacing))
    float4 LumenRadianceCacheParams0; // xyz = ボリューム最小コーナー [m], w = プローブ間隔 [m]
    float4 LumenRadianceCacheParams1; // x = プローブ数/軸, y = 有効 (0/1), z = 1/間隔, w = 予約
};

#endif
