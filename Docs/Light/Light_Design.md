# ライトの UE5 フレームワーク移行 設計書

**対象ブランチ:** `Light` (基点 `72f8f63`)。
**スコープ:** UE5 対応。UE5 のライト (`ALight` 系アクター / `ULightComponent` 系 / `FLightSceneProxy` 系 / `FLightSceneInfo` / `FScene` のライト登録 / 可視判定・ソート・ライトグリッド / ライトのシェーディング関数) を、本エンジンが許す範囲で 1:1 に移植する。
**前提:** 単位はメートル、Y アップ、前方 +Z、左手系。回転はラジアンの `XMFLOAT3` (pitch, yaw, roll)。

**タグ:**

| タグ | 意味 |
|---|---|
| **[H] / [M] / [L]** | UE と一致している確度 (UE 4.26〜5.x のソースの記憶に基づく) |
| **[PORT]** | UE からの意図的な乖離。理由を併記する |
| **[FIX]** | 既存の不具合の修正 |

---

## 0. 決定事項

1. **軸の対応 [PORT]。** UE のライトのローカル軸 (X = 前方, Y = 右, Z = 上) を本エンジンの (Z = 前方, X = 右, Y = 上) に読み替える。
   - `FLightSceneProxy::GetDirection()` は発光方向 (コンポーネント +Z)。
   - `FLightRenderParameters::Direction` は UE と同じく `-GetDirection()` (受光点からライトへ向かう方向)。**従来は発光方向そのものだった。**
   - `FLightRenderParameters::Tangent` は UE と同じくライトの上方向 (UE ローカル Z = 本エンジン +Y)。レクトライトの高さ軸、チューブ (SourceLength) の軸になる。**従来はコンポーネント +X だった。チューブの軸は +X から +Y に変わる。**
2. **単位 [PORT]。** UE の cm を m に読み替える。`ComputeLightBrightness` の `100 * 100` (cm² → m²) は掛けない。Unitless の係数は `16 / 10000 = 1 / 625`。カプセルライトの `DistBiasSqr = 1` (cm²) は `1e-4` (m²)。距離の既定値は 1/100 にする。
3. **1 パスのクラスタードデファードを維持する [PORT]。** UE はライト 1 灯ごとにデファードライトパスを描く。本エンジンは `DeferredPS` 1 パスの中でライトグリッドのセルを巡回する (UE の `ClusteredDeferredShadingPixelShader` + インラインのシャドウ参照に相当)。ライト 1 灯の評価は UE と同じ `GetDynamicLighting` を呼ぶ。
4. **サーフェスの BxDF カーネルとマテリアル入力は変えない [PORT]。** 既存の `GGX_NDF` / `SmithGeometry` / `SchlickFresnel` / `kD = (1 - F)(1 - Metallic)` と `F0 = lerp(0.04, BaseColor, Metallic)`、および Substrate Slab の評価式は、マテリアル / シェーディングモデルのフレームワークに属するので対象外。ライト側の枠組み (`FAreaLightIntegrateContext` / `SphereMaxNoH` / `EnergyNormalization` / LTC) は UE と同じにする。光源形状を持たないライトの結果は移行前と一致する。
5. **ライトの更新経路を UE と同じ 3 本にする。**
   - `MarkRenderStateDirty` : プロキシを作り直す (フレーム末尾の `RemoveLight` → `AddLight`)。
   - `MarkRenderTransformDirty` → `SendRenderTransform` → `FScene::UpdateLightTransform`。
   - `UpdateColorAndBrightness` → `FScene::UpdateLightColorAndBrightness` (プロキシを作り直さない軽量経路)。
