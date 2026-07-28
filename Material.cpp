#include "Main.h"
#include "RenderManager.h"
#include "Material.h"

Material::Material()
{
	Params.BaseColor     = { 1.0f, 1.0f, 1.0f, 1.0f };
	Params.EmissionColor = { 0.0f, 0.0f, 0.0f, 0.0f };
	Params.Metallic      = 0.0f;
	Params.Specular      = 0.0f;
	Params.Roughness     = 1.0f;
	Params.NormalWeight  = 0.0f;
	Params.Unlit		 = FALSE;

	// ---- Blend Mode / Two Sided (UMaterial 既定値) ----
	Params.Opacity              = 1.0f;
	Params.OpacityMaskClipValue = 0.3333f;	// UE5 既定
	Params.BlendMode            = EBlendMode::BLEND_Opaque;
	Params.TwoSided             = FALSE;
	Params._pad                 = { 0.0f, 0.0f, 0.0f };

	// ---- Substrate Slab BSDF (UE5.8 既定値) ----
	// DiffuseAlbedo 0.18 (18% グレー) / F0 0.04 (誘電体 4%) /
	// F90 白 / Thickness 0.01cm (SUBSTRATE_LAYER_DEFAULT_THICKNESS_CM)
	Params.SubstrateDiffuseAlbedo      = { 0.18f, 0.18f, 0.18f, 1.0f };
	Params.SubstrateF0                 = { 0.04f, 0.04f, 0.04f, 1.0f };
	Params.SubstrateF90                = { 1.0f, 1.0f, 1.0f, 1.0f };
	Params.SubstrateTransmittanceColor = { 0.5f, 0.5f, 0.5f, 1.0f }; // w = SSSMFPScale
	Params.SubstrateFuzzColor          = { 1.0f, 1.0f, 1.0f, 0.0f }; // w = FuzzAmount

	Params.SubstrateAnisotropy         = 0.0f;
	Params.SubstrateSSSPhaseAnisotropy = 0.0f;
	// 参照厚 1cm と同値 -> 既定で τ = -log(T) となり
	// Transmittance Color がそのまま実現される (v6 以前と同じ見た目)
	Params.SubstrateThickness          = 1.0f; // [cm]
	Params.SubstrateSSSType            = ESubstrateSSSType::None;

	Params.SubstrateSecondRoughness       = 0.5f;
	Params.SubstrateSecondRoughnessWeight = 0.0f;
	Params.SubstrateFuzzRoughness         = 0.5f;
	Params.SubstrateIsThin                = FALSE;

	Params.bUseSubstrate       = FALSE;
	Params.RefractionMethod    = ERefractionMethod::None;
	Params.RefractionData      = { 1.5f, 0.0f }; // 既定 IOR 1.5 (ガラス)
	Params.RefractionDepthBias = 0.0f;
	Params.bRefractionUseF0    = FALSE;
	Params._padSubstrate       = { 0.0f, 0.0f };
}

void Material::Bind(RenderManager* rm) const
{
	MATERIAL_CONSTANT constant{};
	constant.Material = Params;
	rm->SetConstant(RenderManager::CONSTANT_TYPE::MATERIAL, &constant, sizeof(constant));
}
