#pragma once
#include "PostProcessSettings.h"
#include "Actor.h"

// ============================================================
//  APostProcessVolume
//  APostProcessVolume ワールド内アクター。
//  アーティスト設定 (PP_SETTINGS) を保持し、BeginPlay で FScene に
//  自己登録する。レンダラは毎フレームこの設定を
//  FSceneRenderer::m_FinalSettings (FFinalPostProcessSettings 相当)
//  に解決してから使うため、レンダリング中の一時変更 (テクセル
//  サイズ / DofPad) がボリューム側に書き戻されることはない。
// ============================================================

class APostProcessVolume : public AActor
{
private:
	PP_SETTINGS m_Settings;

	// EV editing convenience (UI-side). Exposure = 2^EV.
	float m_EV = 0.0f;

public:
	// ---- パリティ用プロパティ ----
	// bUnbound=false の範囲ボリューム / BlendWeight による多段ブレンドは
	// あたり判定フェーズ (カメラ内包判定) で有効化する。
	bool  bEnabled = true;
	bool  bUnbound = true;
	float BlendWeight = 1.0f;

	void BeginPlay() override;             // FScene へ登録
	void EndPlay() override;               // FScene から登録解除
	void Tick(float DeltaTime) override;   // grain seed / EV -> Exposure

	PP_SETTINGS& Settings() { return m_Settings; }
	const PP_SETTINGS& Settings() const { return m_Settings; }

	float& EV() { return m_EV; }

	bool HasFlag(PP_FLAG f) const { return (m_Settings.Flags & f) != 0; }
	void SetFlag(PP_FLAG f, bool on)
	{
		if (on) m_Settings.Flags |= (unsigned int)f;
		else    m_Settings.Flags &= ~(unsigned int)f;
	}
};
