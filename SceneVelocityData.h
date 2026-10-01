#pragma once
#include <DirectXMath.h>
#include <unordered_map>
#include <cstdint>
#include <cstddef>

using namespace DirectX;

class UPrimitiveComponent;

// ============================================================
//  FSceneVelocityData (UE ScenePrivate.h の FSceneVelocityData / FComponentVelocityData)
//  プリミティブごとの「今フレーム描く LocalToWorld」と「前フレームに描いた LocalToWorld」。
//  FScene が所有し、キーはコンポーネント (プロキシ再生成を跨いで履歴が残る)。
//
//  1 フレームの流れ (FScene::UpdateAllPrimitiveSceneInfos, ゲーム側フェーズ):
//    StartFrame()           : 全エントリ Prev = Current (動かなかったプリミティブは速度 0 に戻る。
//                             プッシュの無いフレームでも必ず呼ぶ = 古い Prev が残る危険の解消)
//    UpdateTransform(C, M)  : SendRenderTransform の直後。Current = M
//                             (テレポート保留中なら Prev = M も = 速度を出さない)
//    EndFrameUpdates()      : テレポート保留フラグをすべて下ろす
//  レンダラ (FSceneRenderer::RenderVelocities) は Find で読むだけ。
//  実装は VelocityRendering.cpp。
// ============================================================

struct FComponentVelocityData
{
	XMFLOAT4X4 LocalToWorld;                         // 今フレーム描画値 (転置前)
	XMFLOAT4X4 PreviousLocalToWorld;                 // 前フレーム描画値 (転置前)
	uint64_t   LastFrameUpdated = 0;                 // UpdateTransform / Register したフレーム (内部番号)
	bool       bTeleportPending = true;              // 次の UpdateTransform で Prev = Current (UE bTeleport)

	// いずれかの要素で |L2W - Prev| > 1e-4 (UE FMatrix::Equals の既定許容誤差)
	bool HasVelocity() const;
};

class FSceneVelocityData
{
public:
	void StartFrame();                                                       // ++InternalFrameIndex; 全エントリ Prev = Current
	void Register(const UPrimitiveComponent* C, const XMFLOAT4X4& L2W);      // Prev = Current = L2W, bTeleportPending = true
	void UpdateTransform(const UPrimitiveComponent* C, const XMFLOAT4X4& L2W); // Current = L2W; 保留中なら Prev = L2W
	void MarkTeleported(const UPrimitiveComponent* C);                       // bTeleportPending = true (UE bTeleport / OverridePreviousTransform)
	void EndFrameUpdates();                                                  // 全 bTeleportPending = false
	void Remove(const UPrimitiveComponent* C);
	const FComponentVelocityData* Find(const UPrimitiveComponent* C) const;
	size_t Num() const { return m_ComponentData.size(); }

private:
	// キー = コンポーネント (プロキシ再生成を跨いで生存)
	std::unordered_map<const UPrimitiveComponent*, FComponentVelocityData> m_ComponentData;
	uint64_t m_InternalFrameIndex = 0;
};
