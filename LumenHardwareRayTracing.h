#pragma once
#include "RenderManager.h"

// ============================================================
//  LumenHardwareRayTracing
//  Lumen の HWRT (DXR 1.1 インラインレイトレーシング) 用の
//  Top Level Acceleration Structure 管理。
//
//    - BLAS は FBXModel::Load が一度だけ構築する (FBXModel.h)
//    - TLAS は毎フレーム、Lumen オブジェクトスロットから再構築
//      (インスタンス数が少ないため PREFER_FAST_BUILD のフル
//       リビルド。InstanceID = Lumen スロット番号 = Surface Cache
//       採光のオブジェクトインデックス)
//    - トレースは各コンピュートパスの RT バリアント (SM 6.5,
//      RayQuery) がルート SRV t28 経由で参照する
//      (LumenTracingHardware.hlsl)
//
//  DXR 非対応環境 (RenderManager::IsRayTracingSupported = false)
//  では Init が何もせず、IsAvailable が false のまま SWRT
//  (メッシュ SDF + Global Distance Field) が使われる。
// ============================================================

class FLumenHardwareRayTracing
{
private:
	RenderManager* m_RHI = nullptr;

	// ---- インスタンスバッファ (アップロードヒープ, ダブルバッファ) ----
	ComPtr<ID3D12Resource>            m_InstanceBuffer[2];
	D3D12_RAYTRACING_INSTANCE_DESC* m_InstancePointer[2] = {};
	unsigned int                      m_Frame = 0;

	// ---- TLAS (毎フレームインプレース再構築) ----
	ComPtr<ID3D12Resource> m_TLAS;
	ComPtr<ID3D12Resource> m_TLASScratch;

	unsigned int m_MaxInstances = 0;
	unsigned int m_NumInstances = 0;	// 今フレームの有効インスタンス数
	bool         m_bAvailable = false;	// Init 成功 (= DXR 対応環境)

public:
	explicit FLumenHardwareRayTracing(RenderManager* RHI);
	~FLumenHardwareRayTracing();

	// MaxInstances 分の TLAS / スクラッチ / インスタンスバッファを確保。
	// DXR 非対応環境では何もしない。
	void Init(unsigned int MaxInstances);

	// 今フレームのインスタンス書き込み先 (MaxInstances 分) を返す。
	// フリップするので毎フレーム 1 回だけ呼ぶこと。
	// 利用不可なら nullptr。
	D3D12_RAYTRACING_INSTANCE_DESC* BeginInstances();

	// TLAS 再構築を記録する (NumInstances = 0 なら何もしない =
	// HasTLAS が false になり SWRT へフォールバック)。
	void BuildTLAS(unsigned int NumInstances);

	bool IsAvailable() const { return m_bAvailable; }
	bool HasTLAS() const { return m_bAvailable && m_NumInstances > 0; }
	unsigned int GetNumInstances() const { return m_NumInstances; }

	D3D12_GPU_VIRTUAL_ADDRESS GetTLASAddress() const
	{
		return m_TLAS ? m_TLAS->GetGPUVirtualAddress() : 0;
	}
};