6. **シーンに居るライトは描画されるライトだけ。** UE の `CreateRenderState_Concurrent` と同じく `bAffectsWorld && IsVisible() && Intensity > 0` のライトだけを `FScene::AddLight` する。レンダラ側の `AffectsWorld()` 判定は無くなる。
7. **ライトの可視判定を行う。** `ComputeLightVisibility` がローカルライトをビューフラスタムと描画距離 (`MaxDrawDistance`、最小スクリーン半径) で判定し、`FVisibleLightViewInfo` に書く。ライトバッファには視界内のライトだけを積む。
8. **`GatherAndSortLights` でソートする。** UE の `FSortedLightSceneInfo::SortKey` と同じビット順。ライトバッファ / ローカルシャドウパラメータの添字はソート後の順になる。
9. **ディレクショナルライトは複数灯を照らす。** デファードは全ディレクショナルライトを評価する。フォワード (半透明) と Volumetric Fog は UE と同じく「選択された 1 灯」(`ForwardShadingPriority` → 輝度の順) を使う。**CSM / DF シャドウを持つのは選択された 1 灯だけ [PORT]** (シャドウ描画系が CSM を 1 組しか持たない)。
10. **Exponential Height Fog の太陽は `FScene::AtmosphereLights[0]`。** `bAtmosphereSunLight` / `AtmosphereSunLightIndex` で選ぶ (UE)。
11. **Lumen の直接光は専用のライトリストを使う。** UE の `FLumenGatheredLight` と同じく、視錐台外でも描画距離内なら含め、`bAffectGlobalIllumination` が真のライトだけを、色に `IndirectLightingIntensity` を掛けて渡す。
12. **レクトライトは UE と同じ式にする。** 拡散は多角形の放射照度 (`RectIrradianceLambert`)、スペキュラは LTC (`RectGGXApproxLTC`)。色は UE と同じく `0.5 * SourceWidth * SourceHeight` で割る [M]。**同じ Intensity のレクトライトは移行前の約 2 倍明るくなる。**
13. **LTC テーブルは再フィットして持つ [PORT]。** UE の `LTC.h` と同じ手順 (Heitz 2016 の公開フィット、64x64、`UV = (Roughness, sqrt(1 - NoV))`) で生成した表を `LTC.cpp` に持つ。生成ツールは `Tools/LTCFit`。値は UE の表と数値誤差の範囲で一致する。
14. **`bCastVolumetricShadow` の既定は UE と同じ** (ディレクショナル = true、ローカル = false)。**既存シーンのローカルライトは Volumetric Fog 内で影を落とさなくなる** (チェックを入れれば従来どおり)。
15. **ライト色の保持は線形の float [PORT]。** UE は sRGB の `FColor` (8 bit) で持つ。API は UE と同じく線形で受け渡しする。Details の色編集だけ UE と同じく sRGB で行う。INI の値は従来どおり線形。
16. **INI は後方互換。** 既存キーは変えない。新しいプロパティは UE のプロパティ名でキーを足す。キーが無ければ既定値。

---

## 1. スコープ

### 1.1 移植する機能

