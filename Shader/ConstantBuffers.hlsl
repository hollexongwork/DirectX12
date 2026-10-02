#ifndef CONSTANT_BUFFERS_HLSL
#define CONSTANT_BUFFERS_HLSL

// =============================================================
//  定数バッファ
//  C++ 側 (RenderManager.h) の CONSTANT_TYPE と 1:1 ミラー必須:
//    b0 = VIEW / b1 = PRIMITIVE / b2 = MATERIAL /
//    b3 = FORWARD_LIGHT / b4 = POST_PROCESS / b5 = SHADOW /
//    b6 = LUMEN / b7 = FOG
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
//  b0 : View (FViewUniformShaderParameters 相当, 448 B)
//  ビュー行列群 + カメラ + 代表ディレクショナルライト + Temporal AA / TAAU。
//  太陽ライトは View ユニフォームに常駐する。
//  Projection / InvViewProjection は TAA ジッタ込み (UE ViewToClip / ClipToTranslatedWorld)。
//  シャドウ / Lumen カード等のビューはゼロ初期化の定数を使うので、
//  テンポラル系フィールドとミップバイアスは 0 になる。
// -------------------------------------------------------------
cbuffer ViewConstantBuffer : register(b0)
{
    float4x4 View;
    float4x4 Projection; // 64  ジッタ込み
    float4x4 InvViewProjection; // 128 ジッタ込み (深度 + UV からのワールド復元)
    float4 WorldCameraOrigin; // xyz = カメラワールド位置 [m]
    float4 NearFar; // x=Near, y=Far
    // DirectionalLightDirection.xyz = 受光面からライトへ向かう方向 (発光方向の逆)
    // DirectionalLightColor.rgb     = 線形色 x 強度 (lux)。ライト不在時は 0 (無光)
    float4 DirectionalLightDirection;
    float4 DirectionalLightColor;
    // ---- Temporal AA / TAAU ----
    float4x4 PrevViewProjection; // 256 前フレーム (ジッタ込み)
    float4x4 ClipToPrevClip; // 320 NoAA
    float4 TemporalAAJitter; // 384 xy cur, zw prev (NDC)
    float4 TemporalAAParams; // 400 x = SampleIndex, y = SampleCount, zw = ジッタ (レンダー px)
    float4 ViewSizeAndInvSize; // 416 (R.x, R.y, 1/R.x, 1/R.y)
    float MaterialTextureMipBias; // 432 マテリアルテクスチャの SampleBias (TemporalUpscale 時のみ非 0)
    float MaterialTextureDerivativeMultiply; // 436 = 2^MipBias (予約)
    uint StateFrameIndexMod8; // 440 TAA 有効時 FrameIndex & 7, それ以外 0
    uint StateFrameIndex; // 444
};

// -------------------------------------------------------------
//  b1 : Primitive (FPrimitiveUniformShaderParameters 相当, 128 B)
//  per-draw のローカル→ワールド変換。
//  PreviousLocalToWorld は前フレームに描いた変換 (ベロシティパス用。
//  それ以外のパスでは LocalToWorld と同値 / 単位行列)
// -------------------------------------------------------------
cbuffer PrimitiveConstantBuffer : register(b1)
{
    float4x4 LocalToWorld;
    float4x4 PreviousLocalToWorld; // 64 前フレームの LocalToWorld (UE PreviousLocalToWorld)
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
//  ---- Substrate Slab BSDF ----
//    bUseSubstrate = true のときレガシー Metallic/Specular ワーク
//    フローの代わりに Slab (DiffuseAlbedo / F0 / F90 / SSS) で
//    シェーディングする。MFP は TransmittanceColor と固定参照厚
//    SUBSTRATE_TRANSMITTANCE_REFERENCE_CM (1cm) から
//    TransmittanceToMeanFreePath で導出し、Thickness は SSS 評価厚
//    として濃度をスケールする (Constant.hlsl 参照)。
//    Thickness は cm 単位オーサリング。
//
//  ---- Refraction ----
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

        // ---- Substrate Slab BSDF ----
        float4 SubstrateDiffuseAlbedo; // rgb (w 未使用)
        float4 SubstrateF0; // rgb (w 未使用)
        float4 SubstrateF90; // rgb (w 未使用)
        float4 SubstrateTransmittanceColor; // rgb = 透過色 (参照厚 1cm あたりの透過率) / w = 予約 (未使用)
        float4 SubstrateFuzzColor; // rgb = ファズ色 / w = FuzzAmount

        float SubstrateAnisotropy; // [-1,1] (評価は等方近似)
        float SubstrateSSSPhaseAnisotropy; // HG の g [-1,1]
        float SubstrateThickness; // スラブ厚 [cm]
        uint SubstrateSSSType; // SUBSTRATE_SSS_TYPE_*

        float SubstrateSecondRoughness;
        float SubstrateSecondRoughnessWeight;
        float SubstrateFuzzRoughness;
        bool SubstrateIsThin; // Thin Surface

        bool bUseSubstrate; // Slab ワークフロー有効
        uint RefractionMethod; // REFRACTION_METHOD_*
        float2 RefractionData; // IOR / 法線強度 / 2D オフセット

        float RefractionDepthBias; // [m]
        // Index Of Refraction From F0 :
        // TRUE のとき IOR を手入力値でなく Slab F0 から導出する
        // (DielectricF0ToIor(F0RGBToF0(F0))。Substrate + IOR 方式のみ)
        bool bRefractionUseF0;
        float2 _padSubstrate;
    } Material;
};

