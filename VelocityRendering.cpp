#include "Main.h"
#include "VelocityRendering.h"

#include "RenderManager.h"
#include "SceneRenderer.h"
#include "SceneVelocityData.h"
#include "SceneViewState.h"
#include "Scene.h"
#include "PrimitiveSceneProxy.h"
#include "SceneRenderingUtils.h"

#include <algorithm>
#include <cmath>

// ============================================================
//  VelocityRendering : FSceneVelocityData / PrimitiveHasVelocityForView /
//  FSceneRenderer::RenderVelocities
// ============================================================

namespace
{
	// VelocityCommon.hlsl と同値
	constexpr float kVelocityEncodeScale = 0.499f * 0.5f;          // 0.2495
	constexpr float kVelocityEncodeBias  = 32767.0f / 65535.0f;    // 0.49999237
}


// ============================================================
//  FComponentVelocityData / FSceneVelocityData
// ============================================================
bool FComponentVelocityData::HasVelocity() const
{
	const float* a = &LocalToWorld._11;
	const float* b = &PreviousLocalToWorld._11;
	for (int i = 0; i < 16; ++i)
	{
		if (std::fabs(a[i] - b[i]) > 1.0e-4f)	// FMatrix::Equals(…, 0.0001)
		{
			return true;
		}
	}
	return false;
}

void FSceneVelocityData::StartFrame()
{
	++m_InternalFrameIndex;
	// 前フレームの描画値 -> Prev。このフレームにプッシュされなかったプリミティブは速度 0 になる
	for (auto& kv : m_ComponentData)
	{
		kv.second.PreviousLocalToWorld = kv.second.LocalToWorld;
	}
}

void FSceneVelocityData::Register(const UPrimitiveComponent* C, const XMFLOAT4X4& L2W)
{
	FComponentVelocityData& d = m_ComponentData[C];
	d.LocalToWorld = L2W;
	d.PreviousLocalToWorld = L2W;
	d.LastFrameUpdated = m_InternalFrameIndex;
	d.bTeleportPending = true;		// 登録直後の最初のプッシュはテレポート扱い (スポーン時の姿勢から速度を出さない)
}

void FSceneVelocityData::UpdateTransform(const UPrimitiveComponent* C, const XMFLOAT4X4& L2W)
{
	auto it = m_ComponentData.find(C);
	if (it == m_ComponentData.end())
	{
		Register(C, L2W);
		return;
	}
	it->second.LocalToWorld = L2W;
	it->second.LastFrameUpdated = m_InternalFrameIndex;
	if (it->second.bTeleportPending)
	{
		it->second.PreviousLocalToWorld = L2W;	// テレポート: 速度を出さない
	}
}

void FSceneVelocityData::MarkTeleported(const UPrimitiveComponent* C)
{
	auto it = m_ComponentData.find(C);
	if (it != m_ComponentData.end())
	{
		it->second.bTeleportPending = true;
	}
}

void FSceneVelocityData::EndFrameUpdates()
{
	for (auto& kv : m_ComponentData)
	{
		kv.second.bTeleportPending = false;
	}
}

void FSceneVelocityData::Remove(const UPrimitiveComponent* C)
{
	m_ComponentData.erase(C);
}

const FComponentVelocityData* FSceneVelocityData::Find(const UPrimitiveComponent* C) const
{
	auto it = m_ComponentData.find(C);
	return (it != m_ComponentData.end()) ? &it->second : nullptr;
}


// ============================================================
//  PrimitiveHasVelocityForView
// ============================================================
bool PrimitiveHasVelocityForView(const FViewInfo& View, const FPrimitiveSceneProxy& Proxy, bool bDisableSmallObjectCull)
{
	if (View.bCameraCut)
	{
		return false;	// カット時は速度を描かない (前フレーム情報は捨てる)
	}
	if (bDisableSmallObjectCull || kMotionBlurPerObjectSize <= 0.0f)
	{
		return true;
	}

	const FBoxSphereBounds& b = Proxy.GetBounds();
	const XMFLOAT3 o = View.ViewMatrices.ViewOrigin;
	const float dx = b.Origin.x - o.x, dy = b.Origin.y - o.y, dz = b.Origin.z - o.z;
	const float LODFactorDistanceSquared = dx * dx + dy * dy + dz * dz;	// LODDistanceFactor = 1
	const float MinR = kMotionBlurPerObjectSize * 2.0f / 100.0f;			// = 0.01
	// 画面上で小さ過ぎる物体はカメラモーションに任せる
	return b.SphereRadius * b.SphereRadius > MinR * MinR * LODFactorDistanceSquared;
}


