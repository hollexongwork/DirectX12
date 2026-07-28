#pragma once

// ============================================================
//  IBLBaker
//  RenderManager から分離。Compute Shader でキューブマップ・
//  irradiance・prefilter・BRDF LUT を起動時に一度だけベイクし、
//  Deferred パスでは 3 サンプルで参照する。
//
//  RenderManager への依存は public アクセサ経由（デバイス・
//  コマンドリスト・SRVヒープ確保・コマンドリスト即時フラッシュ）。
// ============================================================

class RenderManager;

class IBLBaker
{
private:
	struct IBL_CUBE
	{
		ComPtr<ID3D12Resource>      Resource;
		unsigned int                SRVIndex = 0;
		D3D12_GPU_DESCRIPTOR_HANDLE SRVHandle{};
	};

	RenderManager* m_Owner = nullptr;

	IBL_CUBE m_EnvCube;        // equirect から変換した環境キューブ(ミップ付)
	IBL_CUBE m_IrradianceCube; // 拡散irradiance
	IBL_CUBE m_PrefilterCube;  // roughness別specular

	ComPtr<ID3D12Resource>      m_BRDFLut;
	unsigned int                m_BRDFLutSRVIndex = 0;
	D3D12_GPU_DESCRIPTOR_HANDLE m_BRDFLutSRVHandle{};

	// compute 専用ルートシグネチャ & PSO
	ComPtr<ID3D12RootSignature> m_ComputeRootSignature;
	ComPtr<ID3D12PipelineState> m_PSOEquirectToCube;
	ComPtr<ID3D12PipelineState> m_PSODownsampleCube;
	ComPtr<ID3D12PipelineState> m_PSOIrradiance;
	ComPtr<ID3D12PipelineState> m_PSOPrefilter;
	ComPtr<ID3D12PipelineState> m_PSOBRDFLut;

	// --- 内部ヘルパ ---
	ID3D12Device* Device();
	ID3D12GraphicsCommandList* CommandList();

	unsigned int CreateCubeUAV(ID3D12Resource* res, unsigned int mip);
	unsigned int CreateCubeSRV(ID3D12Resource* res, unsigned int mipLevels);
	unsigned int CreateTex2DUAV(ID3D12Resource* res, DXGI_FORMAT fmt);
	unsigned int CreateTex2DSRV_RG(ID3D12Resource* res);
	ComPtr<ID3D12Resource>      CreateCubeResource(unsigned int size, unsigned int mips, DXGI_FORMAT fmt);
	ComPtr<ID3D12PipelineState> CreateComputePipeline(const char* csoFile);

public:
	explicit IBLBaker(RenderManager* owner);
	~IBLBaker();

	void Init();                          // ルートシグネチャ・PSO・リソース生成
	void Bake(ID3D12Resource* EquirectResource, unsigned int equirectSRVIndex); // 起動時に一度だけ全ベイク
	void BindTextures();                  // Deferred パスで t6,t7,t8 をバインド

	// 設定値（HLSL の PREFILTER_MAX_MIP と整合させること）
	static const unsigned int ENV_CUBE_SIZE = 512;
	static const unsigned int IRRADIANCE_SIZE = 32;
	static const unsigned int PREFILTER_SIZE = 128;
	static const unsigned int PREFILTER_MIP_COUNT = 5;   // mip0..4 -> HLSL PREFILTER_MAX_MIP=4
	static const unsigned int BRDF_LUT_SIZE = 512;
	static const unsigned int ENV_CUBE_MIP_COUNT = 10;  // 512 -> full chain (importance sampling用)


};