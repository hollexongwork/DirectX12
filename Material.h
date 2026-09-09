#pragma once
#include <windows.h>
#include <DirectXMath.h>
using namespace DirectX;

class RenderManager;

// ============================================================
//  EBlendMode (EngineTypes.h の EBlendMode 相当)
//  マテリアルのシーンへの合成方法。
//    BLEND_Opaque      : 上書き (ベースパス -> G-Buffer)
//    BLEND_Masked      : OpacityMask を OpacityMaskClipValue で
//                        clip した上で不透明として描く (ベースパス)
//    BLEND_Translucent : SrcAlpha / InvSrcAlpha でフォワード合成
//                        (トランスルーセンシーパス, 深度書き込みなし)
//    BLEND_Additive    : SrcAlpha / One で加算合成 (同上)
// ============================================================
enum class EBlendMode : unsigned int
{
	BLEND_Opaque = 0,
	BLEND_Masked = 1,
	BLEND_Translucent = 2,
	BLEND_Additive = 3,
	BLEND_MAX,
};

// MaterialShared.h の IsTranslucentBlendMode / IsMaskedBlendMode /
// IsOpaqueBlendMode 相当のヘルパ
inline bool IsTranslucentBlendMode(EBlendMode BlendMode)
{
	return BlendMode == EBlendMode::BLEND_Translucent
		|| BlendMode == EBlendMode::BLEND_Additive;
}

inline bool IsMaskedBlendMode(EBlendMode BlendMode)
{
	return BlendMode == EBlendMode::BLEND_Masked;
}

inline bool IsOpaqueBlendMode(EBlendMode BlendMode)
{
	return BlendMode == EBlendMode::BLEND_Opaque;
}

// ============================================================
//  ESubstrateSSSType (HLSL SUBSTRATE_SSS_TYPE_* と 1:1)
//  Substrate Slab の Sub-Surface Type 。
//    None             : 標準 Lambert
//    Wrap             : ラップライティング
//    TwoSidedWrap     : 表ラップ + 裏面透過 (葉など)
//    Diffusion        : スクリーン空間拡散 (本エンジンでは
//                       非散乱へフォールバック)
//    DiffusionProfile : プロファイル拡散 (同上フォールバック)
//    SimpleVolume     : 単散乱スラブ (Beer-Lambert 透過)
// ============================================================
enum class ESubstrateSSSType : unsigned int
{
	None = 0,
	Wrap = 1,
	TwoSidedWrap = 2,
	Diffusion = 3,
	DiffusionProfile = 4,
	SimpleVolume = 5,
	MAX,
};

// ============================================================
//  ERefractionMethod (HLSL REFRACTION_METHOD_* と 1:1)
//  ERefractionMode 相当。ゼロ初期化 = 屈折なしにする
//  ため None = 0 とする。
//    IndexOfRefraction : 屈折率 (ビュー空間法線 x (IOR-1))
//    PixelNormalOffset : 頂点法線とピクセル法線の差分
//    Offset2D          : Data.xy をピクセル単位オフセットとして直接使用
// ============================================================
enum class ERefractionMethod : unsigned int
{
	None = 0,
	IndexOfRefraction = 1,
	PixelNormalOffset = 2,
	Offset2D = 3,
	MAX,
};

class Material
{
private:

	struct MATERIAL
	{
		XMFLOAT4		BaseColor;
		XMFLOAT4		EmissionColor;

		float			Metallic;
		float			Specular;
		float			Roughness;
		float			NormalWeight;

		BOOL			Unlit;
		// Translucent / Additive の不透明度 (BaseColor テクスチャ α x
		// 頂点カラー α に乗算)。Opaque / Masked では未使用。
		float			Opacity;
		// BLEND_Masked の clip しきい値 (UMaterial::OpacityMaskClipValue)
		float			OpacityMaskClipValue;
		// EBlendMode (HLSL 側 BLEND_* 定数と 1:1)
		EBlendMode		BlendMode;

		// bTwoSided: ラスタライザのカリング無効 + 裏面の法線反転
		BOOL			TwoSided;
		XMFLOAT3		_pad;

