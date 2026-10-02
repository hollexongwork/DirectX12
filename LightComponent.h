#pragma once
#include <DirectXMath.h>
#include "LightComponentBase.h"
#include "LightSceneProxy.h"

using namespace DirectX;

// ============================================================
//  ULightComponent
//  ULightComponent (Engine/Classes/Components/LightComponent.h) に相当。
//  FScene への登録とレンダー側ミラー (FLightSceneProxy) の生成を担う。
//
//  レンダー側への反映は UE と同じ 3 経路:
//    1. MarkRenderStateDirty        : プロキシを作り直す。フレーム末尾
//                                     (FScene::UpdateAllLightSceneInfos) に
//                                     DestroyRenderState -> CreateRenderState
//    2. MarkRenderTransformDirty    : フレーム末尾に SendRenderTransform ->
//                                     FScene::UpdateLightTransform
//    3. UpdateColorAndBrightness    : FScene::UpdateLightColorAndBrightness
//                                     (プロキシを作り直さない軽量経路)
//
//  シーンに入るのは bAffectsWorld && IsVisible() && Intensity > 0 のライトだけ
//  (ULightComponent::CreateRenderState_Concurrent と同じ条件)。
// ============================================================
class ULightComponent : public ULightComponentBase
{
protected:
	// ---- 色温度 ----
	float m_Temperature = 6500.0f;			// ケルビン (1700..12000 が実用域。6500K は白)
	bool  m_bUseTemperature = false;

	// ---- 描画距離 ----
	float m_MaxDrawDistance = 0.0f;			// これより遠いローカルライトは描かない [m] (0 = 無制限)
	float m_MaxDistanceFadeRange = 0.0f;	// MaxDrawDistance の手前でフェードする幅 [m]

	// ---- スケール (物理的ではない。写真の偏光フィルタ的な調整用) ----
	float m_SpecularScale = 1.0f;
	float m_DiffuseScale = 1.0f;

	// ---- シャドウ ----
	float m_ShadowBias = 0.5f;				// 受光側の深度バイアス
	float m_ShadowSlopeBias = 0.5f;			// 法線オフセット (スロープバイアス相当)

	// コンタクトシャドウ (スクリーンスペースのレイ)。0 で無効。
	// 長さはスクリーン空間 (画面の高さに対する割合) か、ContactShadowLengthInWS ならワールド空間 [m]
	float m_ContactShadowLength = 0.0f;
	bool  m_ContactShadowLengthInWS = false;
	float m_ContactShadowCastingIntensity = 1.0f;		// 影を落とすジオメトリに当たったときの濃さ
	float m_ContactShadowNonCastingIntensity = 0.0f;	// 影を落とさないジオメトリに当たったときの濃さ

	// シャドウマップの代わりにメッシュ SDF をレイマーチして影を評価する
	bool  m_bUseRayTracedDistanceFieldShadows = false;

	// ---- 可視性 (USceneComponent::bVisible。SetVisibility / ALight::SetEnabled が切り替える) ----
	bool  m_bVisible = true;

	// シーンに (可視のライトとして) 入っているか
	bool  m_bAddedToSceneVisible = false;

	// レンダー側ミラー (所有は FLightSceneInfo。ここは参照)
	FLightSceneProxy* m_SceneProxy = nullptr;

public:
	// ---- 登録 (UActorComponent) ----
	void OnRegister() override;		// CreateRenderState
	void OnUnregister() override;	// DestroyRenderState

	// ---- レンダーステート (UActorComponent::*RenderState_Concurrent) ----
	void CreateRenderState();
	void DestroyRenderState();
	// DestroyRenderState -> CreateRenderState (FScene::UpdateAllLightSceneInfos が呼ぶ)
	void RecreateRenderState();
	// トランスフォームをプロキシへ送る (SendRenderTransform_Concurrent)
	void SendRenderTransform();

	// ---- ダーティ通知 ----
	// フラグを立て、登録済みなら FScene のダーティリストへ自分を積む
	// (フラグが既に立っていれば積まない = 二重登録防止)
	void MarkRenderStateDirty() override;
	void MarkRenderTransformDirty() override;

	// 色 / 明るさだけが変わったときの軽量更新。
	// Intensity が 0 を跨いだときはシーンへの出し入れが要るので作り直しに回す
	void UpdateColorAndBrightness();