// -------------------------------------------------------------
//  b3 : ForwardLightData (FForwardLightData 相当, 112 B)
//  ライトの数 + タイルドライトカリング (ライトグリッド) のパラメータ +
//  フォワードシェーディングが使う「選択されたディレクショナルライト」。
//  ライト本体は StructuredBuffer<FLocalLightData> (t13, ForwardLightBuffer):
//    [0, NumLocalLights)                              : 視界内のローカルライト (Point / Spot / Rect)
//    [NumLocalLights, NumLocalLights + NumDirectionalLights) : ディレクショナルライト
//  グリッド本体は NumCulledLightsGrid (t19) + CulledLightDataGrid (t20)。
//  C++ 側 FORWARD_LIGHT_CONSTANT (RenderManager.h) と 1:1 ミラー必須。
//  受光側ヘルパは LightGridCommon.hlsl (GetLocalLightData / GetDirectionalLightData)。
//  UE と同じく ForwardLightData.<フィールド> で読む (b0 の DirectionalLight* と名前を分けるため)。
// -------------------------------------------------------------
cbuffer ForwardLightDataBuffer : register(b3)
{
    struct FForwardLightData
    {
        uint NumLocalLights; // 0   ローカルライト数
        uint NumDirectionalLights; // 4   ディレクショナルライト数
        uint NumGridCells; // 8   グリッド総セル数 (X*Y*Z)。現状どのシェーダーも読まない (将来用)
        uint HasDirectionalLight; // 12  選択されたフォワードディレクショナルライトがあるか

        uint CulledGridSizeX; // 16  画面タイル数 X (= ceil(W / LightGridPixelSize))
        uint CulledGridSizeY; // 20  画面タイル数 Y
        uint CulledGridSizeZ; // 24  Z スライス数 (LIGHT_GRID_SIZE_Z)
        uint LightGridPixelSizeShift; // 28  log2(LightGridPixelSize)

        float3 LightGridZParams; // 32  (B, O, S): Slice = log2(Depth*B + O) * S
        uint MaxCulledLightsPerCell; // 44  セルあたり保持するライト数上限

        uint LightGridDebugMode; // 48  0=off 1=複雑度ヒートマップ 2=Zスライス
        uint bUseLightGrid; // 52  0 = 全灯ループ (フォールバック)
        uint DirectionalLightBufferIndex; // 56  選択されたディレクショナルライトの t13 内の添字 (CSM / DF シャドウを持つライト)
        uint DirectionalLightFlags; // 60  選択されたディレクショナルライトの LIGHT_FLAG_*

        // ---- 選択されたフォワードディレクショナルライト (半透明 / Volumetric Fog が使う 1 灯) ----
        float3 DirectionalLightColor; // 64  線形色 x 強度 (lux)
        float DirectionalLightVolumetricScatteringIntensity; // 76
        float3 DirectionalLightDirection; // 80  受光点 -> ライト方向
        float DirectionalLightSourceRadius; // 92  sin(見かけの半角)
        float DirectionalLightSoftSourceRadius; // 96
        float DirectionalLightSpecularScale; // 100
        float DirectionalLightDiffuseScale; // 104
        float Pad; // 108
    } ForwardLightData;
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
        // --- レンダラ専有 (永続化しない。C++ PP_SETTINGS の同名フィールド) ---
        float UpscaleUnsharpAmount; // 一次空間アップスケール mode 5 のアンシャープ量 (r.Upscale.Softness x (1 - 面積比))
        uint VisualizeMode;         // Temporal AA デバッグ表示 (ETemporalAADebugView)
        float VisualizeScale;       // デバッグ表示の増幅 (FTemporalAADebugSettings::VisualizeScale)
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
    uint LumenDebugMode; // 0=off 1=GIのみ 2=スカイ可視率 3=GI(アルベド乗算) 4=Short Range AO

    float LumenGIIntensity; // 拡散 GI の強度スケール
    float LumenMaxTraceDistance; // トレース最大距離 [m]
    float LumenConeTanAngle; // コーン半角の tan (C++ がコーン数から導出)
    float LumenSkyOcclusionStrength; // IBL をスカイ可視率で減衰する率 (0..1)

    float LumenSurfaceBias; // レイ開始の法線方向オフセット [m]
    uint LumenGatherMode; // 0=off 1=ピクセル毎トレース 2=Screen Probe Gather (t28)
    uint bLumenReflections; // 1 = 反射テクスチャ (t29) を合成
    float LumenReflectionMaxRoughness; // これ以上のラフネスは IBL のみ (現状どのシェーダーも読まない。反射パスは PassReflectionParams.x を使用。将来用)

    float LumenReflectionIntensity; // 反射合成の強度 (現状どのシェーダーも読まない。反射パスは PassReflectionParams.z を使用。将来用)
    uint bLumenTranslucencyGI; // 1 = 半透明パスで Radiance Cache を採光
    float LumenTranslucencyGIIntensity; // 半透明 GI の強度
    float LumenPadA;

    // ---- Radiance Cache (カメラ周囲ワールドプローブ, t30-t32) ----
    // トロイダルアドレッシング: probeIndex = wrap(floor(worldPos / spacing))
    float4 LumenRadianceCacheParams0; // xyz = ボリューム最小コーナー [m], w = プローブ間隔 [m]
    float4 LumenRadianceCacheParams1; // x = プローブ数/軸, y = 有効 (0/1), z = 1/間隔, w = 予約
};