// ============================================================
//  CPU 鏡像 (自己テスト T6)
// ============================================================
XMFLOAT2 EncodeVelocityToTextureCPU(XMFLOAT2 V)
{
	// ±2 クランプ (|V| > 2.0038 は UNORM 0 = 未書き込みと衝突する)
	const float x = std::clamp(V.x, -2.0f, 2.0f);
	const float y = std::clamp(V.y, -2.0f, 2.0f);
	return { x * kVelocityEncodeScale + kVelocityEncodeBias, y * kVelocityEncodeScale + kVelocityEncodeBias };
}

XMFLOAT2 DecodeVelocityFromTextureCPU(XMFLOAT2 E)
{
	return { (E.x - kVelocityEncodeBias) * (1.0f / kVelocityEncodeScale), (E.y - kVelocityEncodeBias) * (1.0f / kVelocityEncodeScale) };
}


// ============================================================
//  FSceneRenderer::RenderVelocities
//  GameManager::Draw が RenderBasePass の直後に呼ぶ。
//  深度はベースパス直後の DEPTH_WRITE (テストのみ、書き込まない)、ビューポートは R (既定)、
//  b0 はカメラ (念のため積み直す)。Velocity: RD -> RT, クリア (0,0,0,1), 描画, RT -> RD。
//  描いたフレームだけ m_bVelocityValid = true (BeginFrame 先頭で false に戻る)。
//  必要なのは TAA 有効時と、ベロシティを読むデバッグ表示 (1 / 2 / 4) / bForceVelocityPass の時だけ。
// ============================================================
void FSceneRenderer::RenderVelocities(FScene* Scene)
{
	m_TAAStats.NumVelocityDraws = 0;
	const bool bNeeded = m_ViewFamily.bTemporalAA || m_TAADebug.bForceVelocityPass
		|| m_TAADebug.DebugView == ETemporalAADebugView::MotionVectors || m_TAADebug.DebugView == ETemporalAADebugView::VelocityMask
		|| m_TAADebug.DebugView == ETemporalAADebugView::TemporalUpscalerIO;	// 可視化 1 / 2 / 4 は t35 を読む
	m_bVelocityValid = false;													// BeginFrame 先頭でもリセット済み (二重化)
	if (Scene == nullptr || !bNeeded || !m_ViewInfo.bValid)
	{
		return;																	// TAA / 可視化はこのフレーム ダミーを読む
	}

	ID3D12GraphicsCommandList* cl = m_RHI->GetGraphicsCommandList();
	RENDER_TARGET* vel = m_SceneTextures.Velocity.get();

	// b0 = カメラ (RenderBasePass と同じ値。ベースパス中のプリミティブが上書きしていても戻す)
	m_RHI->SetConstant(RenderManager::CONSTANT_TYPE::VIEW, &m_ViewConstant, sizeof(m_ViewConstant));

	TransitionReadToRenderTarget(cl, vel);										// RD -> RT
	const D3D12_CPU_DESCRIPTOR_HANDLE dsv = m_RHI->GetDepthStencilViewHandle();	// 深度はベースパス直後の DEPTH_WRITE (テストのみ)
	cl->OMSetRenderTargets(1, &vel->RTVHandle, FALSE, &dsv);
	const FLOAT clear[4] = { 0.0f, 0.0f, 0.0f, 1.0f };							// RG = 0 = 未書き込み。最適化クリア値 (0,0,0,1) と一致 (#820 回避)
	cl->ClearRenderTargetView(vel->RTVHandle, clear, 0, nullptr);

	if (!m_ViewInfo.bCameraCut)
	{
		const std::vector<FPrimitiveSceneInfo>& prims = Scene->GetPrimitives();
		const FSceneVelocityData& vd = Scene->GetVelocityData();
		const size_t count = (std::min)(prims.size(), m_PrimitiveVisibilityMap.size());
		for (size_t i = 0; i < count; ++i)
		{
			if (m_PrimitiveVisibilityMap[i] == 0)
			{
				continue;
			}
			const FPrimitiveSceneProxy* proxy = prims[i].Proxy.get();
			if (proxy == nullptr || !proxy->RendersVelocity() || !proxy->GetViewRelevance().HasOpaqueRelevance())
			{
				continue;	// 速度を書かないプリミティブ (カメラ追従の空ドーム等) / 不透明サブセット無し
			}
			const FComponentVelocityData* d = vd.Find(prims[i].Component);
			if (d == nullptr || !(d->HasVelocity() || proxy->AlwaysHasVelocity()))	// PrimitiveHasVelocityForFrame
			{
				continue;
			}
			if (!PrimitiveHasVelocityForView(m_ViewInfo, *proxy, m_TAADebug.bDisableVelocitySmallObjectCull))
			{
				continue;
			}
			proxy->DrawVelocity(m_RHI, d->PreviousLocalToWorld);
			++m_TAAStats.NumVelocityDraws;
		}
	}

	TransitionRenderTargetToRead(cl, vel);										// RT -> RD
	m_bVelocityValid = true;
}
