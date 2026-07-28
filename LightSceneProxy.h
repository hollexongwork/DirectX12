#pragma once
#include <DirectXMath.h>

using namespace DirectX;

// ============================================================
//  FLightSceneProxy
//  FLightSceneProxy に相当するレンダー側ミラー。
//  ULightComponent::CreateLightSceneProxy() が生成し、FScene
//  (FLightSceneInfo) が所有する。レンダラ (FSceneRenderer) は
//  このプロキシだけを読み、ゲーム側オブジェクトには触れない。
//
//  データフロー (FPrimitiveSceneProxy と同じ一方向):
//    - トランスフォーム (位置 / 発光方向 / 幅軸):
//        UWorld::SendAllEndOfFrameUpdates ->
//        ULightComponent::SendRenderTransform -> SetTransform
//    - 強度 / 色 / 半径などのプロパティ変更:
//        MarkRenderStateDirty -> 次フレーム頭でプロキシ再生成
//          (FScene::UpdateAllLightSceneInfos)
// ============================================================

class ULightComponent;

// ---- ライト種別 (HLSL 側 LIGHT_TYPE_* と 1:1。順序変更禁止) ----
enum class ELightType : unsigned int
{
	Directional = 0,	// ENV 定数 (b0) 経由。ライトバッファには積まれない
	Point = 1,
	Spot = 2,
	Rect = 3,
};

// ---- GPU へ送るライトフラグ (HLSL 側 LIGHT_FLAG_* と 1:1) ----
#define LIGHT_FLAG_INVERSE_SQUARED (1u << 0)

// レンダラが 1 フレームに扱えるローカルライト (Point/Spot/Rect) の上限。
// FSceneRenderer のライトバッファと ImGui 表示が参照する。
static const unsigned int MAX_LOCAL_LIGHTS = 64;

// ============================================================
//  FLightShaderParameters
//  FLightShaderParameters に相当する GPU 転送用構造体。
//  StructuredBuffer<FLightShaderParameters> (t13, ForwardLocalLights) として毎フレーム
//  アップロードされる。HLSL 側 (Structs.hlsl) と 1:1 ミラー必須。
//  StructuredBuffer は cbuffer と違いパディング規則が無い逐次
//  レイアウトなので、両側とも 96 バイトで完全一致させること。
//
//    - Rect ライトは SourceRadius = 半幅、
//      SourceLength = 半高を流用する
//    - Direction は「発光方向」(コンポーネント +Z)。
// ============================================================
struct FLightShaderParameters
{
	XMFLOAT3     Position;              // ワールド位置
	float        InvRadius;             // 1 / AttenuationRadius

	XMFLOAT3     Color;                 // 線形色 x 強度 (単位換算済み。cd 相当)
	float        FalloffExponent;       // 逆二乗無効時の指数フォールオフ

	XMFLOAT3     Direction;             // 発光方向 (正規化。コンポーネント +Z)
	float        SpecularScale;         // スペキュラ寄与スケール

	XMFLOAT3     Tangent;               // 幅軸 (正規化。コンポーネント +X。Rect の幅 / チューブの軸)
	float        SourceRadius;          // 球光源半径 (Rect では半幅) [m]

	XMFLOAT2     SpotAngles;            // x = cos(Outer), y = 1 / (cos(Inner) - cos(Outer))
	float        SoftSourceRadius;      // 見かけだけ柔らかくする追加半径 (エネルギー正規化なし) [m]
	float        SourceLength;          // チューブ長 (Rect では半高) [m]

	float        RectLightBarnCosAngle; // バーンドア開き角の cos
	float        RectLightBarnLength;   // バーンドア長 [m]
	unsigned int Type;                  // ELightType
	unsigned int Flags;                 // LIGHT_FLAG_*
};

static_assert(sizeof(FLightShaderParameters) == 96,
	"FLightShaderParameters must be 96 bytes (HLSL Structs.hlsl と 1:1 ミラー)");

