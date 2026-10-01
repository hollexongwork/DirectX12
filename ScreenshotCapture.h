#pragma once
#include <string>
#include <d3d12.h>
#include <wrl/client.h>
#include <DirectXMath.h>

class RenderManager;

// ============================================================
//  FScreenshotCapture
//  バックバッファ (UI 描画前) -> READBACK バッファ -> 24bit BMP。
//  UE の FScreenshotRequest + ReadSurfaceData 相当の最小実装で、
//  TAA 検証用テストドライバ (FTemporalAATestDriver, -taatest) と
//  F9 キーのスクリーンショットが使う。
//
//  フレーム内の流れ:
//    Request(Path)           : 次の RenderPostProcessing でキャプチャする
//    RecordCopy (ポスプロ末尾): バックバッファ RT -> COPY_SOURCE ->
//                              CopyTextureRegion (READBACK, 256B アライン
//                              フットプリント) -> RT をコマンドリストへ記録
//    ResolvePending (次フレームの BeginFrame 先頭) /
//    FlushScreenshots (終了前) : WaitGPU -> Map -> BMP 書き出し
//
//  READBACK バッファは初回キャプチャ時に確保し、以後使い回す
//  (O = 1920x1080 RGBA8 で行ピッチ 7680B x 1080 行)。
//  BMP はボトムアップ 24bit (BGR, 行を 4B 境界へパディング)。
// ============================================================

class FScreenshotCapture
{
public:
	explicit FScreenshotCapture(RenderManager* RHI);
	~FScreenshotCapture();

	FScreenshotCapture(const FScreenshotCapture&) = delete;
	FScreenshotCapture& operator=(const FScreenshotCapture&) = delete;

	// 次の RecordCopy でキャプチャする BMP のパス (UTF-8。空は不可。自動パスは呼び出し側で解決)。
	// 同一フレームで複数回呼んだ場合は最後の要求が有効。
	void Request(const std::string& Path);
	bool IsRequested() const { return m_bRequested; }

	// バックバッファ (RENDER_TARGET 状態) のコピーを記録する。記録後も RENDER_TARGET 状態。
	// Extent はコピーする範囲 (= 出力解像度 O)。バックバッファより大きい場合は切り詰める。
	void RecordCopy(ID3D12GraphicsCommandList* CommandList, ID3D12Resource* BackBuffer, DirectX::XMUINT2 Extent);

	// 前フレームまでに記録したコピーがあれば WaitGPU して BMP を書き出す
	// (FSceneRenderer::BeginFrame 先頭。記録済みコマンドリストは Present で実行済み)。
	void ResolvePending();

	// 終了前: 記録済み (実行済み) のコピーを待って書き出す。内容は ResolvePending と同じ。
	void FlushScreenshots() { ResolvePending(); }

	// ---- 統計 (テストドライバの終了コード判定用) ----
	int GetNumWritten() const { return m_NumWritten; }
	int GetNumFailed() const { return m_NumFailed; }

private:
	RenderManager* m_RHI = nullptr;

	// ---- READBACK バッファ (初回キャプチャで確保) ----
	Microsoft::WRL::ComPtr<ID3D12Resource> m_Readback;
	D3D12_PLACED_SUBRESOURCE_FOOTPRINT m_Footprint{};	// バックバッファ 1 枚分のフットプリント
	UINT64   m_TotalBytes = 0;
	D3D12_RESOURCE_DESC m_SourceDesc{};					// フットプリント算出元 (変化したら再確保)

	// ---- 要求 / 記録済み ----
	bool        m_bRequested = false;
	std::string m_RequestedPath;

	bool              m_bPending = false;				// コピー記録済み・未書き出し
	std::string       m_PendingPath;
	DirectX::XMUINT2  m_PendingExtent{};
	DXGI_FORMAT       m_PendingFormat = DXGI_FORMAT_UNKNOWN;

	int         m_NumWritten = 0;
	int         m_NumFailed = 0;

	// Desc に合うフットプリントと READBACK バッファを用意する (必要時のみ再確保)
	bool EnsureReadbackBuffer(const D3D12_RESOURCE_DESC& Desc);

	// Map 済みピクセル (RGBA8 / BGRA8, 行ピッチ RowPitch) -> 24bit ボトムアップ BMP (Path は UTF-8)
	static bool WriteBMP(const std::string& Path, const unsigned char* Pixels, UINT RowPitch,
		UINT Width, UINT Height, DXGI_FORMAT Format);
};