// -------------------------------------------------------------
//  b7 : FogUniformParameters (FFogUniformParameters / FogStruct 相当)
//  Exponential Height Fog + Volumetric Fog のビュー毎パラメータ。
//  C++ 側 FOG_CONSTANT (FogRendering.h) と 1:1 ミラー必須 (192 bytes)。
//  FSceneRenderer::RenderBasePass 先頭 (InitFogConstants) で毎フレーム
//  解決され、フォグパス (HeightFogPS) / トランスルーセンシー
//  (TranslucentPS) が HeightFogCommon.hlsl 経由で参照する。
//  ※ 本エンジンは Y-up / メートル単位。UE の Z (高さ) は全て Y。
//    密度 / 高さ減衰は [1/m] に換算済み (FExponentialHeightFogSceneInfo)。
// -------------------------------------------------------------
cbuffer FogUniformParameters : register(b7)
{
    // x = FogDensity0 * exp2(-HeightFalloff0 * (ObserverY - Height0)) (観測者高さで畳み込んだ密度)
    // y = HeightFalloff0 [1/m], z = MaxWorldObserverHeight [m], w = StartDistance [m]
    float4 ExponentialFogParameters;
    // x = FogDensity1 * exp2(-HeightFalloff1 * (ObserverY - Height1)) (第 2 層)
    // y = HeightFalloff1 [1/m], z = FogDensity1 [1/m], w = Height1 [m]
    float4 ExponentialFogParameters2;
    // rgb = FogInscatteringLuminance (キューブマップ使用時は InscatteringTextureTint)
    // w   = 1 - FogMaxOpacity (= 最小透過率 MinFogOpacity)
    float4 ExponentialFogColorParameter;
    // x = FogDensity0 [1/m], y = Height0 [m], z = キューブマップ使用 (0/1), w = FogCutoffDistance [m] (0 = 無効)
    float4 ExponentialFogParameters3;
    // xyz = 受光点 -> ディレクショナルライト方向 (正規化)
    // w   = DirectionalInscatteringStartDistance [m] (負 = Directional Inscattering 無効)
    float4 InscatteringLightDirection;
    // rgb = DirectionalInscatteringLuminance, w = DirectionalInscatteringExponent
    float4 DirectionalInscatteringColor;
    // x = sin(InscatteringColorCubemapAngle), y = cos(同) (Y 軸まわり回転), zw = 未使用
    float4 SinCosInscatteringColorCubemapRotation;
    // x = 1 / (FullyDirectional - NonDirectional 距離), y = -NonDirectional * x,
    // z = 非指向性色に使う最終ミップ (NumMips - 1), w = 未使用
    float4 FogInscatteringTextureParameters;
    // x = EndDistance [m] (0 = 無効。UE 5.4 の EndDistance 相当: 積分レイ長のクランプ), yzw = 予約
    float4 ExponentialFogParameters4;
    // ---- Volumetric Fog (VolumetricFog.h) ----
    // xyz = froxel Z 分布 (B, O, S): Slice = log2(ViewZ * B + O) * S
    // w   = ApplyVolumetricFog (0/1)
    float4 VolumetricFogGridZParams;
    // x = VolumetricFogMaxDistance [m] (無効時 0), y = 1 / GridSizeZ,
    // zw = SVPosition.xy -> ボリューム UV (1 / (GridSize.xy * GridPixelSize))
    float4 VolumetricFogParameters;
    // xyz = ビュー前方ベクトル (正規化。解析フォグの除外距離計算用), w = 未使用
    float4 VolumetricFogViewForward;
};

#endif