class FLightSceneProxy
{
protected:
	// ---- トランスフォーム (SendRenderTransform が毎フレーム更新) ----
	XMFLOAT3 m_Position = { 0.0f, 0.0f, 0.0f };
	XMFLOAT3 m_Direction = { 0.0f, 0.0f, 1.0f };	// 発光方向 (コンポーネント +Z)
	XMFLOAT3 m_Tangent = { 1.0f, 0.0f, 0.0f };		// 幅軸 (コンポーネント +X)

	// ---- レンダーステート (プロキシ再生成時にのみ変わる) ----
	ELightType m_Type = ELightType::Point;
	XMFLOAT3   m_Color = { 1.0f, 1.0f, 1.0f };		// 強度・色温度込みの最終線形色
	float      m_InvRadius = 0.0f;					// 1 / AttenuationRadius (Directional は 0)
	float      m_FalloffExponent = 8.0f;
	float      m_SpecularScale = 1.0f;
	float      m_SourceRadius = 0.0f;
	float      m_SoftSourceRadius = 0.0f;
	float      m_SourceLength = 0.0f;
	XMFLOAT2   m_SpotAngles = { -2.0f, 1.0f };		// cos(Outer) = -2 -> 全方位 (スポット以外)
	float      m_RectBarnCosAngle = 0.0f;			// 0 (= 90 度) -> バーンドア無効
	float      m_RectBarnLength = 0.0f;
	bool       m_bInverseSquared = true;
	bool       m_bAffectsWorld = true;

	// ---- シャドウ (FShadowSceneRenderer が参照) ----
	bool  m_bCastShadows = true;
	float m_ShadowBias = 0.5f;						// 受光側深度バイアススケール (既定 0.5)
	float m_ShadowSlopeBias = 0.5f;					// 法線オフセット (スロープバイアス相当) スケール

	// ディレクショナル CSM 専用 (UDirectionalLightComponent から流し込まれる)
	float m_DynamicShadowDistance = 100.0f;			// CSM カバー距離 [m]
	int   m_NumDynamicShadowCascades = 4;			// カスケード数 (1..MAX_SHADOW_CASCADES)
	float m_CascadeDistributionExponent = 3.0f;		// 分割の指数 (大きいほど手前が細かい)
	float m_ShadowDistanceFadeoutFraction = 0.1f;	// 遠端フェード比率

	// ---- Distance Field Shadows (RayTraced Distance Field Shadows) ----
	bool  m_bUseRTDFShadows = false;				// SDF レイマーチで影を評価するか
	float m_DistanceFieldShadowDistance = 300.0f;	// (Directional) DF シャドウのカバー距離 [m]
	float m_DistanceFieldTraceDistance = 100.0f;	// (Directional) 1 レイの最大トレース距離 [m]
	float m_LightSourceAngle = 1.0f;				// (Directional) 光源の見かけ全角 [度]

public:
	// 生成時にコンポーネントから共通プロパティをスナップショットする。
	// 種別ごとの追加パラメータは各 CreateLightSceneProxy が Set* で流し込む。
	FLightSceneProxy(const ULightComponent* Component);
	virtual ~FLightSceneProxy() = default;

	// ---- 毎フレーム更新 (ULightComponent::SendRenderTransform) ----
	void SetTransform(const XMFLOAT3& Position, const XMFLOAT3& Direction, const XMFLOAT3& Tangent)
	{
		m_Position = Position;
		m_Direction = Direction;
		m_Tangent = Tangent;
	}

	// ---- CreateLightSceneProxy (コンポーネント側) から呼ばれるセットアップ ----
	void SetRadialParameters(float InvRadius, float FalloffExponent, bool bInverseSquared)
	{
		m_InvRadius = InvRadius;
		m_FalloffExponent = FalloffExponent;
		m_bInverseSquared = bInverseSquared;
	}

	void SetSourceShape(float SourceRadius, float SoftSourceRadius, float SourceLength)
	{
		m_SourceRadius = SourceRadius;
		m_SoftSourceRadius = SoftSourceRadius;
		m_SourceLength = SourceLength;
	}

