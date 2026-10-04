#pragma once
#include "RenderManager.h"

// ============================================================
//  FSystemTextures (LTCMat / LTCAmp)
//  レクトライトのスペキュラ (RectGGXApproxLTC, RectLightLTC.hlsl) が読む
//  Linearly Transformed Cosines (Heitz 2016) のテーブル。
//  LTC.cpp (Tools/LTCFit が生成した定数配列) を起動時に 64x64 の
//  テクスチャへ焼き、デファードライティング / 半透明パスに常駐バインドする:
//    t37 LTCMat : R16G16B16A16_FLOAT  正規化した逆行列の 4 成分 (r0c0, r2c0, r0c2, r2c2)
//    t38 LTCAmp : R16G16_FLOAT        x = 振幅 (BRDF の積分値), y = フレネル項
//  u = ラフネス, v = sqrt(1 - NoV)。
//
//  リソースは LoadTexture と同じ CPU 書き込み可能ヒープ (WriteToSubresource) で作り、
//  生成時に開いているグラフィックスコマンドリストで PIXEL_SHADER_RESOURCE へ遷移する。
// ============================================================
class FSystemTextures
{
	RenderManager*          m_RHI = nullptr;

	ComPtr<ID3D12Resource>  m_LTCMat;
	ComPtr<ID3D12Resource>  m_LTCAmp;
	unsigned int            m_LTCMatSRVIndex = 0;
	unsigned int            m_LTCAmpSRVIndex = 0;

	// Size x Size の 2D テクスチャを CPU データから作る (Format は 16 bit float 系のみ)
	ComPtr<ID3D12Resource> CreateTexture2D(unsigned int Size, DXGI_FORMAT Format, unsigned int BytesPerTexel,
		const void* Data, const wchar_t* Name);
	unsigned int CreateSRV(ID3D12Resource* Resource, DXGI_FORMAT Format);

public:
	explicit FSystemTextures(RenderManager* RHI);
	~FSystemTextures();

	void Init();

	// t37 / t38 を今のグラフィックスルートシグネチャへバインドする
	void BindTextures();

	unsigned int GetLTCMatSRVIndex() const { return m_LTCMatSRVIndex; }
	unsigned int GetLTCAmpSRVIndex() const { return m_LTCAmpSRVIndex; }
};