| # | UE5 の機能 | 移植内容 |
|---|---|---|
| 1 | `ALight` / `ADirectionalLight` / `APointLight` / `ASpotLight` / `ARectLight` | API を UE に合わせる。`SetEnabled` は可視性 (`SetVisibility`) を切り替える [H]。`ToggleEnabled` / `SetCastShadows` / `SetAffectTranslucentLighting` / `APointLight::SetRadius` / `SetLightFalloffExponent` / `ASpotLight::SetInnerConeAngle` / `SetOuterConeAngle` を追加 |
| 2 | `ULightComponentBase` | `Intensity` / `LightColor` / `bAffectsWorld` / `CastShadows` / `CastDynamicShadows` / `bAffectTranslucentLighting` / `bCastVolumetricShadow` / `bAffectGlobalIllumination` / `IndirectLightingIntensity` / `VolumetricScatteringIntensity` |
| 3 | `ULightComponent` | `Temperature` / `bUseTemperature` / `MaxDrawDistance` / `MaxDistanceFadeRange` / `SpecularScale` / `DiffuseScale` / `ShadowBias` / `ShadowSlopeBias` / `ContactShadowLength` / `ContactShadowLengthInWS` / `ContactShadowCastingIntensity` / `ContactShadowNonCastingIntensity` / `bUseRayTracedDistanceFieldShadows`、可視性、`CreateSceneProxy` / `GetLightType` / `GetBoundingSphere` / `GetDirection` / `GetLightPosition` / `ComputeLightBrightness` / `GetColoredLightBrightness` / `UpdateColorAndBrightness` |
| 4 | `UDirectionalLightComponent` | `DynamicShadowDistanceMovableLight` / `DynamicShadowCascades` / `CascadeDistributionExponent` / `ShadowDistanceFadeoutFraction` / `DistanceFieldShadowDistance` / `TraceDistance` / `LightSourceAngle` / `LightSourceSoftAngle` / `ForwardShadingPriority` / `bAtmosphereSunLight` / `AtmosphereSunLightIndex` |
| 5 | `ULocalLightComponent` | `AttenuationRadius` / `IntensityUnits` / `GetUnitsConversionFactor` |
| 6 | `UPointLightComponent` | `bUseInverseSquaredFalloff` / `LightFalloffExponent` / `SourceRadius` / `SoftSourceRadius` / `SourceLength`、`ComputeLightBrightness` / `SetLightBrightness` |
| 7 | `USpotLightComponent` | `InnerConeAngle` / `OuterConeAngle`、`GetHalfConeAngle` (UE のクランプ: 内 0..89 度、外 内+0.001 rad .. 89 度+0.001 rad) [H] |
| 8 | `URectLightComponent` | `SourceWidth` / `SourceHeight` / `BarnDoorAngle` / `BarnDoorLength` |
| 9 | `FLightSceneProxy` 階層 | `FLightSceneProxy` → `FDirectionalLightSceneProxy`、`FLocalLightSceneProxy` → `FPointLightSceneProxy` → `FSpotLightSceneProxy`、`FLocalLightSceneProxy` → `FRectLightSceneProxy`。仮想の `GetLightShaderParameters(FLightRenderParameters&)` / `GetBoundingSphere` / `AffectsBounds` / `GetRadius` / `IsInverseSquared` / `IsRectLight` / `IsLocalLight` ほか |
| 10 | `FLightRenderParameters` | UE と同じフィールド (`WorldPosition` / `InvRadius` / `Color` / `FalloffExponent` / `Direction` / `SpecularScale` / `DiffuseScale` / `Tangent` / `SourceRadius` / `SpotAngles` / `SoftSourceRadius` / `SourceLength` / `RectLightBarnCosAngle` / `RectLightBarnLength` / `bAffectsTranslucentLighting`) |
| 11 | `FLightSceneInfo` / `FLightSceneInfoCompact` | `Id` / `bVisible` / `ShouldRenderLight` / `ShouldRenderLightViewIndependent` / `GetBoundingSphere`、コンパクト版 (境界球 / 色 / 種別 / フラグ) |
| 12 | `FScene` のライト登録 | `Lights` (Id で引くスパース配列) / `DirectionalLights` / `SimpleDirectionalLight` / `AtmosphereLights[2]`、`AddLight` / `RemoveLight` / `UpdateLightTransform` / `UpdateLightColorAndBrightness` |
| 13 | ライトの可視判定 | `FVisibleLightViewInfo` (`bInViewFrustum` / `bInDrawRange`)、`ComputeLightVisibility` (フラスタム、`r.MinScreenRadiusForLights = 0.03`、`MaxDrawDistance`) [M] |
| 14 | ライトのフェード | `GetLightFadeFactor` (サイズフェード x 距離フェード) をローカルライトの色に掛ける [M] |
| 15 | ソート | `FSortedLightSceneInfo` / `FSortedLightSetSceneInfo` / `GatherAndSortLights` [H] |
| 16 | ライトグリッド | `ComputeLightGrid` が `FForwardLightData` (b3) と `ForwardLightBuffer` (t13) を詰める。Z 分布の遠端は最遠のライトまで (`FurthestLight`) [M]。フォワードディレクショナルライトの選択 (`ForwardShadingPriority` → 輝度) [M] |
| 17 | シェーダのライトデータ | `FLocalLightData` / `FDirectionalLightData` / `FDeferredLightData`、`GetLocalLightData` / `GetDirectionalLightData` / `ConvertToDeferredLight`。逆二乗は `FalloffExponent == 0` で表す (UE) |
| 18 | 減衰 | `GetLocalLightAttenuation` (`RadialAttenuation` / 半径の窓 / `SpotAttenuation` / レクトの背面) [H] |
| 19 | カプセルライト | `FCapsuleLight` / `GetCapsule` / `LineIrradiance` / `SphereHorizonCosWrap` / `ClosestPointLineToRay` / `CreateCapsuleIntegrateContext` / `IntegrateLight` [H] |
| 20 | 面光源のスペキュラ | `SphereMaxNoH` / `New_a2` / `EnergyNormalization` [H] |
| 21 | レクトライト | `FRect` / `GetRect` (バーンドアの可視矩形) [M] / `RectIrradianceLambert` [H] / `RectGGXApproxLTC` [H] / `IntegrateLight` |
| 22 | ライト 1 灯の評価 | `GetDynamicLighting` / `AccumulateDynamicLighting` (`SpecularScale` / `DiffuseScale` 込み) |
| 23 | コンタクトシャドウ | `ShadowRayCast` (8 ステップのスクリーンスペースレイ) を `GetShadowTerms` に入れる。デファードのみ [M] |
| 24 | Lumen の直接光 | 全ディレクショナルライトとローカルライトを `GetLocalLightAttenuation` + `IntegrateLight` で評価 (UE `GetIrradianceForLight`) |
| 25 | Volumetric Fog のライト | `GetLocalLightAttenuation` + `IntegrateLight`、`bCastVolumetricShadow` |
| 26 | Details / INI | 追加した全プロパティを Details に出し、INI に保存する |

