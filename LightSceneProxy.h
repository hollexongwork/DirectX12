#pragma once
#include <DirectXMath.h>
#include <cfloat>
#include "BoxSphereBounds.h"

using namespace DirectX;

// ============================================================
//  FLightSceneProxy
//  FLightSceneProxy (Engine/Public/LightSceneProxy.h) に相当する
//  ライトのレンダー側ミラー。ULightComponent::CreateSceneProxy() が
//  種別ごとの派生を生成し、FLightSceneInfo が所有する。
//  レンダラ (FSceneRenderer) はこのプロキシだけを読み、ゲーム側の
//  コンポーネントには触れない。
//
//  階層:
//    FLightSceneProxy
//      ├ FDirectionalLightSceneProxy          (DirectionalLightSceneProxy.h)
//      └ FLocalLightSceneProxy                (LocalLightSceneProxy.h)
//          ├ FPointLightSceneProxy            (PointLightSceneProxy.h)
//          │   └ FSpotLightSceneProxy         (SpotLightSceneProxy.h)
//          └ FRectLightSceneProxy             (RectLightSceneProxy.h)
//
//  データフロー (一方向):
//    - 生成 / 破棄              : FScene::AddLight / RemoveLight
//    - トランスフォーム         : FScene::UpdateLightTransform -> SetTransform
//    - 色 / 明るさ (軽量経路)   : FScene::UpdateLightColorAndBrightness -> SetColor
//    - それ以外のプロパティ変更 : MarkRenderStateDirty -> フレーム末尾に作り直し
//
//  軸の対応 [PORT]: UE のライトのローカル軸 (X = 前方, Y = 右, Z = 上) を
//  本エンジンの (Z = 前方, X = 右, Y = 上) に読み替える。
//    GetDirection() = LightToWorld の行 2 (+Z) = 発光方向
//    Tangent        = LightToWorld の行 1 (+Y) = ライトの上方向
//  単位はメートル。
// ============================================================

class ULightComponent;
class FLightSceneInfo;
class FScene;

// ---- ライト種別 (ELightComponentType。HLSL 側 LIGHT_TYPE_* と 1:1。順序変更禁止) ----
enum ELightComponentType
{
	LightType_Directional = 0,
	LightType_Point,
	LightType_Spot,
	LightType_Rect,
	LightType_MAX,
	LightType_NumBits = 2
};

// レンダラが 1 フレームに扱えるライトの上限 [PORT]。
// UE は可変長だが、本エンジンのライトバッファ / ローカルシャドウパラメータは固定長のアップロードヒープ
static const unsigned int MAX_LOCAL_LIGHTS = 64;		// 視界内のローカルライト (Point / Spot / Rect)
static const unsigned int MAX_DIRECTIONAL_LIGHTS = 4;	// ディレクショナルライト

// FScene::AtmosphereLights の数 (UE NUM_ATMOSPHERE_LIGHTS)
static const unsigned int NUM_ATMOSPHERE_LIGHTS = 2;

// ============================================================
//  FLightRenderParameters
//  FLightRenderParameters に相当。FLightSceneProxy::GetLightShaderParameters が
//  ワールド空間で埋め、レンダラ (FSceneRenderer::ComputeLightGrid) が
//  GPU 用の FForwardLocalLightData へ詰める。
//
//    - Direction は -GetDirection() (受光点からライトへ向かう方向)
//    - Tangent はライトの上方向 (レクトライトの高さ軸 / チューブの軸)
//    - レクトライトは SourceRadius = 半幅、SourceLength = 半高
//    - FalloffExponent はプロキシの値そのまま。逆二乗のライトを 0 にするのは
//      レンダラの役目 (UE と同じ)
// ============================================================
struct FLightRenderParameters
{
	XMFLOAT3 WorldPosition = { 0.0f, 0.0f, 0.0f };	// ワールド位置 [m]
	float    InvRadius = 0.0f;						// 1 / AttenuationRadius

	XMFLOAT3 Color = { 0.0f, 0.0f, 0.0f };			// 線形色 x 明るさ
	float    FalloffExponent = 0.0f;

	XMFLOAT3 Direction = { 0.0f, 0.0f, 1.0f };		// -GetDirection()
	XMFLOAT3 Tangent = { 0.0f, 1.0f, 0.0f };		// ライトの上方向

