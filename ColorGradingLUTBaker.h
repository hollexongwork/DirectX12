#pragma once

// ============================================================
//  ColorGradingLUTBaker
//
//  Color-grading LUT pipeline. Bakes the grading chain
//  (white balance + Sat/Con/Gamma/Gain/Offset) into a 33x33x33
//  RGB volume texture via a compute shader, only when settings
//  change. The Tonemap pass then samples this LUT once per pixel
//  (trilinear), replacing the per-pixel grading math -- exactly
//  how collapses grading into a single LUT lookup.
//
//  Mirrors IBLBaker's dependency model: talks to RenderManager
//  through its public accessors (device, command list, descriptor
//  allocation, immediate flush).
// ============================================================

#include "PostProcessSettings.h"

class RenderManager;

class ColorGradingLUTBaker
{
private:
	struct GRADING_PARAMS
	{
		XMFLOAT4 ColorSaturation;
		XMFLOAT4 ColorContrast;
		XMFLOAT4 ColorGamma;
		XMFLOAT4 ColorGain;
		XMFLOAT4 ColorOffset;
		float    WhiteTemp;
		float    WhiteTint;
		unsigned int LUTSize;
		unsigned int Flags;

		// ---- artist LUT (CombineLUTs) ----
		float    ArtistLUTWeight;   // 0..1 blend
		float    ArtistLUTTileSize; // cube edge (16, 32, ...)
		float    ArtistLUTPixelsX;  // strip width
		float    ArtistLUTPixelsY;  // strip height
	};

	RenderManager* m_Owner = nullptr;

	// Artist 2D strip LUT (optional), loaded via RenderManager::LoadTexture.
	// Its SRVIndex already lives in the shared shader-visible heap.
	std::unique_ptr<struct TEXTURE> m_ArtistLUT;
	// 1x1 fallback so t0 is always bound even when no artist LUT is loaded
	// (the shader references ArtistLUT unconditionally).
	unsigned int                m_FallbackSRVIndex = 0;
	ComPtr<ID3D12Resource>      m_FallbackTex;
	float                       m_ArtistWeight = 1.0f;
	unsigned int                m_ArtistTileSize = 0;   // 16, 32, ...
	unsigned int                m_ArtistPixelsX = 0;
	unsigned int                m_ArtistPixelsY = 0;

	ComPtr<ID3D12Resource>      m_LUT;            // Texture3D 33^3 RGBA16F
	unsigned int                m_LUTUAVIndex = 0;
	unsigned int                m_LUTSRVIndex = 0;
	D3D12_GPU_DESCRIPTOR_HANDLE m_LUTSRVHandle{};

	ComPtr<ID3D12RootSignature> m_RootSignature;
	ComPtr<ID3D12PipelineState> m_PSO;

	// Upload buffer for GRADING_PARAMS (b0).
	ComPtr<ID3D12Resource>      m_ParamBuffer;
	void* m_ParamPtr = nullptr;

	bool          m_Dirty = true;      // bake on first frame
	bool          m_BakedOnce = false; // tracks LUT state (UAV vs SRV)
	GRADING_PARAMS m_LastParams{};     // to detect settings changes

	std::string m_ArtistLUTPath;

	ID3D12Device* Device();
	ID3D12GraphicsCommandList* CommandList();

	ComPtr<ID3D12PipelineState> CreateComputePipeline(const char* csoFile);
	bool ParamsChanged(const GRADING_PARAMS& p) const;
	void CreateFallbackSRV();    // 1x1 white 2D texture for t0 when no artist LUT

public:
	explicit ColorGradingLUTBaker(RenderManager* owner);
	~ColorGradingLUTBaker();

	void Init();                 // root sig / PSO / 3D LUT resource

	// Load an artist-authored 2D strip LUT (e.g. 256x16 -> 16^3 cube) to be
	// combined into the bake. Pass nullptr/empty to clear. Marks dirty.
	void LoadArtistLUT(const char* ddsFile);
	void ClearArtistLUT();
	bool  HasArtistLUT() const { return m_ArtistLUT != nullptr; }
	float& ArtistLUTWeight() { return m_ArtistWeight; }

	// Re-bake the LUT from the volume's current settings if dirty.
	// Cheap no-op when settings are unchanged.
	void UpdateIfDirty(const PP_SETTINGS& Settings);

	// Force a re-bake on the next UpdateIfDirty() (call when any
	// grading-related ImGui control changes).
	void MarkDirty() { m_Dirty = true; }

	// SRV index/handle of the baked LUT (Texture3D), for the tonemap pass.
	unsigned int                GetLUTSRVIndex()  const { return m_LUTSRVIndex; }
	D3D12_GPU_DESCRIPTOR_HANDLE GetLUTSRVHandle() const { return m_LUTSRVHandle; }

	static const unsigned int LUT_SIZE = 33;   // Standard neutral LUT size



	const std::string& GetArtistLUTPath() const { return m_ArtistLUTPath; }


};