### 1.2 対象外 (理由つき)

| 機能 | 理由 |
|---|---|
| Mobility (Static / Stationary) と Lightmass (`CastStaticShadows`、ライトマップ、シャドウマップチャンネル) | 本エンジンは全ライト Movable。静的ライティングが無い |
| ライトファンクション (`LightFunctionMaterial` ほか) | マテリアルグラフが無い |
| IES プロファイル | IES の読み込みとアトラスが無い |
| ライティングチャンネル | G-Buffer にチャンネルを書く仕組みが必要 (プリミティブ / ベースパス側の作業) |
| レクトライトの `SourceTexture` | レクトライトアトラス (プレフィルタ済みミップ) が無い |
| シャドウ系の未実装機能 (`ShadowResolutionScale` / `ShadowSharpen` / `CascadeTransitionFraction` / Far Cascade / Inset Shadow / Modulated Shadow / `CastTranslucentShadows` / VSM / レイトレースシャドウ / `RayStartOffsetDepthScale`) | シャドウ描画系 (`ShadowRendering`) の範囲。ライト側のプロパティだけ足しても効かない |
| ライトシャフト (`bEnableLightShaftBloom` / `bEnableLightShaftOcclusion`) | ポストプロセスの機能 |
| SkyAtmosphere / 雲との相互作用 (`AtmosphereSunDiskColorScale` / Cloud Shadow ほか) | SkyAtmosphere / Volumetric Cloud が無い |
| `InverseExposureBlend` | プリエクスポージャが無い |
| `bTransmission` / `bCastDeepShadow` / `SamplesPerPixel` / `bAffectReflection` / `CastRaytracedShadow` | サブサーフェス透過シャドウ、ヘア、パストレーサ、レイトレース反射が無い |
| `USkyLightComponent` | IBL は `IBLBaker` が担当している。別の作業 |
| サーフェスの BxDF (`ShadingModels.ush` の `DefaultLitBxDF` の D / Vis / F、`Specular` 入力) | 決定事項 4 |

---

## 2. アーキテクチャ

### 2.1 ファイル

| ファイル | 内容 (UE の対応) |
|---|---|
| `Light.h/.cpp`、`DirectionalLight.*`、`PointLight.*`、`SpotLight.*`、`RectLight.*` | ライトアクター |
| `LightComponentBase.h/.cpp` | `ULightComponentBase` |
| `LightComponent.h/.cpp` | `ULightComponent` |
| `DirectionalLightComponent.h/.cpp`、`DirectionalLightSceneProxy.h` | `UDirectionalLightComponent`、`FDirectionalLightSceneProxy` [PORT: UE は .cpp 内に定義。シャドウ描画系が CSM パラメータを直接読むのでヘッダに出す] |
| `LocalLightComponent.h/.cpp`、`LocalLightSceneProxy.h/.cpp` | `ULocalLightComponent` (`ELightUnits`)、`FLocalLightSceneProxy` |
| `PointLightComponent.h/.cpp`、`PointLightSceneProxy.h` | `UPointLightComponent`、`FPointLightSceneProxy` |
| `SpotLightComponent.h/.cpp`、`SpotLightSceneProxy.h` | `USpotLightComponent`、`FSpotLightSceneProxy` |
| `RectLightComponent.h/.cpp`、`RectLightSceneProxy.h` | `URectLightComponent`、`FRectLightSceneProxy` |
| `LightSceneProxy.h/.cpp` | `ELightComponentType`、`FLightRenderParameters`、`FLightSceneProxy` |
| `LightSceneInfo.h/.cpp` | `FLightSceneInfo`、`FLightSceneInfoCompact` |
| `LightRendering.h/.cpp` | `FSortedLightSceneInfo`、`FSortedLightSetSceneInfo`、`GetLightFadeFactor`、`FSceneRenderer::GatherAndSortLights`、`FSceneRenderer::ComputeLightVisibility` |
| `LightGridInjection.h/.cpp` | `FForwardLocalLightData`、`FSceneRenderer::ComputeLightGrid`、`FLightGridInjection` (コンピュート 2 パス) |
| `SystemTextures.h/.cpp`、`LTC.h/.cpp` | `FSystemTextures` (LTCMat / LTCAmp)、LTC テーブル |
| `Tools/LTCFit/LTCFit.cpp` | LTC テーブルの生成ツール (ビルド対象外) |
| `Scene.h/.cpp` | `FScene` のライト登録 |

