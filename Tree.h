#pragma once
#include "StaticMeshActor.h"
#include "ProceduralTree.h"

// ============================================================
//  ATree
//  AStaticMeshActor 派生のプロシージャルツリー。FProceduralTreeParams
//  から FProceduralTreeBuilder でメッシュを生成し、FAssetManager の
//  プロシージャルメッシュキャッシュ ("Tree_<パラメータハッシュ>") を
//  通して UStaticMeshComponent に載せる。同じパラメータの木は GPU
//  バッファ / SDF / BLAS を共有する。
//
//  マテリアルスロット (FProceduralTreeBuilder のセクション添字と同じ):
//    0 = 樹皮 (Opaque)。Saved/ProceduralTexture の生成テクスチャ
//    1 = 単葉 (Masked / Two Sided / Substrate TwoSidedWrap)。葉の形は BaseColor の α
//    2 = 葉の房 (1 と同じ設定 + 房アトラス)
//    3 = 小枝 (Opaque、小枝用の樹皮テクスチャ)
//
//  再生成はフレームの外 (Tick) で行う: Details / INI からの変更は
//  RequestRebuild() で予約され、次の Tick で RebuildTree() が走る
//  (メッシュ構築は SDF ベイク / BLAS 構築でコマンドリストを
//  フラッシュするため、描画中 (ImGui のコールバック内) には行わない)。
// ============================================================

class ATree : public AStaticMeshActor
{
private:
	FProceduralTreeParams m_Params;
	FProceduralTreeStats  m_Stats;
	unsigned long long    m_BuiltHash = 0;
	bool                  m_bRebuildPending = true;	// 初回は Tick で構築する
	bool                  m_bForceRebuild = false;

public:
	static constexpr unsigned int BarkMaterialSlot = FProceduralTreeBuilder::BarkMaterialIndex;
	static constexpr unsigned int LeafMaterialSlot = FProceduralTreeBuilder::LeafMaterialIndex;
	static constexpr unsigned int ClusterMaterialSlot = FProceduralTreeBuilder::ClusterMaterialIndex;
	static constexpr unsigned int TwigMaterialSlot = FProceduralTreeBuilder::TwigMaterialIndex;
	static constexpr unsigned int NumMaterialSlots = FProceduralTreeBuilder::NumSections;

	// コンストラクタのマテリアル既定値 (葉の Substrate TwoSidedWrap 等) を変えたら上げる。
	// INI の [Actor.N] Tree.MaterialPreset がこれより古ければ SettingsManager::ApplyActor が
	// 保存済みの C0.M*.* を適用した後に ApplyDefaultMaterials() で上書きする (キーが無い旧 INI は 0)。
	//   2 = 葉 / 房を Substrate TwoSidedWrap に (F0 0.03 / F90 1 / TransmittanceColor (0.32, 0.58, 0.13) / Roughness 0.45)
	//   3 = 葉の艶を抑える F0 0.02 / F90 0.35 / Roughness 0.65、逆光の色 TransmittanceColor (0.45, 0.70, 0.18)
	// パラメータ (Tree.*) には版が無いので、古い INI の Tree.* は保存した値のまま使われる
	// (新しい既定値は Details の Preset コンボ "Oak 5 m" か Reset Actor で採る)
	static constexpr unsigned int MaterialPresetVersion = 3;

	ATree();

	void Tick(float DeltaTime) override;

	// ---- パラメータ ----
	// 直接書き換えた場合は RequestRebuild() を呼ぶこと (次の Tick で再生成)
	FProceduralTreeParams&       GetTreeParams() { return m_Params; }
	const FProceduralTreeParams& GetTreeParams() const { return m_Params; }
	void SetTreeParams(const FProceduralTreeParams& Params);

	// 次の Tick で再生成する。bForce = true はパラメータが前回と同じでも作り直す
	void RequestRebuild(bool bForce = false) { m_bRebuildPending = true; m_bForceRebuild |= bForce; }
	bool IsRebuildPending() const { return m_bRebuildPending; }

	// 即時再生成 (パラメータのハッシュが前回と同じなら bForce = false では何もしない)。
	// 描画中には呼ばないこと
	void RebuildTree(bool bForce = false);

	// 直近の生成結果の統計 (Details 表示用)
	const FProceduralTreeStats& GetTreeStats() const { return m_Stats; }

	// ---- マテリアル ----
	// 4 スロットをコンストラクタの既定値に戻す (MarkRenderStateDirty 込み)。
	// テクスチャの割り当ては変えない
	void ApplyDefaultMaterials();

	// 直接書き換えた場合は GetStaticMeshComponent()->MarkRenderStateDirty() を呼ぶこと
	Material& GetBarkMaterial() { return GetStaticMeshComponent()->GetMaterial(BarkMaterialSlot); }
	Material& GetLeafMaterial() { return GetStaticMeshComponent()->GetMaterial(LeafMaterialSlot); }
	Material& GetClusterMaterial() { return GetStaticMeshComponent()->GetMaterial(ClusterMaterialSlot); }
	Material& GetTwigMaterial() { return GetStaticMeshComponent()->GetMaterial(TwigMaterialSlot); }
};