	void SetSpotAngles(float CosOuterCone, float InvCosConeDifference)
	{
		m_SpotAngles = { CosOuterCone, InvCosConeDifference };
	}

	void SetRectBarnDoor(float BarnCosAngle, float BarnLength)
	{
		m_RectBarnCosAngle = BarnCosAngle;
		m_RectBarnLength = BarnLength;
	}

	void SetShadowParameters(float ShadowBias, float ShadowSlopeBias)
	{
		m_ShadowBias = ShadowBias;
		m_ShadowSlopeBias = ShadowSlopeBias;
	}

	void SetDirectionalShadowParameters(float DynamicShadowDistance, int NumCascades, float DistributionExponent, float FadeoutFraction)
	{
		m_DynamicShadowDistance = DynamicShadowDistance;
		m_NumDynamicShadowCascades = NumCascades;
		m_CascadeDistributionExponent = DistributionExponent;
		m_ShadowDistanceFadeoutFraction = FadeoutFraction;
	}

	void SetDistanceFieldParameters(float DFShadowDistance, float DFTraceDistance, float SourceAngleDeg)
	{
		m_DistanceFieldShadowDistance = DFShadowDistance;
		m_DistanceFieldTraceDistance = DFTraceDistance;
		m_LightSourceAngle = SourceAngleDeg;
	}

	// ---- レンダラ (FSceneRenderer) 用アクセサ ----
	ELightType      GetLightType() const { return m_Type; }
	bool            AffectsWorld() const { return m_bAffectsWorld; }
	const XMFLOAT3& GetColor() const { return m_Color; }
	const XMFLOAT3& GetDirection() const { return m_Direction; }
	const XMFLOAT3& GetPosition() const { return m_Position; }

	// ---- シャドウ (FShadowSceneRenderer) 用アクセサ ----
	bool  CastsShadows() const { return m_bCastShadows; }
	float GetShadowBias() const { return m_ShadowBias; }
	float GetShadowSlopeBias() const { return m_ShadowSlopeBias; }
	float GetAttenuationRadius() const { return (m_InvRadius > 0.0f) ? (1.0f / m_InvRadius) : 0.0f; }
	const XMFLOAT2& GetSpotAngles() const { return m_SpotAngles; }
	float GetRectBarnCosAngle() const { return m_RectBarnCosAngle; }
	float GetDynamicShadowDistance() const { return m_DynamicShadowDistance; }
	int   GetNumDynamicShadowCascades() const { return m_NumDynamicShadowCascades; }
	float GetCascadeDistributionExponent() const { return m_CascadeDistributionExponent; }
	float GetShadowDistanceFadeoutFraction() const { return m_ShadowDistanceFadeoutFraction; }
	bool  UseRayTracedDistanceFieldShadows() const { return m_bUseRTDFShadows; }
	float GetDistanceFieldShadowDistance() const { return m_DistanceFieldShadowDistance; }
	float GetDistanceFieldTraceDistance() const { return m_DistanceFieldTraceDistance; }
	float GetLightSourceAngle() const { return m_LightSourceAngle; }

	// GPU 転送用パラメータ構築 (FLightSceneProxy::GetLightShaderParameters)
	void GetLightShaderParameters(FLightShaderParameters& Out) const
	{
		Out.Position = m_Position;
		Out.InvRadius = m_InvRadius;
		Out.Color = m_Color;
		Out.FalloffExponent = m_FalloffExponent;
		Out.Direction = m_Direction;
		Out.SpecularScale = m_SpecularScale;
		Out.Tangent = m_Tangent;
		Out.SourceRadius = m_SourceRadius;
		Out.SpotAngles = m_SpotAngles;
		Out.SoftSourceRadius = m_SoftSourceRadius;
		Out.SourceLength = m_SourceLength;
		Out.RectLightBarnCosAngle = m_RectBarnCosAngle;
		Out.RectLightBarnLength = m_RectBarnLength;
		Out.Type = (unsigned int)m_Type;
		Out.Flags = m_bInverseSquared ? LIGHT_FLAG_INVERSE_SQUARED : 0u;
	}
};