シェーダ (`Shader/`。ヘッダは全てビルド対象外):

| ファイル | 内容 (UE の対応) | レジスタ |
|---|---|---|
| `LightData.hlsl` | `LIGHT_TYPE_*`、`FRectLightData`、`FDeferredLightData`、`FLocalLightData`、`FDirectionalLightData`、`ConvertToDeferredLight` (LightData.ush) | 無し |
| `DynamicLightingCommon.hlsl` | `RadialAttenuationMask` / `RadialAttenuation` / `SpotAttenuationMask` / `SpotAttenuation` | 無し |
| `BRDF.hlsl` | `Pow2..5`、`BxDFContext`、`Init`、`SphereMaxNoH`、`D_GGX` | 無し |
| `CapsuleLight.hlsl` | `FCapsuleLight`、`LineIrradiance`、`SphereHorizonCosWrap`、`ClosestPointLineToRay`、`IntegrateLight` | 無し |
| `RectLight.hlsl` | `FRect`、`GetRect`、`PolygonIrradiance`、`RectIrradianceLambert`、`IntegrateLight` | 無し |
| `AreaLightCommon.hlsl` | `FAreaLight`、`FAreaLightIntegrateContext` | 無し |
| `DeferredLightingCommon.hlsl` | `GetLocalLightAttenuation`、`GetCapsule`、`GetRect`。`NON_DIRECTIONAL_DIRECT_LIGHTING == 0` のときだけ `FShadowTerms`、`GetShadowTerms`、`ShadowRayCast`、`AccumulateDynamicLighting`、`GetDynamicLighting` | 前半は無し |
| `DeferredShadingCommon.hlsl` | `FGBufferData` と構築 | 無し |
| `ShadingModels.hlsl` | `FDirectLighting`、`New_a2`、`EnergyNormalization`、`SpecularGGX`、`DefaultLitBxDF`、`IntegrateBxDF` | グラフィックス |
| `RectLightLTC.hlsl` | `RectGGXApproxLTC` [PORT: UE は RectLight.ush 内。LTC テクスチャ (t37/t38) を読むのでレジスタ依存のファイルへ分離] | グラフィックス |
| `CapsuleLightIntegrate.hlsl`、`RectLightIntegrate.hlsl` | `CreateCapsuleIntegrateContext`、`CreateRectIntegrateContext`、`IntegrateBxDF` | グラフィックス |
| `LightGridCommon.hlsl` | `GetLocalLightData`、`GetDirectionalLightData`、セル参照 | グラフィックス |
| `ForwardLightingCommon.hlsl` | `GetForwardDirectLighting` / `GetForwardDirectLightingSubstrate` (ForwardLightingCommon.ush + ClusteredDeferredShadingPixelShader.usf)。DeferredPS と TranslucentPS が共有 | グラフィックス |
| `LumenSceneDirectLighting_CS.hlsl` | `GetIrradianceForLight` (Lumen 用ライトバッファ t3 を全灯巡回。ディレクショナルも同じバッファ) | Lumen |
| `VolumetricFogCommon.hlsl` | `ComputeLocalLightVolumetricScattering` (`GetLocalLightAttenuation` + `IntegrateLight`、`bCastVolumetricShadow`)。ディレクショナルは b3 と同じ 1 灯 | Volumetric Fog |
| `LightGridInjection_CS.hlsl` | `FLocalLightData` で判定 (Direction は受光点 -> ライトなので発光方向は符号反転) | ライトグリッド |
| `SubstrateEvaluation.hlsl` | Slab の直接光を `FAreaLightIntegrateContext` で受ける。`SubstrateDeferredLighting` | グラフィックス |

コンピュート (Lumen / Volumetric Fog / ライトグリッド構築) は `#define NON_DIRECTIONAL_DIRECT_LIGHTING 1` で `DeferredLightingCommon.hlsl` を取り込み、減衰と `IntegrateLight` だけを使う (UE の VolumetricFog.usf と同じ使い方)。

### 2.2 フレームの流れ

