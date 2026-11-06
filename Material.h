#pragma once
#include <windows.h>
#include <DirectXMath.h>
#include <memory>
using namespace DirectX;

class RenderManager;
struct TEXTURE;

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
//  EMaterialDomain (EngineTypes.h の EMaterialDomain 相当)
//  マテリアルがどのレンダリング経路で評価されるか。
//    MD_Surface : サーフェス (ベースパス / トランスルーセンシー /
//                 シャドウ / ベロシティ / Lumen カード)
//    MD_Volume  : ボリューム (Volumetric Fog の froxel ボリュームへ
//                 プリミティブの境界をボクセル化する。サーフェス系の
//                 どのパスにも描かれない。FVolumetricFog::
//                 VoxelizeFogVolumePrimitives)
//  [PORT] DeferredDecal / LightFunction / PostProcess / UI は無い
// ============================================================
enum class EMaterialDomain : unsigned int
{
	MD_Surface = 0,
	MD_Volume = 1,
	MD_MAX,
};

// ============================================================
//  EVolumeShape
//  Volume ドメインのボクセル化形状 (VolumetricFogVoxelization の
//  EVoxelizeShape 相当)。プリミティブの境界ボックス (OBB) か
//  境界球のどちらに媒質を詰めるか。[PORT] UE はメッシュから決める
//  (UE のサンプルは SphereMask ノードで球にする) が、本エンジンは
//  マテリアルグラフが無いのでマテリアルのプロパティにする
// ============================================================
enum class EVolumeShape : unsigned int
{
	Box = 0,
	Sphere = 1,
	MAX,
};

