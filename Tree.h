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
//  マテリアルスロット:
//    0 = 樹皮 (Opaque)。Saved/ProceduralTexture の生成テクスチャ
//    1 = 葉   (Masked / Two Sided)。葉カードの形は BaseColor の α
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

	// マテリアル。直接書き換えた場合は
	// GetStaticMeshComponent()->MarkRenderStateDirty() を呼ぶこと
	Material& GetBarkMaterial() { return GetStaticMeshComponent()->GetMaterial(BarkMaterialSlot); }
	Material& GetLeafMaterial() { return GetStaticMeshComponent()->GetMaterial(LeafMaterialSlot); }
};