```
ゲーム側
  セッター            -> UpdateColorAndBrightness (即時) / MarkRenderStateDirty / MarkRenderTransformDirty
  UWorld::SendAllEndOfFrameUpdates
    FScene::UpdateAllLightSceneInfos  : レンダーステートダーティ -> RemoveLight + AddLight
                                        トランスフォームダーティ -> SendRenderTransform -> UpdateLightTransform
レンダー側 (FSceneRenderer)
  RenderBasePass
    PrepareViewStateForVisibility
    ComputeViewVisibility            : フラスタム構築 + プリミティブ
    ComputeLightVisibility           : m_ViewInfo.VisibleLightInfos
    GatherLightsAndComputeLightGrid
      GatherAndSortLights            : m_SortedLightSet
      ComputeLightGrid               : ForwardLightBuffer (t13) + FForwardLightData (b3)
                                       + フォワードディレクショナルライトの選択 + Lumen 用ライトリスト
    InitFogConstants                 : 太陽 = FScene::AtmosphereLights[0]
  RenderShadowDepths                 : 選択ディレクショナルライトの CSM + ローカルシャドウ (バッファ順)
  RenderLumenScene                   : Lumen 用ライトリスト
  RenderLighting
    FLightGridInjection::Dispatch    : Injection -> Compact
    Volumetric Fog / デファード (全ディレクショナル + グリッドのローカル)
  RenderTranslucency                 : フォワードディレクショナル 1 灯 + グリッドのローカル
```

---

## 3. データレイアウト

### 3.1 `FForwardLocalLightData` (C++) / `FLocalLightData` (HLSL)。`ForwardLightBuffer` (t13) の 1 要素、128 B

| オフセット | フィールド | 内容 |
|---|---|---|
| 0 | `LightPositionAndInvRadius` | xyz = ワールド位置 [m] (ディレクショナルは 0)、w = 1 / AttenuationRadius (ディレクショナルは 0) |
| 16 | `LightColorAndFalloffExponent` | rgb = 色 x 明るさ (フェード適用済み)、w = FalloffExponent (**0 = 逆二乗**) |
| 32 | `LightDirectionAndSpecularScale` | xyz = Direction (= -発光方向)、w = SpecularScale |
| 48 | `SpotAnglesAndSourceRadiusPacked` | xy = SpotAngles (cos(Outer), 1 / (cos(Inner) - cos(Outer)))、z = SourceRadius、w = SourceLength |
| 64 | `LightTangentAndSoftSourceRadius` | xyz = Tangent (ライトの上方向)、w = SoftSourceRadius |
| 80 | `RectBarnDoorAndScales` | x = RectLightBarnCosAngle、y = RectLightBarnLength、z = DiffuseScale、w = VolumetricScatteringIntensity |
| 96 | `ContactShadowParams` | x = ContactShadowLength (負 = ワールド空間)、y = CastingIntensity、z = NonCastingIntensity、w = 予約 |
| 112 | `LightType` (uint) | `LIGHT_TYPE_*` |
| 116 | `Flags` (uint) | `LIGHT_FLAG_*` |
| 120 | `Pad` (uint x 2) | 予約 |

バッファの並びは `[0, NumLocalLights)` = 視界内のローカルライト (ソート順)、`[NumLocalLights, NumLocalLights + NumDirectionalLights)` = ディレクショナルライト。フィールド名は UE 4.26 の `FForwardLocalLightData` に合わせる。パックのビット配置は本エンジン独自 [PORT]。

`LIGHT_FLAG_*`: bit 0 = `CAST_DYNAMIC_SHADOW` (UE `ShadowedBits`)、bit 1 = `AFFECT_TRANSLUCENT_LIGHTING`、bit 2 = `CAST_VOLUMETRIC_SHADOW`。

### 3.2 `FORWARD_LIGHT_CONSTANT` (b3、`FForwardLightData`)。112 B

| オフセット | フィールド |
|---|---|
| 0 | `NumLocalLights`、`NumDirectionalLights`、`NumGridCells`、`HasDirectionalLight` |
| 16 | `CulledGridSizeX`、`CulledGridSizeY`、`CulledGridSizeZ`、`LightGridPixelSizeShift` |
| 32 | `LightGridZParams` (float3)、`MaxCulledLightsPerCell` |
| 48 | `LightGridDebugMode`、`bUseLightGrid`、`DirectionalLightBufferIndex` (選択されたライトの t13 内の添字)、`DirectionalLightFlags` (`LIGHT_FLAG_*`) |
| 64 | `DirectionalLightColor` (float3)、`DirectionalLightVolumetricScatteringIntensity` |
| 80 | `DirectionalLightDirection` (float3)、`DirectionalLightSourceRadius` |
| 96 | `DirectionalLightSoftSourceRadius`、`DirectionalLightSpecularScale`、`DirectionalLightDiffuseScale`、予約 |

