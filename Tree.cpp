#include "Main.h"
#include "Tree.h"
#include "AssetManager.h"
#include "FBXModel.h"

#include <filesystem>
#include <unordered_map>

namespace
{
	// 同じパラメータの木をキャッシュから取ったときに統計を復元するための表
	std::unordered_map<unsigned long long, FProceduralTreeStats> g_TreeStatsCache;

	// テクスチャファイルがあるときだけ設定する (生成失敗時に DDS ローダで落ちないように)
	template <class TSetter>
	void SetTextureIfExists(const std::string& Path, TSetter Setter)
	{
		if (std::filesystem::exists(Path))
		{
			Setter(Path.c_str());
		}
	}
}

ATree::ATree()
{
	// 再生成は Tick で行う (AStaticMeshActor は Tick 不要にしているので戻す)
	bCanEverTick = true;

	UStaticMeshComponent* mesh = GetStaticMeshComponent();
	mesh->SetNumMaterialSlots(2);

	// ---- テクスチャ (無ければ生成する。Saved/ProceduralTexture/*.dds) ----
	FProceduralTreeTextures::EnsureGenerated();

	SetTextureIfExists(FProceduralTreeTextures::GetBarkBaseColorPath(), [&](const char* p) { mesh->SetBaseColorTexture(BarkMaterialSlot, p); });
	SetTextureIfExists(FProceduralTreeTextures::GetBarkNormalPath(), [&](const char* p) { mesh->SetNormalTexture(BarkMaterialSlot, p); });
	SetTextureIfExists(FProceduralTreeTextures::GetBarkARMPath(), [&](const char* p) { mesh->SetARMTexture(BarkMaterialSlot, p); });

	SetTextureIfExists(FProceduralTreeTextures::GetLeafBaseColorPath(), [&](const char* p) { mesh->SetBaseColorTexture(LeafMaterialSlot, p); });
	SetTextureIfExists(FProceduralTreeTextures::GetFlatNormalPath(), [&](const char* p) { mesh->SetNormalTexture(LeafMaterialSlot, p); });
	SetTextureIfExists(FProceduralTreeTextures::GetLeafARMPath(), [&](const char* p) { mesh->SetARMTexture(LeafMaterialSlot, p); });

	// ---- 樹皮 (Lit / Opaque)。色はテクスチャが担うので BaseColor は白 ----
	{
		Material& bark = mesh->GetMaterial(BarkMaterialSlot);
		bark.Params.BaseColor = { 1.0f, 1.0f, 1.0f, 1.0f };
		bark.Params.EmissionColor = { 0.0f, 0.0f, 0.0f, 0.0f };
		bark.Params.Metallic = 0.0f;
		bark.Params.Specular = 0.3f;
		bark.Params.Roughness = 0.9f;
		bark.Params.NormalWeight = 1.0f;
		bark.Params.Unlit = FALSE;
		bark.SetBlendMode(EBlendMode::BLEND_Opaque);
		bark.SetTwoSided(false);
	}

	// ---- 葉 (Lit / Masked / Two Sided)。葉の形は BaseColor の α で clip ----
	{
		Material& leaf = mesh->GetMaterial(LeafMaterialSlot);
		leaf.Params.BaseColor = { 1.0f, 1.0f, 1.0f, 1.0f };
		leaf.Params.EmissionColor = { 0.0f, 0.0f, 0.0f, 0.0f };
		leaf.Params.Metallic = 0.0f;
		leaf.Params.Specular = 0.25f;
		leaf.Params.Roughness = 0.65f;
		leaf.Params.NormalWeight = 1.0f;
		leaf.Params.Unlit = FALSE;
		leaf.SetBlendMode(EBlendMode::BLEND_Masked);
		leaf.SetTwoSided(true);
	}

	// GetMaterial() 直接書き換えの反映 (次フレームでプロキシ再生成)
	mesh->MarkRenderStateDirty();
}

void ATree::Tick(float DeltaTime)
{
	(void)DeltaTime;

	// Details / INI からの変更はここでまとめて反映する (描画の外)
	if (m_bRebuildPending)
	{
		const bool bForce = m_bForceRebuild;
		m_bForceRebuild = false;
		RebuildTree(bForce);
	}
}

void ATree::SetTreeParams(const FProceduralTreeParams& Params)
{
	m_Params = Params;
	RequestRebuild();
}

void ATree::RebuildTree(bool bForce)
{
	m_bRebuildPending = false;

	m_Params.Sanitize();
	const unsigned long long hash = m_Params.ComputeHash();

	UStaticMeshComponent* mesh = GetStaticMeshComponent();
	const bool bHasMesh = mesh->GetStaticMesh() != nullptr && mesh->GetStaticMesh()->IsLoaded();
	if (!bForce && bHasMesh && hash == m_BuiltHash)
	{
		return;
	}

	char name[64];
	sprintf_s(name, "Tree_%016llx", hash);

	FAssetManager* assets = FAssetManager::GetInstance();

	// 同じパラメータの木が生きていれば共有し、無ければ生成する
	std::shared_ptr<FBXModel> model = (!bForce) ? assets->FindProceduralMesh(name) : nullptr;
	if (model)
	{
		auto it = g_TreeStatsCache.find(hash);
		if (it != g_TreeStatsCache.end())
		{
			m_Stats = it->second;
		}
	}
	else
	{
		FProceduralTreeStats stats;
		FProceduralMeshData data = FProceduralTreeBuilder::Build(m_Params, &stats);
		model = assets->CreateProceduralMesh(name, data);

		m_Stats = stats;
		g_TreeStatsCache[hash] = stats;
	}

	mesh->SetStaticMesh(model);	// MarkRenderStateDirty 込み (境界はプロキシ再生成時に CalcBounds で更新される)
	m_BuiltHash = hash;
}
