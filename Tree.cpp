#include "Main.h"
#include "Tree.h"
#include "AssetManager.h"
#include "FBXModel.h"

#include <chrono>
#include <filesystem>
#include <unordered_map>

namespace
{
	// 同じパラメータの木をキャッシュから取ったときに統計を復元するための表
	std::unordered_map<unsigned long long, FProceduralTreeStats> g_TreeStatsCache;

	// 候補パスのうち最初に存在するものを返す (無ければ空文字)
	std::string FirstExisting(std::initializer_list<std::string> Paths)
	{
		for (const std::string& p : Paths)
		{
			if (!p.empty() && std::filesystem::exists(p))
			{
				return p;
			}
		}
		return std::string();
	}
}

ATree::ATree()
{
	// 再生成は Tick で行う (AStaticMeshActor は Tick 不要にしているので戻す)
	bCanEverTick = true;

	UStaticMeshComponent* mesh = GetStaticMeshComponent();
	mesh->SetNumMaterialSlots(NumMaterialSlots);

	// ---- テクスチャ (無ければ生成する。Saved/ProceduralTexture/*.dds) ----
	FProceduralTreeTextures::EnsureGenerated();

	// 全スロットに 3 枚 (BaseColor / Normal / ARM) を必ず割り当てる。
	// FStaticMeshSceneProxy::BindSlot は未設定のテクスチャを「直前のバインドのまま」にするので、
	// 生成に失敗したファイルは同系統のもの (房 -> 葉、小枝 -> 樹皮、法線 -> 平坦) で代替する
	using T = FProceduralTreeTextures;
	const std::string flat = FirstExisting({ T::GetFlatNormalPath() });

	const std::string barkDiff = FirstExisting({ T::GetBarkBaseColorPath() });
	const std::string barkNor = FirstExisting({ T::GetBarkNormalPath(), flat });
	const std::string barkArm = FirstExisting({ T::GetBarkARMPath() });

	const std::string leafDiff = FirstExisting({ T::GetLeafBaseColorPath() });
	const std::string leafNor = FirstExisting({ T::GetLeafNormalPath(), flat });
	const std::string leafArm = FirstExisting({ T::GetLeafARMPath() });

	const std::string clusterDiff = FirstExisting({ T::GetClusterBaseColorPath(), leafDiff });
	const std::string clusterNor = FirstExisting({ T::GetClusterNormalPath(), leafNor, flat });
	const std::string clusterArm = FirstExisting({ T::GetClusterARMPath(), leafArm });

	const std::string twigDiff = FirstExisting({ T::GetTwigBaseColorPath(), barkDiff });
	const std::string twigNor = FirstExisting({ T::GetTwigNormalPath(), barkNor, flat });
	const std::string twigArm = FirstExisting({ T::GetTwigARMPath(), barkArm });

	auto Bind = [&](unsigned int Slot, const std::string& Diff, const std::string& Nor, const std::string& Arm)
		{
			if (!Diff.empty()) mesh->SetBaseColorTexture(Slot, Diff.c_str());
			if (!Nor.empty())  mesh->SetNormalTexture(Slot, Nor.c_str());
			if (!Arm.empty())  mesh->SetARMTexture(Slot, Arm.c_str());
		};
	Bind(BarkMaterialSlot, barkDiff, barkNor, barkArm);
	Bind(LeafMaterialSlot, leafDiff, leafNor, leafArm);
	Bind(ClusterMaterialSlot, clusterDiff, clusterNor, clusterArm);
	Bind(TwigMaterialSlot, twigDiff, twigNor, twigArm);

	ApplyDefaultMaterials();
}