	XMFLOAT2 SpotAngles = { 0.0f, 0.0f };			// x = cos(Outer), y = 1 / (cos(Inner) - cos(Outer))

	float    SpecularScale = 1.0f;
	float    DiffuseScale = 1.0f;

	float    SourceRadius = 0.0f;
	float    SoftSourceRadius = 0.0f;
	float    SourceLength = 0.0f;

	float    RectLightBarnCosAngle = 0.0f;
	float    RectLightBarnLength = 0.0f;

	unsigned int bAffectsTranslucentLighting = 1;
};

class FLightSceneProxy
{
public:
	// 生成時にコンポーネントから共通プロパティをスナップショットする
	explicit FLightSceneProxy(const ULightComponent* InLightComponent);
	virtual ~FLightSceneProxy();

	// ---- 仮想インターフェース ----
	// 境界がこのライトの影響範囲と交差するか
	virtual bool AffectsBounds(const FBoxSphereBounds& Bounds) const { return true; }

	// ライトの影響範囲を包む球
	virtual FSphere GetBoundingSphere() const
	{
		// ディレクショナルライトはシーン全体
		return FSphere(XMFLOAT3(0.0f, 0.0f, 0.0f), HALF_WORLD_MAX);
	}

	// 減衰半径 [m]
	virtual float GetRadius() const { return FLT_MAX; }
	// スポットライトの外側コーン半角 [rad]
	virtual float GetOuterConeAngle() const { return 0.0f; }
	virtual float GetSourceRadius() const { return 0.0f; }
	virtual bool  IsInverseSquared() const { return true; }
	virtual bool  IsRectLight() const { return false; }
	virtual bool  IsLocalLight() const { return false; }
	// ディレクショナルライトの見かけの全角 [度]
	virtual float GetLightSourceAngle() const { return 0.0f; }
	// Distance Field シャドウの 1 レイの最大トレース距離 [m]
	virtual float GetTraceDistance() const { return 0.0f; }
	// 描画距離 [m] (0 = 無制限) とフェード幅 [m]。FLocalLightSceneProxy が持つ
	virtual float GetMaxDrawDistance() const { return 0.0f; }
	virtual float GetFadeRange() const { return 0.0f; }
	// フォワードシェーディング (半透明 / Volumetric Fog) のディレクショナルライト選択の優先度
	virtual int   GetDirectionalLightForwardShadingPriority() const { return 0; }

	// GPU へ渡すパラメータをワールド空間で構築する
	virtual void GetLightShaderParameters(FLightRenderParameters& OutLightParameters) const {}

	// ---- アクセサ ----
	const XMFLOAT4X4& GetWorldToLight() const { return m_WorldToLight; }
	const XMFLOAT4X4& GetLightToWorld() const { return m_LightToWorld; }

	// 発光方向 (正規化)。UE は LightToWorld の X 軸、本エンジンは +Z [PORT]
	XMFLOAT3 GetDirection() const { return XMFLOAT3(m_LightToWorld._31, m_LightToWorld._32, m_LightToWorld._33); }
	// ライトの上方向 (正規化)。レクトライトの高さ軸 / チューブの軸
	XMFLOAT3 GetUpVector() const { return XMFLOAT3(m_LightToWorld._21, m_LightToWorld._22, m_LightToWorld._23); }
	// ライトの右方向 (正規化)。レクトライトの幅軸
	XMFLOAT3 GetRightVector() const { return XMFLOAT3(m_LightToWorld._11, m_LightToWorld._12, m_LightToWorld._13); }
	// ワールド位置
	XMFLOAT3 GetOrigin() const { return XMFLOAT3(m_LightToWorld._41, m_LightToWorld._42, m_LightToWorld._43); }
	// 同次位置 (ローカルライトは w = 1、ディレクショナルライトは -方向 x WORLD_MAX で w = 0)
	const XMFLOAT4& GetPosition() const { return m_Position; }

	// 線形色 x 明るさ (色温度 / 単位換算込み)
	const XMFLOAT3& GetColor() const { return m_Color; }
	float GetIndirectLightingScale() const { return m_IndirectLightingScale; }
	float GetVolumetricScatteringIntensity() const { return m_VolumetricScatteringIntensity; }
	float GetSpecularScale() const { return m_SpecularScale; }
	float GetDiffuseScale() const { return m_DiffuseScale; }

