#include "Main.h"
#include "RenderManager.h"
#include "ScreenshotCapture.h"
#include "D3DX12.h"

#include <filesystem>
#include <cstdio>

// ============================================================
//  FScreenshotCapture : バックバッファ -> READBACK -> 24bit BMP
// ============================================================

FScreenshotCapture::FScreenshotCapture(RenderManager* RHI)
	: m_RHI(RHI)
{
}

FScreenshotCapture::~FScreenshotCapture()
{
	// 未書き出しのコピーは破棄する (終了時は GameManager デストラクタ / テストドライバが FlushScreenshots 済み)。
	// READBACK バッファは GPU 使用中の可能性があるので遅延削除キューへ回す。
	if (m_Readback && m_RHI)
	{
		m_RHI->DeferredRelease(std::move(m_Readback));
	}
}


void FScreenshotCapture::Request(const std::string& Path)
{
	if (Path.empty())
	{
		OutputDebugStringA("[Screenshot] Request: empty path ignored\n");
		return;
	}
	m_RequestedPath = Path;
	m_bRequested = true;
}


// ------------------------------------------------------------
//  READBACK バッファの確保 (初回 / バックバッファの形状が変わった時のみ)
// ------------------------------------------------------------
bool FScreenshotCapture::EnsureReadbackBuffer(const D3D12_RESOURCE_DESC& Desc)
{
	if (m_Readback &&
		m_SourceDesc.Width == Desc.Width && m_SourceDesc.Height == Desc.Height &&
		m_SourceDesc.Format == Desc.Format)
	{
		return true;
	}

	ID3D12Device* device = m_RHI->GetDevice();

	// 行ピッチは D3D12_TEXTURE_DATA_PITCH_ALIGNMENT (256B) に揃う
	// (1920 x RGBA8 = 7680B はそのまま 256 の倍数)
	D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
	UINT64 totalBytes = 0;
	device->GetCopyableFootprints(&Desc, 0, 1, 0, &footprint, nullptr, nullptr, &totalBytes);
	if (totalBytes == 0 || totalBytes == UINT64_MAX)
	{
		OutputDebugStringA("[Screenshot] GetCopyableFootprints failed\n");
		return false;
	}

	// 形状が変わった場合は旧バッファを遅延削除 (GPU が参照中の可能性)
	if (m_Readback)
	{
		m_RHI->DeferredRelease(std::move(m_Readback));
		m_Readback.Reset();
	}

	const CD3DX12_HEAP_PROPERTIES heap(D3D12_HEAP_TYPE_READBACK);
	const CD3DX12_RESOURCE_DESC   bufferDesc = CD3DX12_RESOURCE_DESC::Buffer(totalBytes);

	// READBACK ヒープのリソースは COPY_DEST で作成する (以後状態遷移しない)
	HRESULT hr = device->CreateCommittedResource(
		&heap, D3D12_HEAP_FLAG_NONE, &bufferDesc,
		D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&m_Readback));
	if (FAILED(hr))
	{
		OutputDebugStringA("[Screenshot] readback buffer creation failed\n");
		m_Readback.Reset();
		return false;
	}
	m_Readback->SetName(L"ScreenshotReadback");

	m_Footprint = footprint;
	m_TotalBytes = totalBytes;
	m_SourceDesc = Desc;
	return true;
}


// ------------------------------------------------------------
//  RecordCopy: RT -> COPY_SOURCE -> CopyTextureRegion -> RT
//  (RenderPostProcessing 末尾、UI 描画前のバックバッファ)
// ------------------------------------------------------------
void FScreenshotCapture::RecordCopy(ID3D12GraphicsCommandList* CommandList, ID3D12Resource* BackBuffer, DirectX::XMUINT2 Extent)
{
	if (!m_bRequested || CommandList == nullptr || BackBuffer == nullptr)
		return;

	m_bRequested = false;

	// 通常は BeginFrame 先頭で解決済み。同一フレーム内で 2 回記録されることはないが、
	// READBACK バッファは 1 本なので念のため先に書き出す。
	if (m_bPending)
	{
		ResolvePending();
	}

	const D3D12_RESOURCE_DESC desc = BackBuffer->GetDesc();
	if (!EnsureReadbackBuffer(desc))
	{
		++m_NumFailed;
		return;
	}

	const UINT width = (UINT)std::min<UINT64>(Extent.x, desc.Width);
	const UINT height = std::min<UINT>(Extent.y, desc.Height);
	if (width == 0 || height == 0)
	{
		OutputDebugStringA("[Screenshot] RecordCopy: empty extent\n");
		++m_NumFailed;
		return;
	}

	// バックバッファ: RENDER_TARGET -> COPY_SOURCE
	CommandList->ResourceBarrier(1,
		&CD3DX12_RESOURCE_BARRIER::Transition(BackBuffer,
			D3D12_RESOURCE_STATE_RENDER_TARGET,
			D3D12_RESOURCE_STATE_COPY_SOURCE));

	const CD3DX12_TEXTURE_COPY_LOCATION dst(m_Readback.Get(), m_Footprint);
	const CD3DX12_TEXTURE_COPY_LOCATION src(BackBuffer, 0);
	const D3D12_BOX box = { 0, 0, 0, width, height, 1 };
	CommandList->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);

	// バックバッファ: COPY_SOURCE -> RENDER_TARGET (以降のデバッグ表示 / ImGui 描画用)
	CommandList->ResourceBarrier(1,
		&CD3DX12_RESOURCE_BARRIER::Transition(BackBuffer,
			D3D12_RESOURCE_STATE_COPY_SOURCE,
			D3D12_RESOURCE_STATE_RENDER_TARGET));

	m_bPending = true;
	m_PendingPath = m_RequestedPath;
	m_PendingExtent = { width, height };
	m_PendingFormat = desc.Format;
}