void ATree::ApplyDefaultMaterials()
{
	UStaticMeshComponent* mesh = GetStaticMeshComponent();
	if (mesh->GetNumMaterialSlots() < NumMaterialSlots)
	{
		mesh->SetNumMaterialSlots(NumMaterialSlots);
	}

	// ---- 樹皮 / 小枝 (Lit / Opaque)。色はテクスチャが担うので BaseColor は白 ----
	auto SetBark = [](Material& m)
		{
			m = Material();
			m.Params.BaseColor = { 1.0f, 1.0f, 1.0f, 1.0f };
			m.Params.EmissionColor = { 0.0f, 0.0f, 0.0f, 0.0f };
			m.Params.Metallic = 0.0f;
			m.Params.Specular = 0.3f;
			m.Params.Roughness = 0.85f;		// ARM の G が 0 のときのフォールバック
			m.Params.NormalWeight = 1.0f;
			m.Params.Unlit = FALSE;
			m.SetBlendMode(EBlendMode::BLEND_Opaque);
			m.SetTwoSided(false);
			m.SetUseSubstrate(false);
		};
	SetBark(mesh->GetMaterial(BarkMaterialSlot));
	SetBark(mesh->GetMaterial(TwigMaterialSlot));

	// ---- 葉 / 房 (Masked / Two Sided / Substrate TwoSidedWrap) ----
	// 表はラップ拡散、裏は透過 (TransmittanceColor がそのまま逆光の葉の色になる。
	// 透過項はテクスチャで色付かないので線形の葉色を直接入れる)。
	// SubstrateDiffuseAlbedo はテクスチャ x 頂点カラーに乗算されるので白 (既定 0.18 だと暗くなる)
	auto SetLeaf = [](Material& m)
		{
			m = Material();
			m.Params.BaseColor = { 1.0f, 1.0f, 1.0f, 1.0f };
			m.Params.EmissionColor = { 0.0f, 0.0f, 0.0f, 0.0f };
			m.Params.Metallic = 0.0f;
			m.Params.Specular = 0.15f;
			m.Params.Roughness = 0.65f;		// ARM の G が 0 のときのフォールバック (葉のクチクラは粗め)
			m.Params.NormalWeight = 1.0f;
			m.Params.Unlit = FALSE;
			m.Params.OpacityMaskClipValue = FProceduralTreeTextures::LeafCoverageClip;
			m.SetBlendMode(EBlendMode::BLEND_Masked);
			m.SetTwoSided(true);

			m.SetUseSubstrate(true);
			m.SetSubstrateSSSType(ESubstrateSSSType::TwoSidedWrap);
			m.Params.SubstrateDiffuseAlbedo = { 1.0f, 1.0f, 1.0f, 1.0f };
			m.Params.SubstrateF0 = { 0.02f, 0.02f, 0.02f, 0.0f };	// 葉の艶は控えめに (裏面の銀色の光沢を抑える)
			m.Params.SubstrateF90 = { 0.35f, 0.35f, 0.35f, 0.0f };	// 粗い蝋質の葉はグレージングでも鏡面にならない (斜めのカードが空を映して灰色に光るのを防ぐ)
			m.Params.SubstrateTransmittanceColor = { 0.45f, 0.70f, 0.18f, 0.0f };	// 線形。exp(-tau) == T (Thickness 1cm)
			m.Params.SubstrateThickness = 1.0f;										// [cm]
			m.Params.SubstrateSSSPhaseAnisotropy = 0.35f;							// 前方散乱 (逆光で抜ける)
			m.Params.SubstrateIsThin = TRUE;
			m.Params.SubstrateFuzzColor = { 0.35f, 0.40f, 0.22f, 0.0f };			// w = FuzzAmount 0 (必要なら艶に使う)
			m.Params.SubstrateFuzzRoughness = 0.6f;
		};
	SetLeaf(mesh->GetMaterial(LeafMaterialSlot));
	SetLeaf(mesh->GetMaterial(ClusterMaterialSlot));

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

	const auto t0 = std::chrono::steady_clock::now();

	char name[64];
	sprintf_s(name, "Tree_%016llx", hash);

	FAssetManager* assets = FAssetManager::GetInstance();

	// 同じパラメータの木が生きていれば共有し、無ければ生成する
	std::shared_ptr<FBXModel> model = (!bForce) ? assets->FindProceduralMesh(name) : nullptr;
	bool bFromCache = false;
	if (model)
	{
		bFromCache = true;
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
		model = assets->CreateProceduralMesh(name, data);	// GPU バッファ + SDF ベイク + BLAS

		m_Stats = stats;
	}

	mesh->SetStaticMesh(model);	// MarkRenderStateDirty 込み (境界はプロキシ再生成時に CalcBounds で更新される)
	m_BuiltHash = hash;

	// 再生成全体の所要時間 (キャッシュから取ったときは 0)
	m_Stats.RebuildMs = bFromCache ? 0.0f
		: std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - t0).count();
	if (!bFromCache)
	{
		g_TreeStatsCache[hash] = m_Stats;
	}
}