// ============================================================
//  FVolumeMaterialParameters
//  Volume ドメインのマテリアル入力 (MaterialTemplate の Volume 用
//  ピン: Albedo / Emissive Color / Extinction / Ambient Occlusion)
//  と、それを作るノード (UE のサンプル M_VolumetricFog 相当:
//  FogColor -> Albedo, Noise(AbsoluteWorldPosition x Scale, Panner)
//  x Opacity x Hardness -> Extinction) の固定機能版。
//  b2 (MaterialConstantBuffer) の外。FVolumetricFog の
//  ボクセル化パスが自前の cbuffer (FVoxelizeVolumeElementParams) へ
//  コピーする。HLSL 側 VolumetricFogVoxelizationCommon.hlsl の
//  FVolumeMaterialParameters と 1:1 ミラー (96 bytes)。
//  単位はメートル (Extinction [1/m], Emissive [/m])。
// ============================================================
struct FVolumeMaterialParameters
{
	// rgb = Albedo (散乱アルベド, FogColor パラメータ), w = AmbientOcclusion (0..1)
	XMFLOAT4 Albedo = { 1.0f, 1.0f, 1.0f, 1.0f };
	// rgb = Emissive Color [/m], w = 未使用
	XMFLOAT4 Emissive = { 0.0f, 0.0f, 0.0f, 0.0f };
	// x = Extinction (Opacity パラメータ) [1/m], y = Hardness (Extinction 乗数),
	// z = EdgeFalloff (境界からのフェード幅。形状半径比 0..1), w = EVolumeShape
	XMFLOAT4 ExtinctionParams = { 0.1f, 0.6f, 0.0f, 0.0f };
	// x = Noise Scale (ワールド座標の乗数 = 周波数 [1/m]), y = Levels (オクターブ数),
	// z = OutputMin, w = OutputMax (Noise ノードの出力レンジ)
	XMFLOAT4 NoiseParams = { 1.0f, 4.0f, 0.0f, 1.0f };
	// xyz = Panner Speed (ワールド座標のスクロール [m/s]), w = LevelScale (オクターブ間の周波数比)
	XMFLOAT4 NoisePanner = { 0.0f, 0.0f, 0.0f, 2.0f };
	// x = bUseNoise (0/1), y = bTurbulence (0/1: |noise| で尖らせる), z = bUseFilterWidth
	// (0/1: froxel サイズより細かいオクターブを減衰), w = 未使用
	XMFLOAT4 NoiseFlags = { 1.0f, 0.0f, 1.0f, 0.0f };
};
static_assert(sizeof(FVolumeMaterialParameters) == 96,
	"FVolumeMaterialParameters must mirror HLSL FVolumeMaterialParameters (96 bytes)");

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
		// シェーディングする。MFP は TransmittanceColor と固定参照厚 1cm
		// (SUBSTRATE_TRANSMITTANCE_REFERENCE_CM) から HLSL 側
		// TransmittanceToMeanFreePath で導出し、Thickness は SSS 評価厚
		// として濃度をスケールする。
		XMFLOAT4		SubstrateDiffuseAlbedo;       // rgb (w 未使用)
		XMFLOAT4		SubstrateF0;                  // rgb (w 未使用)
		XMFLOAT4		SubstrateF90;                 // rgb (w 未使用)
		XMFLOAT4		SubstrateTransmittanceColor;  // rgb = 透過色 (参照厚 1cm あたりの透過率) / w = 予約 (未使用)
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

	// ---- Responsive AA (UMaterial::bEnableResponsiveAA) ----
	// CPU 専用フラグ (MATERIAL / b2 の外なので定数バッファのレイアウト 224 B は不変)。
	// true の Translucent / Additive サブセットは RenderResponsiveAAMask が Responsive AA マスク
	// (R8_UNORM) へ描き、TAA がその画素の現フレーム重みを 0.25 にする (動く半透明の残像を短くする)。
	// Material は FSlot へ値コピーされるのでプロキシへもそのまま渡る
	bool bEnableResponsiveAA = false;

	// ---- Material Domain (UMaterial::MaterialDomain) ----
	// CPU 専用 (b2 の外)。MD_Volume のスロットはサーフェス系のパス
	// (ベース / 半透明 / シャドウ / ベロシティ / Responsive AA / Lumen カード)
	// から除外され、FVolumetricFog のボクセル化パスが VolumeParams で評価する
	EMaterialDomain MaterialDomain = EMaterialDomain::MD_Surface;
	
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

	// ---- Responsive AA (UMaterial::bEnableResponsiveAA。Translucent / Additive のみ有効) ----
	bool ShouldEnableResponsiveAA() const { return bEnableResponsiveAA; }
	void SetEnableResponsiveAA(bool bEnable) { bEnableResponsiveAA = bEnable; }

	// ---- Material Domain (UMaterial::MaterialDomain / FMaterial::GetMaterialDomain) ----
	EMaterialDomain GetMaterialDomain() const { return MaterialDomain; }
	void            SetMaterialDomain(EMaterialDomain Domain) { MaterialDomain = Domain; }
	// FMaterial::IsVolumeDomain 相当。true のスロットはサーフェスのどのパスにも描かれない
	bool IsVolumeDomain() const { return MaterialDomain == EMaterialDomain::MD_Volume; }

	// ---- Volume ドメインのマテリアル入力 (b2 の外。ボクセル化パスがコピーする) ----
	FVolumeMaterialParameters VolumeParams;

	EVolumeShape GetVolumeShape() const { return (EVolumeShape)(unsigned int)(VolumeParams.ExtinctionParams.w + 0.5f); }
	void         SetVolumeShape(EVolumeShape Shape) { VolumeParams.ExtinctionParams.w = (float)(unsigned int)Shape; }

	void Bind(RenderManager* rm) const;
};

// ============================================================
//  FMaterialSlot
//  メッシュコンポーネントのマテリアルスロット (UStaticMeshComponent /
//  USkinnedMeshComponent 共通。FStaticMaterial / FSkeletalMaterial の
//  スロット + マテリアルインスタンスの値に相当)。
//  テクスチャは FAssetManager のキャッシュと共有する
//  (UTexture2D 相当。プロキシも同じ shared_ptr を保持するため、
//   実行時差し替え後も旧テクスチャは旧プロキシが生かし続ける)
// ============================================================
struct FMaterialSlot
{
	Material                 Mat;

	std::shared_ptr<TEXTURE> BaseColor;
	std::shared_ptr<TEXTURE> Normal;
	std::shared_ptr<TEXTURE> ARM;
};
