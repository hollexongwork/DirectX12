#pragma once
#include <DirectXMath.h>
#include "SceneComponent.h"

using namespace DirectX;

// ============================================================
//  ライトコンポーネント階層
//
//    USceneComponent
//      └ ULightComponentBase                  (LightComponentBase.h)       … 強度 / 色 / シーンへの影響
//          └ ULightComponent                  (LightComponent.h)           … 色温度 / シャドウ / FScene 登録 / プロキシ
//              ├ UDirectionalLightComponent   (DirectionalLightComponent.h)  強度 = lux
//              └ ULocalLightComponent         (LocalLightComponent.h)      … AttenuationRadius / IntensityUnits
//                  ├ UPointLightComponent     (PointLightComponent.h)      … 光源形状 / 逆二乗
//                  │   └ USpotLightComponent  (SpotLightComponent.h)       … Inner / OuterConeAngle
//                  └ URectLightComponent      (RectLightComponent.h)       … SourceWidth / Height / BarnDoor
//
//  ※ 距離の単位はメートル
//  ※ 発光方向はコンポーネント +Z、幅軸 (レクトライトの幅) は +X、
//    上方向 (レクトライトの高さ / チューブの軸) は +Y
// ============================================================

// ============================================================
//  ULightComponentBase
//  ULightComponentBase に相当。
//  全ライト (将来のスカイライトを含む) に共通するプロパティを持つ。
//
//  セッターの規約:
//    - ランタイムに変わり得るもの (SetCastShadows など) は
//      「値が変わったときだけ MarkRenderStateDirty」。
//    - エディタ的な設定 (bAffectsWorld など) のセッターも、
//      Details / INI からの編集 (PostEditChangeProperty 相当) として同じ動作にする。
//  本エンジンのライトは全て Movable なので AreDynamicDataChangesAllowed は常に真。
// ============================================================
class ULightComponentBase : public USceneComponent
{
protected:
	// 明るさ。単位はライトの種別による (ディレクショナル = lux、ローカル = IntensityUnits)
	float    m_Intensity = 3.1415926535897932f;			// 既定値 π

	// ライトの色 (線形の float で持つ)。
	// API (SetLightColor / GetLightColor) も線形で受け渡しする
	XMFLOAT4 m_LightColor = { 1.0f, 1.0f, 1.0f, 1.0f };

	// ライトがワールドに影響するか。false のライトはシーンに一切寄与しない
	bool     m_bAffectsWorld = true;

	// 影を落とすか。動的シャドウは CastShadows && CastDynamicShadows のときに落ちる
	bool     m_CastShadows = true;
	bool     m_CastDynamicShadows = true;

	// 半透明 (フォワードシェーディング) を照らすか
	bool     m_bAffectTranslucentLighting = true;

	// Volumetric Fog の中で影を落とすか (既定はディレクショナルライトのみ true)
	bool     m_bCastVolumetricShadow = false;

	// グローバルイルミネーション (Lumen の Surface Cache 直接光) に寄与するか
	bool     m_bAffectGlobalIllumination = true;

	// 間接光への寄与のスケール (Lumen の直接光の色に掛かる)
	float    m_IndirectLightingIntensity = 1.0f;

	// Volumetric Fog への散乱寄与のスケール (0 で寄与なし)
	float    m_VolumetricScatteringIntensity = 1.0f;

	// レンダーステート変更フラグ。立っている = FScene のレンダーステートダーティリストに
	// エンキュー済み (登録中のみ)。次の FScene::UpdateAllLightSceneInfos でプロキシが作り直される
	bool     m_RenderStateDirty = false;

public:
	// ---- レンダーステート更新 (UActorComponent::MarkRenderStateDirty) ----
	// 基底はフラグのみ。ULightComponent がオーバーライドして FScene のダーティリストへ自分を積む
	virtual void MarkRenderStateDirty() { m_RenderStateDirty = true; }
	bool IsRenderStateDirty() const { return m_RenderStateDirty; }
	void ClearRenderStateDirty() { m_RenderStateDirty = false; }

	// ---- ランタイムに変わり得るプロパティ ----
	void SetCastShadows(bool bNewValue);
	bool GetCastShadows() const { return m_CastShadows; }

	void SetCastVolumetricShadow(bool bNewValue);
	bool GetCastVolumetricShadow() const { return m_bCastVolumetricShadow; }

	void SetAffectGlobalIllumination(bool bNewValue);
	bool GetAffectGlobalIllumination() const { return m_bAffectGlobalIllumination; }

	// ---- エディタ編集相当のプロパティ ----
	void SetAffectsWorld(bool bNewValue);
	bool GetAffectsWorld() const { return m_bAffectsWorld; }

	void SetCastDynamicShadows(bool bNewValue);
	bool GetCastDynamicShadows() const { return m_CastDynamicShadows; }

	// ---- 読み取り ----
	float GetIntensity() const { return m_Intensity; }
	// 線形色 (ULightComponentBase::GetLightColor)
	const XMFLOAT4& GetLightColor() const { return m_LightColor; }
	bool  GetAffectTranslucentLighting() const { return m_bAffectTranslucentLighting; }
	float GetIndirectLightingIntensity() const { return m_IndirectLightingIntensity; }
	float GetVolumetricScatteringIntensity() const { return m_VolumetricScatteringIntensity; }

	// 発光方向 (正規化不要) を向くピッチ / ヨー / ロール (ラジアン) を返す。
	// エンジン前方 +Z 規約 (FRotationMatrix::MakeFromX 相当)
	static XMFLOAT3 DirectionToRotator(const XMFLOAT3& Direction);
};