	// ---- 派生が実装する ----
	// レンダー側ミラーの生成 (ULightComponent::CreateSceneProxy)
	virtual FLightSceneProxy* CreateSceneProxy() const { return nullptr; }
	virtual ELightComponentType GetLightType() const = 0;
	// 同次位置。ローカルライトは (位置, 1)、ディレクショナルライトは (-方向 x WORLD_MAX, 0)
	virtual XMFLOAT4 GetLightPosition() const = 0;
	// ライトの影響範囲を包む球
	virtual FSphere GetBoundingSphere() const { return FSphere(XMFLOAT3(0.0f, 0.0f, 0.0f), HALF_WORLD_MAX); }
	// 単位換算後の明るさ。基底は Intensity そのまま
	virtual float ComputeLightBrightness() const;
	// Exponential Height Fog の太陽 (FScene::AtmosphereLights) に使うか。ディレクショナルライトだけが真を返す
	virtual bool IsUsedAsAtmosphereSunLight() const { return false; }
	virtual unsigned char GetAtmosphereSunLightIndex() const { return 0; }

	// 発光方向 (ULightComponent::GetDirection)
	XMFLOAT3 GetDirection() const { return GetForwardVector(); }

	// スケール無しのコンポーネント行列 (行 = 右 / 上 / 前方 / 位置)。
	// FTransform::ToMatrixNoScale 相当。プロキシの LightToWorld になる
	XMFLOAT4X4 GetLightToWorldNoScale() const;

	// 色 x 明るさ x 色温度 (ULightComponent::GetColoredLightBrightness)
	XMFLOAT3 GetColoredLightBrightness() const;

	FLightSceneProxy* GetSceneProxy() const { return m_SceneProxy; }
	void SetSceneProxy(FLightSceneProxy* Proxy) { m_SceneProxy = Proxy; }
	bool IsAddedToSceneVisible() const { return m_bAddedToSceneVisible; }

	// ---- 可視性 (USceneComponent::SetVisibility) ----
	void SetVisibility(bool bNewVisibility);
	void ToggleVisibility() { SetVisibility(!m_bVisible); }
	bool IsVisible() const { return m_bVisible; }

	// ---- 軽量経路で反映されるプロパティ (UpdateColorAndBrightness) ----
	void SetIntensity(float NewIntensity);
	void SetIndirectLightingIntensity(float NewIntensity);
	void SetVolumetricScatteringIntensity(float NewIntensity);
	// 線形色を渡す (UE は内部で sRGB の FColor に量子化するが、本エンジンは線形のまま保持する [PORT])
	void SetLightColor(const XMFLOAT4& NewLightColor);
	void SetTemperature(float NewTemperature);
	void SetUseTemperature(bool bNewValue);

	// ---- プロキシを作り直すプロパティ (UE にセッターがあるもの) ----
	void SetAffectTranslucentLighting(bool bNewValue);
	void SetShadowBias(float NewValue);
	void SetShadowSlopeBias(float NewValue);
	void SetSpecularScale(float NewValue);
	void SetDiffuseScale(float NewValue);

	// ---- プロキシを作り直すプロパティ (エディタ編集相当) ----
	void SetMaxDrawDistance(float NewValue);
	void SetMaxDistanceFadeRange(float NewValue);
	void SetContactShadowLength(float NewValue);
	void SetContactShadowLengthInWS(bool bNewValue);
	void SetContactShadowCastingIntensity(float NewValue);
	void SetContactShadowNonCastingIntensity(float NewValue);
	void SetUseRayTracedDistanceFieldShadows(bool bNewValue);

	// ---- 読み取り ----
	float GetTemperature() const { return m_Temperature; }
	bool  GetUseTemperature() const { return m_bUseTemperature; }
	float GetMaxDrawDistance() const { return m_MaxDrawDistance; }
	float GetMaxDistanceFadeRange() const { return m_MaxDistanceFadeRange; }
	float GetSpecularScale() const { return m_SpecularScale; }
	float GetDiffuseScale() const { return m_DiffuseScale; }
	float GetShadowBias() const { return m_ShadowBias; }
	float GetShadowSlopeBias() const { return m_ShadowSlopeBias; }
	float GetContactShadowLength() const { return m_ContactShadowLength; }
	bool  GetContactShadowLengthInWS() const { return m_ContactShadowLengthInWS; }
	float GetContactShadowCastingIntensity() const { return m_ContactShadowCastingIntensity; }
	float GetContactShadowNonCastingIntensity() const { return m_ContactShadowNonCastingIntensity; }
	bool  GetUseRayTracedDistanceFieldShadows() const { return m_bUseRayTracedDistanceFieldShadows; }

	// Planckian locus による色温度 -> リニア sRGB 変換
	// (FLinearColor::MakeFromColorTemperature)
	static XMFLOAT3 MakeFromColorTemperature(float TemperatureKelvin);
};