`DirectionalLight*` は選択されたフォワードディレクショナルライト。`VIEW_CONSTANT` (b0) の `DirectionalLightDirection` / `DirectionalLightColor` は同じライトの値を引き続き入れる (レイアウト維持。UE の `View.DirectionalLightDirection` と同じ位置づけ)。シェーダは b3 を `ForwardLightData.<フィールド>` で読む (cbuffer 内の struct。b0 のグローバル `DirectionalLight*` と名前が衝突しないようにするため)。

Lumen の `FLumenPassParams` (b0, 528 B) は `PassNumLocalLights` を `PassNumLumenLights` (ディレクショナル + ローカル) に、`PassDirectionalLightDirection` / `PassDirectionalLightColor` を `PassReserved0` / `PassReserved1` に置き換えた (レイアウト維持)。Volumetric Fog の `FVolumetricFogParams` はレイアウトを変えず、ディレクショナルライトの値を b3 から取る (`bCastVolumetricShadow` が偽なら `NumCascades = 0`)。

### 3.3 追加の SRV

| レジスタ | 内容 |
|---|---|
| t37 | `LTCMatTexture` (64x64 RGBA16F。LTC の逆行列の 4 成分) |
| t38 | `LTCAmpTexture` (64x64 RG16F。x = 大きさ、y = フレネル項) |

サンプラは s1 (線形クランプ)。ルートシグネチャは 47 DWORD。

---

## 4. 主要な式 (UE との対応)

### 4.1 明るさ

```
ULightComponent::ComputeLightBrightness            = Intensity
UPointLightComponent  (逆二乗)  Candelas: x1 / Lumens: x 1/(4π) / EV: 1.2 * 2^Intensity / Unitless: x 1/625
USpotLightComponent   (逆二乗)  Lumens: x 1/(2π(1 - cos(HalfConeAngle)))、他は Point と同じ
URectLightComponent             Lumens: x 1/π、他は Point と同じ (常に逆二乗)
GetColoredLightBrightness       = LightColor * 明るさ [* MakeFromColorTemperature(Temperature)]
FRectLightSceneProxy            Color /= 0.5 * SourceWidth * SourceHeight
```

### 4.2 可視判定とフェード (メートル換算)

```
bInViewFrustum = Frustum.IntersectSphere(BoundingSphere)
bDrawLight     = Square(min(0.02, 0.03 / R)) * DistSq < 1  &&  (MaxDist == 0 || DistSq < MaxDist^2)
SizeFade       = saturate(6 - 6 * Square(min(0.02, 0.03 / R)) * DistSq)
DistanceFade   = MaxDist ? saturate((MaxDist - Dist) / FadeRange) : 1
```

`0.02` は UE の `0.0002` [1/cm]、`LODDistanceFactor` は 1 [PORT]。スポットライトの境界球は `ComputeBoundingSphereForCone`。

### 4.3 ライト 1 灯 (`AccumulateDynamicLighting`)

```
L = LightData.Direction; ToLight = L; LightMask = 1
if (bRadialLight) LightMask = GetLocalLightAttenuation(WorldPosition, LightData, ToLight, L)
if (LightMask > 0):
    GetShadowTerms (呼び出し側が求めたシャドウ係数 + コンタクトシャドウ)
    bRectLight ? IntegrateBxDF(GBuffer, N, V, GetRect(ToLight, LightData), Shadow)
               : IntegrateBxDF(GBuffer, N, V, GetCapsule(ToLight, LightData), Shadow, bInverseSquared)
    Specular *= SpecularScale; Diffuse *= DiffuseScale
    結果 = (Diffuse + Specular) * Color * LightMask * SurfaceShadow
```

`GetCapsule` の `DistBiasSqr` はローカルライトで `1e-4`、ディレクショナルライトで `1` [PORT: UE は定数 1。ディレクショナルは単位ベクトルに 1 を足す UE の数値をそのまま再現する]。`MinRoughness = 0.02`。

レクトライトの正規化 (本実装の規約):

```
PolygonIrradiance(Poly)   = 0.5 * Σ θ_i n_i  (ベクトル放射照度。半球を覆うと |E| = π)
RectIrradianceLambert     Falloff = |E|、L = E / |E|、NoL = SphereHorizonCosWrap(N・L, |E| / π)
RectGGXApproxLTC          LTC 空間の多角形で同じ積分、Irradiance = (|E| / π) * NoL、
                          結果 = Color * Irradiance * (F0 * Amp.x + (1 - F0) * Amp.y)
```

UE の `Length` (= 形状係数 = |E| / π) と π の置き場所が違うだけで同値。`Tools/LTCFit --verify` が、矩形のスペキュラを力任せ積分と比べて比 ≈ 1.0 (地平線付近を除く) になることを確認している。放射輝度 = `Color / (0.5 * SourceWidth * SourceHeight)` なので、遠方では同じ Intensity のポイントライトの約 2 倍の放射照度になる (UE と同じ)。