// ------------------------------------------------------------
//  ResolvePending: WaitGPU -> Map -> BMP 書き出し
// ------------------------------------------------------------
void FScreenshotCapture::ResolvePending()
{
	if (!m_bPending)
		return;

	m_bPending = false;

	// コピーを含むコマンドリストは Present (または FlushAndResetCommandList) で
	// 実行済み。GPU 完了を待ってから読む (キャプチャ要求フレームのみのストール)。
	m_RHI->WaitGPU();

	bool bOK = false;
	void* mapped = nullptr;
	const D3D12_RANGE readRange = { 0, (SIZE_T)m_TotalBytes };
	if (m_Readback && SUCCEEDED(m_Readback->Map(0, &readRange, &mapped)) && mapped)
	{
		const unsigned char* pixels = static_cast<const unsigned char*>(mapped) + m_Footprint.Offset;
		bOK = WriteBMP(m_PendingPath, pixels, m_Footprint.Footprint.RowPitch,
			m_PendingExtent.x, m_PendingExtent.y, m_PendingFormat);

		const D3D12_RANGE writeRange = { 0, 0 };	// CPU は書き込まない
		m_Readback->Unmap(0, &writeRange);
	}

	char msg[600];
	if (bOK)
	{
		++m_NumWritten;
		sprintf_s(msg, "[Screenshot] wrote %s (%ux%u)\n", m_PendingPath.c_str(), m_PendingExtent.x, m_PendingExtent.y);
	}
	else
	{
		++m_NumFailed;
		sprintf_s(msg, "[Screenshot] FAILED to write %s\n", m_PendingPath.c_str());
	}
	OutputDebugStringA(msg);
}


// ------------------------------------------------------------
//  24bit ボトムアップ BMP (BITMAPFILEHEADER + BITMAPINFOHEADER, BI_RGB)
// ------------------------------------------------------------
bool FScreenshotCapture::WriteBMP(const std::string& Path, const unsigned char* Pixels, UINT RowPitch,
	UINT Width, UINT Height, DXGI_FORMAT Format)
{
	// バックバッファは R8G8B8A8_UNORM。BGRA 系も一応受け付ける
	bool bSwapRB = false;
	switch (Format)
	{
	case DXGI_FORMAT_R8G8B8A8_UNORM:
	case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
		bSwapRB = true;		// RGBA -> BMP の BGR
		break;
	case DXGI_FORMAT_B8G8R8A8_UNORM:
	case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
		bSwapRB = false;
		break;
	default:
		OutputDebugStringA("[Screenshot] unsupported back buffer format\n");
		return false;
	}

	const UINT stride = (Width * 3u + 3u) & ~3u;	// 行は 4B 境界
	const UINT imageSize = stride * Height;
	const UINT headerSize = 14u + 40u;
	const UINT fileSize = headerSize + imageSize;

	std::vector<unsigned char> file(fileSize, 0);
	auto put16 = [&](size_t o, uint16_t v) { file[o] = (unsigned char)(v & 0xFF); file[o + 1] = (unsigned char)(v >> 8); };
	auto put32 = [&](size_t o, uint32_t v) { for (int i = 0; i < 4; ++i) file[o + i] = (unsigned char)((v >> (8 * i)) & 0xFF); };

	// BITMAPFILEHEADER
	file[0] = 'B'; file[1] = 'M';
	put32(2, fileSize);
	put32(10, headerSize);			// ピクセルデータへのオフセット
	// BITMAPINFOHEADER
	put32(14, 40u);
	put32(18, Width);
	put32(22, Height);				// 正 = ボトムアップ
	put16(26, 1);					// planes
	put16(28, 24);					// bpp
	put32(30, 0);					// BI_RGB
	put32(34, imageSize);
	put32(38, 2835);				// 72 DPI
	put32(42, 2835);

	// ボトムアップ: ファイル先頭行 = 画像の最下行
	for (UINT y = 0; y < Height; ++y)
	{
		const unsigned char* src = Pixels + (size_t)(Height - 1u - y) * RowPitch;
		unsigned char* dst = file.data() + headerSize + (size_t)y * stride;
		for (UINT x = 0; x < Width; ++x)
		{
			const unsigned char c0 = src[x * 4 + 0];
			const unsigned char c1 = src[x * 4 + 1];
			const unsigned char c2 = src[x * 4 + 2];
			dst[x * 3 + 0] = bSwapRB ? c2 : c0;	// B
			dst[x * 3 + 1] = c1;					// G
			dst[x * 3 + 2] = bSwapRB ? c0 : c2;	// R
		}
	}

	// 出力先フォルダを作成 (Saved/Screenshots, -taaout)
	std::error_code ec;
	const std::filesystem::path path = std::filesystem::u8path(Path);	// Path は UTF-8 (-taaout の非 ANSI 文字を保つ)
	if (path.has_parent_path())
	{
		std::filesystem::create_directories(path.parent_path(), ec);
	}

	FILE* fp = nullptr;
	if (_wfopen_s(&fp, path.c_str(), L"wb") != 0 || fp == nullptr)
		return false;

	const size_t written = fwrite(file.data(), 1, file.size(), fp);
	const bool bCloseOK = (fclose(fp) == 0);
	return written == file.size() && bCloseOK;
}