	float GetUserShadowBias() const { return m_ShadowBias; }
	float GetUserShadowSlopeBias() const { return m_ShadowSlopeBias; }
	float GetContactShadowLength() const { return m_ContactShadowLength; }
	bool  IsContactShadowLengthInWS() const { return m_bContactShadowLengthInWS; }
	float GetContactShadowCastingIntensity() const { return m_ContactShadowCastingIntensity; }
	float GetContactShadowNonCastingIntensity() const { return m_ContactShadowNonCastingIntensity; }

	bool CastsDynamicShadow() const { return m_bCastDynamicShadow; }
	bool CastsVolumetricShadow() const { return m_bCastVolumetricShadow; }
	bool AffectGlobalIllumination() const { return m_bAffectGlobalIllumination; }
	bool AffectsTranslucentLighting() const { return m_bAffectTranslucentLighting; }
	bool UseRayTracedDistanceFieldShadows() const { return m_bUseRayTracedDistanceFieldShadows; }

	bool IsUsedAsAtmosphereSunLight() const { return m_bUsedAsAtmosphereSunLight; }
	unsigned char GetAtmosphereSunLightIndex() const { return m_AtmosphereSunLightIndex; }

	unsigned char GetLightType() const { return m_LightType; }

	const ULightComponent* GetLightComponent() const { return m_LightComponent; }
	FLightSceneInfo* GetLightSceneInfo() const { return m_LightSceneInfo; }

	// ---- FScene だけが呼ぶ更新 ----
	// LightToWorld はスケール無し (行 = 右 / 上 / 前方 / 位置)
	void SetTransform(const XMFLOAT4X4& InLightToWorld, const XMFLOAT4& InPosition);
	void SetColor(const XMFLOAT3& InColor) { m_Color = InColor; }

protected:
	friend class FScene;
	friend class FLightSceneInfo;

	// このプロキシを持つ FLightSceneInfo (FScene::AddLight が設定する)
	FLightSceneInfo* m_LightSceneInfo = nullptr;

	// 生成元のコンポーネント (識別用。レンダラは内容を読まない)
	const ULightComponent* m_LightComponent = nullptr;

	// ---- トランスフォーム ----
	XMFLOAT4X4 m_LightToWorld;
	XMFLOAT4X4 m_WorldToLight;
	XMFLOAT4   m_Position = { 0.0f, 0.0f, 0.0f, 1.0f };

	// ---- 色 / スケール ----
	XMFLOAT3 m_Color = { 1.0f, 1.0f, 1.0f };
	float    m_IndirectLightingScale = 1.0f;
	float    m_VolumetricScatteringIntensity = 1.0f;
	float    m_SpecularScale = 1.0f;
	float    m_DiffuseScale = 1.0f;

	// ---- シャドウ ----
	float m_ShadowBias = 0.5f;
	float m_ShadowSlopeBias = 0.5f;
	float m_ContactShadowLength = 0.0f;
	float m_ContactShadowCastingIntensity = 1.0f;
	float m_ContactShadowNonCastingIntensity = 0.0f;
	bool  m_bContactShadowLengthInWS = false;

	bool m_bCastDynamicShadow = true;
	bool m_bCastVolumetricShadow = false;
	bool m_bAffectGlobalIllumination = true;
	bool m_bAffectTranslucentLighting = true;
	bool m_bUseRayTracedDistanceFieldShadows = false;
	bool m_bUsedAsAtmosphereSunLight = false;

	unsigned char m_AtmosphereSunLightIndex = 0;
	unsigned char m_LightType = LightType_Point;
};

// FLinearColor::GetLuminance / IsAlmostBlack 相当 (ライトの選択 / 描画要否の判定に使う)
inline float GetLightColorLuminance(const XMFLOAT3& Color)
{
	return Color.x * 0.3f + Color.y * 0.59f + Color.z * 0.11f;
}

inline bool IsLightColorAlmostBlack(const XMFLOAT3& Color)
{
	const float Delta = 0.00001f;
	return (Color.x * Color.x < Delta) && (Color.y * Color.y < Delta) && (Color.z * Color.z < Delta);
}