### 4.4 選択

```
フォワードディレクショナルライト : ForwardShadingPriority が最大、同値なら Color の輝度が最大
AtmosphereLights[Index]          : bAtmosphereSunLight のライトのうち輝度が最大
ソートキー (LSB から)            : LightType(2) | bTextureProfile | bLightFunction | bUsesLightingChannels
                                   | bShadowed | bIsNotSimpleLight | bClusteredDeferredNotSupported
```

---

## 5. 段階

| 段階 | 内容 | 受け入れ条件 |
|---|---|---|
| L0 | 検証の準備。ベースライン画像 (AA 無し、静止) | 2 回の実行がバイト一致する (確認済み) |
| L1 | C++ のフレームワーク (コンポーネント / プロキシ / SceneInfo / FScene / 可視判定 / ソート)。GPU へ渡すデータとシェーダは旧形式のまま | ベースラインとバイト一致 |
| L2 | GPU のライトデータと全シェーダを UE の形へ。LTC | 光源形状の無い構成で移行前と一致 (浮動小数点の誤差内)。面光源は目視と数値で確認 |
| L3 | 消費側 (Lumen / Volumetric Fog / フォグ / シャドウ) を新しい経路へ | 同上 |
| L4 | 追加プロパティ (フェード、DiffuseScale、フラグ類、コンタクトシャドウ) と Details / INI | 既定値で L3 と一致。各プロパティが効く |
| L5 | Debug ビルド、デバッグレイヤ、自己テスト、文書の更新 | エラー無し |

### 5.1 進捗 (2026-10-03)

L0〜L5 まで完了。L3 は L2 と同時に行った。L4 で追加した Details / INI のプロパティ (INI キーは UE のプロパティ名から先頭の `b` を除いたもの):

```
全ライト      : Visible, CastDynamicShadows, AffectTranslucentLighting, CastVolumetricShadow,
                AffectGlobalIllumination, IndirectLightingIntensity, DiffuseScale,
                ContactShadowLength, ContactShadowLengthInWS, ContactShadowCastingIntensity,
                ContactShadowNonCastingIntensity
ローカル      : MaxDrawDistance, MaxDistanceFadeRange
ディレクショナル : LightSourceSoftAngle, ForwardShadingPriority, AtmosphereSunLight, AtmosphereSunLightIndex
```

無いキーはコンポーネントの既定値 (旧 INI との後方互換)。Details の Light Color は sRGB で編集し、線形で保持する。Light Grid ウィンドウに `FLightStats` を表示する。自己テスト (ライト専用) は追加していない。

---

## 6. 検証

- テストドライバ: `x64/Release/DirectX12.exe -taatest=static -taaframes=90 -taacapture=90 -aa=0 -taaout=<dir>`。AA 無しの静止シナリオは決定的 (2 回の実行がバイト一致)。
- 設定を変えた比較は、作業ディレクトリを分けて行う (`Asset` と `Shader` をジャンクションで共有し、`Saved/Config/EngineSettings.ini` だけ差し替える)。ユーザーの INI は書き換えない。
- 画像の比較は純 Python の差分ツール (PSNR / 最大差 / 差分画像)。

### 6.1 結果 (2026-10-03、移行前 = 72f8f63 のビルドとの比較)

| 構成 | 結果 |
|---|---|
| L1 (C++ のみ)、7 構成 | すべてバイト一致 |
| 点光源相当 (`SourceRadius = 0`、`LightSourceAngle = 0`、レクト無し) | PSNR 54 dB、差 > 1 の画素 0.02 % (光沢のハイライト縁の丸め差) |
| レクト無し (球光源 r = 0.26 m、太陽の見かけ角 1 度) | PSNR 46 dB (面光源の正規化と地平線の回り込みの差) |
| レクトあり | レクトに照らされる面が線形で約 1.6〜2.3 倍明るい (UE の正規化どおり)。床の鏡面反射に矩形が映る |
| 同じ設定の 2 回の実行 | バイト一致 (決定性を維持) |
| `Visible=false` と `AffectsWorld=false` | バイト一致 |
| `CastVolumetricShadow` | ローカルライトのフォグ内の影が切り替わる (既定 off) |
| Debug ビルド (デバッグレイヤ、ERROR で break) | エラー無し。警告は移行前からあるもの (クリア値の不一致 / バッファの初期ステート) のみ |