		// ---- Substrate Slab BSDF ----
		// bUseSubstrate = TRUE のときレガシー Metallic/Specular の
		// 代わりに Slab (DiffuseAlbedo / F0 / F90 / SSS) で
		// シェーディングする。MFP は TransmittanceColor + Thickness
		// から HLSL 側 TransmittanceToMeanFreePath で導出。
		XMFLOAT4		SubstrateDiffuseAlbedo;       // rgb (w 未使用)
		XMFLOAT4		SubstrateF0;                  // rgb (w 未使用)
		XMFLOAT4		SubstrateF90;                 // rgb (w 未使用)
		XMFLOAT4		SubstrateTransmittanceColor;  // rgb = 透過色 (指定厚での透過率) / w = 予約 (未使用)
		XMFLOAT4		SubstrateFuzzColor;           // rgb = ファズ色 / w = FuzzAmount

		float			SubstrateAnisotropy;          // [-1,1] (評価は等方近似)
		float			SubstrateSSSPhaseAnisotropy;  // Henyey-Greenstein の g [-1,1]
		float			SubstrateThickness;           // スラブ厚 [cm]
		ESubstrateSSSType SubstrateSSSType;           // HLSL SUBSTRATE_SSS_TYPE_* と 1:1

		float			SubstrateSecondRoughness;
		float			SubstrateSecondRoughnessWeight;
		float			SubstrateFuzzRoughness;
		BOOL			SubstrateIsThin;              // Thin Surface

		BOOL			bUseSubstrate;                // Slab ワークフロー有効
		ERefractionMethod RefractionMethod;           // HLSL REFRACTION_METHOD_* と 1:1
		// x = IOR (IndexOfRefraction) / 法線強度 (PixelNormalOffset)
		// xy = 画面オフセット [pixel] (Offset2D)
		XMFLOAT2		RefractionData;

		float			RefractionDepthBias;          // 屈折の深度棄却バイアス [m]
		// Index Of Refraction From F0 : TRUE のとき IOR を
		// 手入力値でなく SubstrateF0 から導出する (Substrate + IOR 方式のみ)
		BOOL			bRefractionUseF0;
		XMFLOAT2		_padSubstrate;
	};
	static_assert(sizeof(MATERIAL) == 224, "MATERIAL must mirror HLSL MaterialConstantBuffer (b2, 224 bytes)");

	// b2 : MaterialConstantBuffer (HLSL 側と 1:1 ミラー必須)
	struct MATERIAL_CONSTANT
	{
		MATERIAL Material;
	};
	
public:	

	Material();

	MATERIAL Params{};

	// ---- UMaterialInterface 相当のアクセサ ----
	EBlendMode GetBlendMode() const { return Params.BlendMode; }
	void       SetBlendMode(EBlendMode BlendMode) { Params.BlendMode = BlendMode; }

	bool IsTwoSided() const { return Params.TwoSided != FALSE; }
	void SetTwoSided(bool bTwoSided) { Params.TwoSided = bTwoSided ? TRUE : FALSE; }

	float GetOpacityMaskClipValue() const { return Params.OpacityMaskClipValue; }

	// ---- Substrate ----
	bool IsSubstrateEnabled() const { return Params.bUseSubstrate != FALSE; }
	void SetUseSubstrate(bool bUse) { Params.bUseSubstrate = bUse ? TRUE : FALSE; }

	ESubstrateSSSType GetSubstrateSSSType() const { return Params.SubstrateSSSType; }
	void SetSubstrateSSSType(ESubstrateSSSType Type) { Params.SubstrateSSSType = Type; }

	// ---- Refraction ----
	ERefractionMethod GetRefractionMethod() const { return Params.RefractionMethod; }
	void SetRefractionMethod(ERefractionMethod Method) { Params.RefractionMethod = Method; }

	bool IsRefractionUseF0() const { return Params.bRefractionUseF0 != FALSE; }
	void SetRefractionUseF0(bool bUse) { Params.bRefractionUseF0 = bUse ? TRUE : FALSE; }

	void Bind(RenderManager* rm) const;
};
