#include "Main.h"
#include "TemporalAATestDriver.h"

#include "RenderManager.h"
#include "SceneRenderer.h"
#include "ScreenshotCapture.h"
#include "World.h"
#include "Camera.h"
#include "StaticMeshActor.h"
#include "PostProcessVolume.h"
#include "SettingsManager.h"
#include "AutoExposure.h"
#include "Material.h"
#include "Time.h"
#include "AntiAliasingSettings.h"
#include "ScreenPercentage.h"

#include "ImGUI/imgui.h"

#include <filesystem>
#include <cwchar>
#include <cerrno>
#include <climits>
#include <cmath>

// ============================================================
//  FTemporalAATestDriver : -taatest シナリオドライバ
// ============================================================

namespace
{
	// resize シナリオ (§9.3): { 開始フレーム, ScreenPercentage }。フレーム 1 から順に適用する
	struct FResizeStep { int Frame; float ScreenPercentage; };
	const FResizeStep kResizeSchedule[] =
	{
		{   1, 100.0f },
		{  30,  50.0f },
		{  60,  71.0f },
		{  90, 150.0f },
		{ 120, 200.0f },
		{ 150, 100.0f },
	};

	// ---- FAntiAliasingParams の上書きキー (§9.3) ----
	struct FIntOverrideKey   { const wchar_t* Key; int   FAntiAliasingParams::* Field; };
	struct FFloatOverrideKey { const wchar_t* Key; float FAntiAliasingParams::* Field; };
	struct FBoolOverrideKey  { const wchar_t* Key; bool  FAntiAliasingParams::* Field; };

	const FIntOverrideKey kIntOverrideKeys[] =
	{
		{ L"-aa",       &FAntiAliasingParams::AntiAliasingMethod },
		{ L"-taaq",     &FAntiAliasingParams::TemporalAAQuality },
		{ L"-samples",  &FAntiAliasingParams::TemporalAASamples },				// -forcejitter のジッタ列にも効く
		{ L"-upscaleq", &FAntiAliasingParams::UpscaleQuality },
		{ L"-merge",    &FAntiAliasingParams::TonemapperMergeWithUpscaleMode },
	};
	const FFloatOverrideKey kFloatOverrideKeys[] =
	{
		{ L"-sp",        &FAntiAliasingParams::ScreenPercentage },
		{ L"-cfw",       &FAntiAliasingParams::TemporalAACurrentFrameWeight },
		{ L"-hsp",       &FAntiAliasingParams::TemporalAAHistoryScreenPercentage },
		{ L"-mipoffset", &FAntiAliasingParams::ViewTextureMipBiasOffset },
		{ L"-mipmin",    &FAntiAliasingParams::ViewTextureMipBiasMin },
	};
	const FBoolOverrideKey kBoolOverrideKeys[] =
	{
		{ L"-upsampling", &FAntiAliasingParams::bTemporalAAUpsampling },
		{ L"-catmullrom", &FAntiAliasingParams::bTemporalAACatmullRom },
		{ L"-r11",        &FAntiAliasingParams::bTemporalAAR11G11B10History },
		{ L"-allowdown",  &FAntiAliasingParams::bTemporalAAAllowDownsampling },
	};

	// 空白区切り。ダブルクォートで囲んだ部分は空白を含められる (クォート自体は除去)
	std::vector<std::wstring> TokenizeCommandLine(const wchar_t* CmdLine)
	{
		std::vector<std::wstring> tokens;
		if (CmdLine == nullptr)
			return tokens;

		std::wstring current;
		bool bInQuote = false;
		bool bHasToken = false;
		for (const wchar_t* p = CmdLine; *p; ++p)
		{
			const wchar_t c = *p;
			if (c == L'"')
			{
				bInQuote = !bInQuote;
				bHasToken = true;
				continue;
			}
			if (!bInQuote && (c == L' ' || c == L'\t' || c == L'\r' || c == L'\n'))
			{
				if (bHasToken)
				{
					tokens.push_back(current);
					current.clear();
					bHasToken = false;
				}
				continue;
			}
			current += c;
			bHasToken = true;
		}
		if (bHasToken)
			tokens.push_back(current);
		return tokens;
	}

	// パス / メッセージ用 (std::filesystem の狭い文字列と同じ ANSI コードページ)
	std::string ToNarrow(const std::wstring& Wide)
	{
		if (Wide.empty())
			return std::string();
		const int size = WideCharToMultiByte(CP_ACP, 0, Wide.c_str(), (int)Wide.size(), nullptr, 0, nullptr, nullptr);
		std::string out((size_t)(size > 0 ? size : 0), '\0');
		if (size > 0)
			WideCharToMultiByte(CP_ACP, 0, Wide.c_str(), (int)Wide.size(), out.data(), size, nullptr, nullptr);
		return out;
	}

	bool ParseInt(const std::wstring& Text, int& Out)
	{
		if (Text.empty())
			return false;
		wchar_t* end = nullptr;
		errno = 0;
		const long v = wcstol(Text.c_str(), &end, 10);
		if (end == Text.c_str() || *end != L'\0' || errno != 0 || v < INT_MIN || v > INT_MAX)
			return false;
		Out = (int)v;
		return true;
	}

	bool ParseFloat(const std::wstring& Text, float& Out)
	{
		if (Text.empty())
			return false;
		wchar_t* end = nullptr;
		errno = 0;
		const float v = wcstof(Text.c_str(), &end);
		if (end == Text.c_str() || *end != L'\0' || errno != 0 || !std::isfinite(v))
			return false;
		Out = v;
		return true;
	}

	bool ParseBool01(const std::wstring& Text, bool& Out)
	{
		if (Text == L"0") { Out = false; return true; }
		if (Text == L"1") { Out = true;  return true; }
		return false;
	}

	template <class T>
	T* FindActorByIndex(UWorld& World, int Index)
	{
		const auto& actors = World.GetActors();
		if (Index < 0 || Index >= (int)actors.size())
			return nullptr;
		return dynamic_cast<T*>(actors[(size_t)Index].get());
	}
}


bool FTemporalAATestDriver::IsTestCommandLine(const wchar_t* CmdLine)
{
	for (const std::wstring& token : TokenizeCommandLine(CmdLine))
	{
		if (token.substr(0, token.find(L'=')) == L"-taatest")
			return true;
	}
	return false;
}


FTemporalAATestDriver::FTemporalAATestDriver(const wchar_t* CmdLine)
{
	ParseCommandLine(CmdLine);
}

FTemporalAATestDriver::~FTemporalAATestDriver()
{
	if (m_Log)
	{
		fclose(m_Log);
		m_Log = nullptr;
	}
}


int FTemporalAATestDriver::GetExitCode() const
{
	// 最終フレーム前に閉じられた (WM_QUIT) テストはキャプチャ未完了として失敗扱い
	if (m_bActive && !m_bFinished && m_ExitCode == ExitSuccess)
		return ExitCaptureWriteFailed;
	return m_ExitCode;
}


void FTemporalAATestDriver::Message(const std::string& Text)
{
	const std::string line = "[TAA Test] " + Text + "\n";
	OutputDebugStringA(line.c_str());

	if (m_Log)
	{
		fprintf(m_Log, "# %s\n", Text.c_str());
		fflush(m_Log);
	}
	else
	{
		m_PendingMessages.push_back(Text);
	}
}

void FTemporalAATestDriver::CommandLineError(const std::string& Text)
{
	m_bCommandLineError = true;
	Message("command line error: " + Text);
}

void FTemporalAATestDriver::SetFailure(int ExitCode)
{
	// 最初に確定した失敗コードを優先する
	if (m_ExitCode == ExitSuccess)
		m_ExitCode = ExitCode;
}

void FTemporalAATestDriver::RequestExit(int ExitCode)
{
	SetFailure(ExitCode);
	m_bExitRequested = true;
}


// ------------------------------------------------------------
//  FAntiAliasingParams の上書きキー (-aa / -sp / ...)
//  値を検証して m_AAOverrides に積む (適用は OnBegin: INI 適用の後)。
//  範囲外の値は OnBegin で SanitizeAntiAliasingParams が §7.1 の範囲へ丸める
// ------------------------------------------------------------
bool FTemporalAATestDriver::ParseAntiAliasingOverride(const std::wstring& Key, const std::wstring& Value)
{
	const std::string keyA = ToNarrow(Key);
	const std::string text = keyA + "=" + ToNarrow(Value);

	for (const FIntOverrideKey& k : kIntOverrideKeys)
	{
		if (Key != k.Key)
			continue;
		int v = 0;
		if (!ParseInt(Value, v))
		{
			CommandLineError(keyA + " needs an integer");
			return true;
		}
		int FAntiAliasingParams::* field = k.Field;
		m_AAOverrides.push_back([field, v](FAntiAliasingParams& p) { p.*field = v; });
		Message(text + " (not saved)");
		return true;
	}
	for (const FFloatOverrideKey& k : kFloatOverrideKeys)
	{
		if (Key != k.Key)
			continue;
		float v = 0.0f;
		if (!ParseFloat(Value, v))
		{
			CommandLineError(keyA + " needs a finite number");
			return true;
		}
		float FAntiAliasingParams::* field = k.Field;
		m_AAOverrides.push_back([field, v](FAntiAliasingParams& p) { p.*field = v; });
		Message(text + " (not saved)");
		return true;
	}
	for (const FBoolOverrideKey& k : kBoolOverrideKeys)
	{
		if (Key != k.Key)
			continue;
		bool v = false;
		if (!ParseBool01(Value, v))
		{
			CommandLineError(keyA + " needs 0 or 1");
			return true;
		}
		bool FAntiAliasingParams::* field = k.Field;
		m_AAOverrides.push_back([field, v](FAntiAliasingParams& p) { p.*field = v; });
		Message(text + " (not saved)");
		return true;
	}
	return false;
}


// ------------------------------------------------------------
//  FTemporalAADebugSettings の上書きキー (-forcejitter / -nojitter / -overrideindex / -debugvis /
//  -visscale / -ftwmode)。値を検証して m_DebugOverrides に積む (適用は OnBegin)。非永続の設定なので保存とは無関係
// ------------------------------------------------------------
bool FTemporalAATestDriver::ParseDebugOverride(const std::wstring& Key, const std::wstring& Value)
{
	const std::string keyA = ToNarrow(Key);
	const std::string text = keyA + "=" + ToNarrow(Value);

	if (Key == L"-forcejitter" || Key == L"-nojitter")
	{
		bool v = false;
		if (!ParseBool01(Value, v))
		{
			CommandLineError(keyA + " needs 0 or 1");
			return true;
		}
		if (Key == L"-forcejitter")
			m_DebugOverrides.push_back([v](FTemporalAADebugSettings& d) { d.bForceJitterWithoutTAA = v; });	// TAA 無効でもジッタ (比較用)
		else
			m_DebugOverrides.push_back([v](FTemporalAADebugSettings& d) { d.bDisableJitter = v; });		// ジッタ 0 (比較用)
		Message(text + " (debug, not saved)");
		return true;
	}
	if (Key == L"-overrideindex")
	{
		// OverrideTemporalIndex: -1 = 無効, >= 0 でジッタ添字を固定 (N で剰余)
		int v = 0;
		if (!ParseInt(Value, v) || v < -1)
		{
			CommandLineError(keyA + " needs an integer >= -1");
			return true;
		}
		m_DebugOverrides.push_back([v](FTemporalAADebugSettings& d) { d.OverrideTemporalIndex = v; });
		Message(text + " (debug, not saved)");
		return true;
	}
	if (Key == L"-debugvis")
	{
		// ETemporalAADebugView (0 = Off .. 13)
		int v = 0;
		if (!ParseInt(Value, v) || v < 0 || v >= (int)ETemporalAADebugView::Count)
		{
			CommandLineError(keyA + " needs an integer in [0, " + std::to_string((int)ETemporalAADebugView::Count - 1) + "]");
			return true;
		}
		const ETemporalAADebugView view = (ETemporalAADebugView)v;
		m_DebugOverrides.push_back([view](FTemporalAADebugSettings& d) { d.DebugView = view; });
		Message(text + " (debug, not saved)");
		return true;
	}
	if (Key == L"-visscale")
	{
		// VisualizeScale: ImGui スライダーと同じ [0.1, 64] へ丸める
		float v = 0.0f;
		if (!ParseFloat(Value, v))
		{
			CommandLineError(keyA + " needs a finite number");
			return true;
		}
		v = std::clamp(v, AntiAliasingRanges::kVisualizeScaleMin, AntiAliasingRanges::kVisualizeScaleMax);
		m_DebugOverrides.push_back([v](FTemporalAADebugSettings& d) { d.VisualizeScale = v; });
		Message(text + " (debug, not saved)");
		return true;
	}
	if (Key == L"-ftwmode")
	{
		// FilteredTemporalWeight の定義 (MainUpsampling / MainSuperSampling のみ効く。ImGui "Sum / Nearest / One")
		int v = 0;
		if (!ParseInt(Value, v) || v < AntiAliasingRanges::kFilteredTemporalWeightModeMin || v > AntiAliasingRanges::kFilteredTemporalWeightModeMax)
		{
			CommandLineError(keyA + " needs 0, 1 or 2");
			return true;
		}
		m_DebugOverrides.push_back([v](FTemporalAADebugSettings& d) { d.FilteredTemporalWeightMode = v; });
		Message(text + " (debug, not saved)");
		return true;
	}
	return false;
}


// ------------------------------------------------------------
//  コマンドライン解析 (GameManager のコンストラクタ)
// ------------------------------------------------------------
void FTemporalAATestDriver::ParseCommandLine(const wchar_t* CmdLine)
{
	bool bCaptureGiven = false;
	bool bReallocPeriodGiven = false;
	bool bResponsiveGiven = false;
	std::vector<std::string> testOnlyKeys;	// -taatest 無しでは無視するキー

	for (const std::wstring& token : TokenizeCommandLine(CmdLine))
	{
		const size_t eq = token.find(L'=');
		const std::wstring key = token.substr(0, eq);
		const std::wstring value = (eq == std::wstring::npos) ? std::wstring() : token.substr(eq + 1);
		const bool bHasValue = (eq != std::wstring::npos);
		const std::string keyA = ToNarrow(key);

		if (key == L"-taatest")
		{
			// シナリオ名が不正でもテストモードには入る (ログにエラーを書いて終了コード 2 で終わる。
			// 対話モードで起動したまま終わらない事態を避ける)
			m_bActive = true;
			m_ScenarioName = ToNarrow(value);
			if (m_ScenarioName == "static")         m_Scenario = EScenario::Static;
			else if (m_ScenarioName == "pan")       m_Scenario = EScenario::Pan;
			else if (m_ScenarioName == "rotate")    m_Scenario = EScenario::Rotate;
			else if (m_ScenarioName == "cut")       m_Scenario = EScenario::Cut;
			else if (m_ScenarioName == "largemove") m_Scenario = EScenario::LargeMove;
			else if (m_ScenarioName == "realloc")   m_Scenario = EScenario::Realloc;
			else if (m_ScenarioName == "resize")    m_Scenario = EScenario::Resize;
			else if (m_ScenarioName == "moveobj")   m_Scenario = EScenario::MoveObj;
			else if (m_ScenarioName == "sky")       m_Scenario = EScenario::Sky;
			else if (m_ScenarioName == "aatoggle")  m_Scenario = EScenario::AAToggle;
			else if (m_ScenarioName == "translucent") m_Scenario = EScenario::Translucent;
			else if (m_ScenarioName == "qswitch")   m_Scenario = EScenario::QSwitch;
			else
			{
				CommandLineError("unknown scenario '" + m_ScenarioName + "'");
				continue;
			}
		}
		else if (key == L"-taaframes")
		{
			int n = 0;
			if (!ParseInt(value, n) || n < 1)
				CommandLineError("-taaframes needs an integer >= 1");
			else
				m_NumFrames = n;
			testOnlyKeys.push_back(keyA);
		}
		else if (key == L"-taacapture")
		{
			bCaptureGiven = true;
			size_t begin = 0;
			while (begin <= value.size())
			{
				const size_t comma = value.find(L',', begin);
				const std::wstring item = value.substr(begin, (comma == std::wstring::npos) ? std::wstring::npos : comma - begin);
				int f = 0;
				if (!ParseInt(item, f) || f < 1)
				{
					CommandLineError("-taacapture needs a comma separated list of frames >= 1");
					break;
				}
				m_CaptureFrames.push_back(f);
				if (comma == std::wstring::npos)
					break;
				begin = comma + 1;
			}
			testOnlyKeys.push_back(keyA);
		}
		else if (key == L"-taaout")
		{
			if (value.empty())
				CommandLineError("-taaout needs a folder");
			else
				m_OutDir = value;
			testOnlyKeys.push_back(keyA);
		}
		else if (key == L"-reallocperiod")
		{
			// realloc シナリオ: 強制再確保 (bRequestReallocate) の間隔 [フレーム]
			int n = 0;
			if (!ParseInt(value, n) || n < 1)
				CommandLineError("-reallocperiod needs an integer >= 1");
			else
			{
				m_ReallocPeriod = n;
				bReallocPeriodGiven = true;
			}
			testOnlyKeys.push_back(keyA);
		}
		else if (key == L"-responsive")
		{
			// translucent シナリオ: 猫のスロット 0 の bEnableResponsiveAA (UMaterial::bEnableResponsiveAA)
			if (!ParseBool01(value, m_bResponsive))
				CommandLineError("-responsive needs 0 or 1");
			else
				bResponsiveGiven = true;
			testOnlyKeys.push_back(keyA);
		}
		else if (key == L"-fixeddt")
		{
			float dt = 0.0f;
			if (!ParseFloat(value, dt) || dt <= 0.0f || dt > 1.0f)
				CommandLineError("-fixeddt needs seconds in (0, 1]");
			else
				m_FixedDeltaTime = dt;
			testOnlyKeys.push_back(keyA);
		}
		else if (key == L"-grain")
		{
			if (!ParseBool01(value, m_bGrain))
				CommandLineError("-grain needs 0 or 1");
			testOnlyKeys.push_back(keyA);
		}
		else if (key == L"-dof")
		{
			bool b = false;
			if (!ParseBool01(value, b))
				CommandLineError("-dof needs 0 or 1");
			else
				m_DOFOverride = b ? 1 : 0;
			testOnlyKeys.push_back(keyA);
		}
		else if (key == L"-taaselftest")
		{
			// 値は取らない。対話起動 (-taatest 無し) でも有効 (結果はデバッグ出力のみ)
			if (bHasValue)
				CommandLineError("-taaselftest takes no value");
			else
				m_bSelfTest = true;
		}
		else if (ParseAntiAliasingOverride(key, value))
		{
			// FAntiAliasingParams の上書き (保存しないため -taatest 時のみ有効)
			testOnlyKeys.push_back(keyA);
		}
		else if (ParseDebugOverride(key, value))
		{
			// FTemporalAADebugSettings の上書き (-taatest 時のみ有効)
			testOnlyKeys.push_back(keyA);
		}
		else
		{
			CommandLineError("unknown key '" + ToNarrow(token) + "'");
		}
	}

	// キャプチャフレーム: 昇順・重複なし。範囲外はコマンドライン不正
	std::sort(m_CaptureFrames.begin(), m_CaptureFrames.end());
	m_CaptureFrames.erase(std::unique(m_CaptureFrames.begin(), m_CaptureFrames.end()), m_CaptureFrames.end());
	if (!bCaptureGiven)
	{
		m_CaptureFrames.push_back(m_NumFrames);	// 既定 = 最終フレーム
	}
	else if (!m_CaptureFrames.empty() && m_CaptureFrames.back() > m_NumFrames)
	{
		CommandLineError("-taacapture frame " + std::to_string(m_CaptureFrames.back()) +
			" is beyond -taaframes=" + std::to_string(m_NumFrames));
	}

	// -reallocperiod は realloc シナリオ専用 (他のシナリオでは効かない旨を注記するだけ)
	if (m_bActive && bReallocPeriodGiven && m_Scenario != EScenario::Realloc)
	{
		Message("-reallocperiod: only used by the realloc scenario (ignored)");
	}
	// -responsive は translucent シナリオ専用
	if (m_bActive && bResponsiveGiven && m_Scenario != EScenario::Translucent)
	{
		Message("-responsive: only used by the translucent scenario (ignored)");
	}

	if (!m_bActive)
	{
		for (const std::string& k : testOnlyKeys)
			Message(k + ": ignored without -taatest");
		if (m_bCommandLineError)
			m_ExitCode = ExitBadCommandLine;	// 対話モードは起動を続け、終了コードだけ 2 にする
	}
}


// ------------------------------------------------------------
//  OnBegin: SettingsManager::Initialize (INI 適用) の後
// ------------------------------------------------------------
void FTemporalAATestDriver::OnBegin(UWorld& World, FSceneRenderer& Renderer, SettingsManager& Settings)
{
	// -taaselftest: フレーム 1 の BeginFrame 先頭で自己テスト (対話起動でも有効。結果はデバッグ出力)
	if (m_bSelfTest)
	{
		Renderer.GetTemporalAADebugSettings().bRequestSelfTest = true;
	}

	if (!m_bActive)
		return;

	// ---- INI を書き換えない ----
	Settings.SetSaveEnabled(false);				// EngineSettings.ini (終了時自動保存 / 保存ボタン)
	ImGui::GetIO().IniFilename = nullptr;		// imgui.ini (読み込みも保存もしない)

	// ---- 固定デルタタイム ----
	Time::SetFixedDeltaTime(m_FixedDeltaTime);

	// ---- 出力フォルダ + ログ ----
	{
		std::error_code ec;
		std::filesystem::create_directories(std::filesystem::path(m_OutDir), ec);

		const std::filesystem::path logPath = std::filesystem::path(m_OutDir) / L"taatest_log.txt";
		if (_wfopen_s(&m_Log, logPath.c_str(), L"w") != 0)
			m_Log = nullptr;

		if (m_Log == nullptr)
		{
			Message("cannot create " + ToNarrow(logPath.wstring()));
			RequestExit(ExitCaptureWriteFailed);
			return;
		}

		// コンストラクタで出たメッセージを先頭に書く
		for (const std::string& text : m_PendingMessages)
			fprintf(m_Log, "# %s\n", text.c_str());
		m_PendingMessages.clear();
		fflush(m_Log);
	}

	if (m_bCommandLineError)
	{
		RequestExit(ExitBadCommandLine);
		return;
	}

	// ---- カメラ (スポーン順 5 = ACameraActor) ----
	m_Camera = FindActorByIndex<ACameraActor>(World, kCameraActorIndex);
	assert(m_Camera != nullptr && "actor #5 must be ACameraActor");
	if (m_Camera == nullptr)
	{
		Message("actor #" + std::to_string(kCameraActorIndex) + " is not ACameraActor");
		RequestExit(ExitBadCommandLine);
		return;
	}
	m_Camera->SetInputEnabled(false);			// マウス / ホイール / キーボードを無視
	m_Camera->ResetVelocity();					// INI 適用前後の慣性 / 未放出量を捨てる
	m_C0Location = m_Camera->GetActorLocation();	// C0 = INI 適用後の姿勢
	m_C0Rotation = m_Camera->GetActorRotation();

	// ---- moveobj: 動かすアクター (スポーン順 8 = Table) と円運動の基準位置 (INI 適用後) ----
	if (m_Scenario == EScenario::MoveObj)
	{
		m_MovingActor = FindActorByIndex<AStaticMeshActor>(World, kTableActorIndex);
		assert(m_MovingActor != nullptr && "actor #8 must be AStaticMeshActor (Table)");
		if (m_MovingActor == nullptr)
		{
			Message("actor #" + std::to_string(kTableActorIndex) + " is not AStaticMeshActor");
			RequestExit(ExitBadCommandLine);
			return;
		}
		m_MovingActorBase = m_MovingActor->GetActorLocation();
	}

	// ---- translucent: 猫 (スポーン順 9) のスロット 0 を半透明 (Opacity 0.5) + -responsive にし、
	//      円運動の基準位置 (INI 適用後) を覚える。保存はされない (SetSaveEnabled(false) 済み) ----
	if (m_Scenario == EScenario::Translucent)
	{
		m_MovingActor = FindActorByIndex<AStaticMeshActor>(World, kCatActorIndex);
		assert(m_MovingActor != nullptr && "actor #9 must be AStaticMeshActor (Cat)");
		if (m_MovingActor == nullptr || m_MovingActor->GetStaticMeshComponent() == nullptr ||
			m_MovingActor->GetStaticMeshComponent()->GetNumMaterialSlots() == 0)
		{
			Message("actor #" + std::to_string(kCatActorIndex) + " is not AStaticMeshActor with a material slot");
			RequestExit(ExitBadCommandLine);
			return;
		}
		m_MovingActorBase = m_MovingActor->GetActorLocation();

		UStaticMeshComponent* mesh = m_MovingActor->GetStaticMeshComponent();
		Material& material = mesh->GetMaterial(0);
		material.SetBlendMode(EBlendMode::BLEND_Translucent);
		material.Params.Opacity = kTranslucentOpacity;
		material.SetEnableResponsiveAA(m_bResponsive);
		mesh->MarkRenderStateDirty();		// GetMaterial() 直接書き換えの反映 (次の SendAllEndOfFrameUpdates でプロキシ再生成)

		char msg[160];
		sprintf_s(msg, "translucent: Cat slot 0 BLEND_Translucent Opacity=%g bEnableResponsiveAA=%d",
			kTranslucentOpacity, m_bResponsive ? 1 : 0);
		Message(msg);
	}

	// ---- ポストプロセスボリューム: グレイン (既定 Off) / DOF (指定時のみ)。保存はされない ----
	{
		std::vector<APostProcessVolume*> volumes;
		World.GetActorsOfClass<APostProcessVolume>(volumes);
		for (APostProcessVolume* volume : volumes)
		{
			volume->SetFlag(PP_FLAG_GRAIN, m_bGrain);
			if (m_DOFOverride >= 0)
				volume->SetFlag(PP_FLAG_DOF, m_DOFOverride != 0);
		}
	}

	// ---- FAntiAliasingParams の上書き (INI 適用後。SetSaveEnabled(false) 済みなので保存されない) ----
	{
		FAntiAliasingParams& aa = Renderer.GetAntiAliasingParams();
		for (const auto& apply : m_AAOverrides)
			apply(aa);
		SanitizeAntiAliasingParams(aa);		// INI 読み込みと同じ範囲 / 丸め (§7.1)

		char msg[640];
		sprintf_s(msg, "params: AntiAliasingMethod=%d ScreenPercentage=%g bTemporalAAUpsampling=%d TemporalAAQuality=%d "
			"TemporalAASamples=%d TemporalAACurrentFrameWeight=%g TemporalAAFilterSize=%g bTemporalAACatmullRom=%d "
			"bTemporalAAUpsampleFiltered=%d TemporalAAHistoryScreenPercentage=%g bTemporalAAR11G11B10History=%d "
			"bTemporalAAAllowDownsampling=%d UpscaleQuality=%d UpscaleSoftness=%g TonemapperMergeWithUpscaleMode=%d "
			"TonemapperMergeWithUpscaleThreshold=%g ViewTextureMipBiasOffset=%g ViewTextureMipBiasMin=%g "
			"CameraRotationThreshold=%g CameraTranslationThreshold=%g",
			aa.AntiAliasingMethod, aa.ScreenPercentage, aa.bTemporalAAUpsampling ? 1 : 0, aa.TemporalAAQuality,
			aa.TemporalAASamples, aa.TemporalAACurrentFrameWeight, aa.TemporalAAFilterSize, aa.bTemporalAACatmullRom ? 1 : 0,
			aa.bTemporalAAUpsampleFiltered ? 1 : 0, aa.TemporalAAHistoryScreenPercentage, aa.bTemporalAAR11G11B10History ? 1 : 0,
			aa.bTemporalAAAllowDownsampling ? 1 : 0, aa.UpscaleQuality, aa.UpscaleSoftness, aa.TonemapperMergeWithUpscaleMode,
			aa.TonemapperMergeWithUpscaleThreshold, aa.ViewTextureMipBiasOffset, aa.ViewTextureMipBiasMin,
			aa.CameraRotationThreshold, aa.CameraTranslationThreshold);
		// 上書きがある時だけログへ "# " 行で残す (通常の実行のログはフレーム行のみ = N 行のまま)。
		// デバッグ出力には常に出す
		if (!m_AAOverrides.empty())
			Message(msg);
		else
			OutputDebugStringA((std::string("[TAA Test] ") + msg + "\n").c_str());
	}

	// ---- FTemporalAADebugSettings の上書き (-forcejitter / -nojitter / -overrideindex / -debugvis / -visscale / -ftwmode) ----
	if (!m_DebugOverrides.empty())
	{
		FTemporalAADebugSettings& debug = Renderer.GetTemporalAADebugSettings();
		for (const auto& apply : m_DebugOverrides)
			apply(debug);

		char msg[256];
		sprintf_s(msg, "debug: bForceJitterWithoutTAA=%d bDisableJitter=%d OverrideTemporalIndex=%d DebugView=%d VisualizeScale=%g "
			"FilteredTemporalWeightMode=%d",
			debug.bForceJitterWithoutTAA ? 1 : 0, debug.bDisableJitter ? 1 : 0, debug.OverrideTemporalIndex,
			(int)debug.DebugView, debug.VisualizeScale, debug.FilteredTemporalWeightMode);
		Message(msg);
	}

	char msg[512];
	sprintf_s(msg, "start scenario=%s frames=%d captures=%d dt=%.6f out=%s",
		m_ScenarioName.c_str(), m_NumFrames, (int)m_CaptureFrames.size(), m_FixedDeltaTime, ToNarrow(m_OutDir).c_str());
	OutputDebugStringA((std::string("[TAA Test] ") + msg + "\n").c_str());
}


// ------------------------------------------------------------
//  PreWorldTick: World.Tick の前 (ASky がこのフレームのカメラ位置を追従できるように)
// ------------------------------------------------------------
void FTemporalAATestDriver::PreWorldTick(FSceneRenderer& Renderer)
{
	if (!m_bActive || m_bExitRequested)
		return;

	++m_Frame;
	const float t = (float)(m_Frame - 1) * m_FixedDeltaTime;

	// ---- シナリオのトランスフォーム ----
	switch (m_Scenario)
	{
	case EScenario::Pan:
		// カメラ位置 = C0 + (2t, 0, 0) m
		m_Camera->SetActorLocation({ m_C0Location.x + 2.0f * t, m_C0Location.y, m_C0Location.z });
		break;

	case EScenario::Rotate:
	{
		// カメラヨー = C0.yaw + 45 deg * t
		XMFLOAT3 rotation = m_C0Rotation;
		rotation.y = m_C0Rotation.y + XMConvertToRadians(45.0f) * t;
		m_Camera->SetActorRotation(rotation);
		break;
	}

	case EScenario::Cut:
		// 静止。フレーム 60 でカメラを C0 + (3, 0, 3) m / ヨー +90 度へ移し、カメラカットを通知する
		// (ラッチ: ACameraActor::Tick が慣性を捨て、CalcSceneView が消費して bCameraCut = 1)。
		// 以降は入力停止 + 速度 0 なのでその姿勢のまま
		if (m_Frame == kEventFrame)
		{
			m_Camera->SetActorLocation({ m_C0Location.x + 3.0f, m_C0Location.y, m_C0Location.z + 3.0f });
			XMFLOAT3 rotation = m_C0Rotation;
			rotation.y = m_C0Rotation.y + XMConvertToRadians(90.0f);
			m_Camera->SetActorRotation(rotation);
			m_Camera->GetCameraComponent()->NotifyCameraCut();
		}
		break;

	case EScenario::LargeMove:
		// 静止。フレーム 60 でカメラを C0 + (150, 0, 0) m へ移す。カット要求は出さない
		// (レンダラの大移動判定 (100 m 超) で bPrevTransformsReset = 1 になる)
		if (m_Frame == kEventFrame)
		{
			m_Camera->SetActorLocation({ m_C0Location.x + 150.0f, m_C0Location.y, m_C0Location.z });
		}
		break;

	case EScenario::Realloc:
		// 静止。-reallocperiod (既定 90) の倍数フレームで、同一サイズのままレンダー / ポスト解像度の
		// ターゲットを強制的に作り直す (このフレームの BeginFrame 先頭の ResizeRenderTargets で処理される)。
		// Lumen / Fog の履歴はそのフレームだけ無効になる
		if (m_Frame % m_ReallocPeriod == 0)
		{
			Renderer.GetTemporalAADebugSettings().bRequestReallocate = true;
		}
		break;

	case EScenario::Resize:
		// 静止。ScreenPercentage を 100 -> 50 -> 71 -> 150 -> 200 -> 100 (フレーム 1/30/60/90/120/150) と変える。
		// このフレームの BeginFrame 先頭 (PrepareViewRectsForRendering) が R の変化を検出して
		// ResizeRenderTargets で作り直す (保存はしない: SetSaveEnabled(false) 済み)
		for (const FResizeStep& step : kResizeSchedule)
		{
			if (m_Frame == step.Frame)
			{
				Renderer.GetAntiAliasingParams().ScreenPercentage = step.ScreenPercentage;
				char msg[96];
				sprintf_s(msg, "frame %d: ScreenPercentage=%g", m_Frame, step.ScreenPercentage);
				OutputDebugStringA((std::string("[TAA Test] ") + msg + "\n").c_str());
			}
		}
		break;

	case EScenario::MoveObj:
		// カメラ静止。Table を半径 1 m / 周期 2 s の円運動 (XZ 平面, t = 0 で基準位置)。
		// フレーム 91 以降は動かさない (フレーム 90 の位置で停止): 止まった次のフレームに
		// ベロシティが残らないこと (FSceneVelocityData::StartFrame) の検査用
		if (m_Frame <= kMoveObjLastMovingFrame)
		{
			const float phase = XM_2PI * t / 2.0f;
			m_MovingActor->SetActorLocation({ m_MovingActorBase.x + std::cos(phase) - 1.0f, m_MovingActorBase.y,
				m_MovingActorBase.z + std::sin(phase) });
		}
		break;

	case EScenario::Sky:
	{
		// カメラ位置 C0、ピッチ -15 度 (見上げ = スカイドーム)、ヨー = C0.yaw + 90 deg * t
		XMFLOAT3 rotation = m_C0Rotation;
		rotation.x = XMConvertToRadians(-15.0f);
		rotation.y = m_C0Rotation.y + XMConvertToRadians(90.0f) * t;
		m_Camera->SetActorRotation(rotation);
		break;
	}

	case EScenario::AAToggle:
	{
		// 静止。AntiAliasingMethod をフレーム 1-59 は 0 (None)、60 以降は 2 (TemporalAA)。
		// AA 手法の変化はレンダラ側でカメラカット扱い (履歴無し = BlendFinal 1) になる (-aa の指定は上書きされる)
		const int method = (m_Frame < kEventFrame) ? 0 : 2;
		int& current = Renderer.GetAntiAliasingParams().AntiAliasingMethod;
		if (current != method)
		{
			current = method;
			char msg[96];
			sprintf_s(msg, "frame %d: AntiAliasingMethod=%d", m_Frame, method);
			OutputDebugStringA((std::string("[TAA Test] ") + msg + "\n").c_str());
		}
		break;
	}

	case EScenario::Translucent:
	{
		// カメラ静止。半透明の猫を moveobj と同じ円運動 (半径 1 m, 周期 2 s, t = 0 で基準位置) で動かし続ける。
		// 半透明は深度もベロシティも書かないので、TAA は背景 (不透明の LinearDepth) で再投影する
		const float phase = XM_2PI * t / 2.0f;
		m_MovingActor->SetActorLocation({ m_MovingActorBase.x + std::cos(phase) - 1.0f, m_MovingActorBase.y,
			m_MovingActorBase.z + std::sin(phase) });
		break;
	}

	case EScenario::QSwitch:
	{
		// 静止。TemporalAAQuality = 1 + bTemporalAAR11G11B10History = 1 (フレーム 1-59: R11G11B10 履歴) ->
		// Quality 2 (フレーム 60 以降: RGBA16F 履歴 + アンチゴースト)。フレーム 60 は R11G11B10 の入力履歴
		// (SRV の a = 1) を High が読むので、HISTORY_HAS_ALPHA が立たず AntiGhostReject が出ないこと (§3.4)
		FAntiAliasingParams& aa = Renderer.GetAntiAliasingParams();
		const int quality = (m_Frame < kEventFrame) ? 1 : 2;
		if (m_Frame == 1)
		{
			aa.bTemporalAAR11G11B10History = true;
		}
		if (aa.TemporalAAQuality != quality)
		{
			aa.TemporalAAQuality = quality;
			char msg[96];
			sprintf_s(msg, "frame %d: TemporalAAQuality=%d", m_Frame, quality);
			OutputDebugStringA((std::string("[TAA Test] ") + msg + "\n").c_str());
		}
		break;
	}

	case EScenario::Static:
	default:
		break;
	}

	// ---- キャプチャ要求 (このフレームの RenderPostProcessing で記録される) ----
	m_bCaptureThisFrame = IsCaptureFrame(m_Frame);
	if (m_bCaptureThisFrame)
	{
		Renderer.RequestScreenshot(MakeCapturePath(m_Frame));
	}
}


// ------------------------------------------------------------
//  PostFrame: EndFrame (Present) の後
// ------------------------------------------------------------
void FTemporalAATestDriver::PostFrame(FSceneRenderer& Renderer, RenderManager& RHI)
{
	// 自己テストの出力 (このフレームの BeginFrame 先頭で実行された場合) をログへ移す。
	// テストモード外 (対話起動の "Run Self Test" / -taaselftest) は捨てるだけ (デバッグ出力には出ている)
	DrainSelfTestLog(Renderer);

	if (!m_bActive || m_bExitRequested || m_Frame == 0)
		return;

	const int ow = RHI.GetBackBufferWidth();
	const int oh = RHI.GetBackBufferHeight();

	// 出力解像度 O はテストモードのポップアップウィンドウで 1920x1080 固定のはず (Main.cpp)
	if (m_Frame == 1 && (ow != kOutputWidth || oh != kOutputHeight))
	{
		Message("warning: output extent is " + std::to_string(ow) + "x" + std::to_string(oh) +
			", expected " + std::to_string(kOutputWidth) + "x" + std::to_string(kOutputHeight));
	}

	// ---- ログ 1 行 (CSV。列はヘッダのコメント参照) ----
	if (m_Log)
	{
		const FViewFamilyInfo& F = Renderer.GetViewFamily();
		const FViewInfo& V = Renderer.GetViewInfo();
		const FTemporalAAStats& s = Renderer.GetTemporalAAStats();
		const XMFLOAT3 loc = m_Camera->GetActorLocation();
		const XMFLOAT3 rot = m_Camera->GetActorRotation();

		// TAA を実行しない構成はパス / 品質 / 履歴フォーマットを "-"
		char pass[32] = "-", quality[8] = "-", format[16] = "-";
		if (s.bTAARanThisFrame)
		{
			sprintf_s(pass, "%s", GetTAAPassConfigName(F.TAAPass));
			sprintf_s(quality, "%d", (int)F.TAAQuality);
			sprintf_s(format, "%s", (s.HistoryFormat == DXGI_FORMAT_R11G11B10_FLOAT) ? "R11G11B10" : "RGBA16F");
		}

		const double vramMB = (double)RHI.QueryLocalVideoMemoryUsage() / (1024.0 * 1024.0);
		// デスクリプタ / 遅延解放キューは WaitGPU (下) の全解放より前の値 (Present 直後の定常状態) を記録する
		const size_t freeSRV = RHI.GetNumFreeSRVDescriptors();
		const size_t freeRTV = RHI.GetNumFreeRTVDescriptors();
		const size_t deferredQueue = RHI.GetDeferredReleaseQueueLength();

		// AutoExposure の読み戻し露出 (ImGui と同じ READBACK)。TAA ハーフ解像度出力 (AutoExposure の入力) の検証用。
		// READBACK バッファは 1 つだけで、このフレームの CopyBufferRegion が GPU でまだ実行中かもしれない
		// (Present は 1 フレーム前のフェンスしか待たない)。WaitGPU で完了を待ってから読む = 常にこのフレームの結果
		float aeExposure = 0.0f, aeAvgLuminance = 0.0f;
		if (AutoExposure* ae = Renderer.GetAutoExposure())
		{
			RHI.WaitGPU();
			ae->UpdateReadback();
			aeExposure = ae->GetCurrentExposure();
			aeAvgLuminance = ae->GetCurrentAvgLuminance();
		}

		fprintf(m_Log,
			"%d,%.6f,%ux%u,%ux%u,%ux%u,%ux%u,%s,%s,%s,%s,"		// frame,t,R,S,H,P,method,pass,quality,format
			"%d,%d,%.5f,%.5f,%.4f,"							// jitter_index,jitter_n,jitter_x,jitter_y,mip_bias
			"%d,%d,%d,%d,%d,"								// bCameraCut,bPrevTransformsReset,bLumenHistoryValid,bFogHistoryValid,bHistoryValidThisFrame
			"%d,%d,%d,%zu,%zu,%zu,%.1f,%d,%d,"				// velocity_draws,taa_ran,merge,free_srv,free_rtv,deferred_queue,vram_mb,resizes,selftest_failures
			"%.5f,%.5f,%.5f,%.4f,%.4f,%.4f,%d,%.6f,%.6f,"	// cam_x..cam_roll,capture,ae_exposure,ae_avg_luminance
			"%d,%.3f\n",									// num_visible,frame_ms
			m_Frame, (double)((float)(m_Frame - 1) * m_FixedDeltaTime),
			F.RenderExtent.x, F.RenderExtent.y, F.SecondaryExtent.x, F.SecondaryExtent.y,
			F.HistoryExtent.x, F.HistoryExtent.y, s.PostExtent.x, s.PostExtent.y,
			GetAntiAliasingMethodName(F.AntiAliasingMethod), pass, quality, format,
			V.TemporalJitterIndex, V.TemporalJitterSequenceLength, V.TemporalJitterPixels.x, V.TemporalJitterPixels.y, V.MaterialTextureMipBias,
			V.bCameraCut ? 1 : 0, V.bPrevTransformsReset ? 1 : 0, s.bLumenHistoryValid ? 1 : 0, s.bFogHistoryValid ? 1 : 0, s.bHistoryValidThisFrame ? 1 : 0,
			s.NumVelocityDraws, s.bTAARanThisFrame ? 1 : 0, s.bUpscaleMerged ? 1 : 0,
			freeSRV, freeRTV, deferredQueue,
			vramMB, s.NumResizes, s.LastSelfTestFailures,
			loc.x, loc.y, loc.z,
			XMConvertToDegrees(rot.x), XMConvertToDegrees(rot.y), XMConvertToDegrees(rot.z),
			m_bCaptureThisFrame ? 1 : 0, aeExposure, aeAvgLuminance,
			Renderer.GetCullingStats().NumVisible, (double)Time::GetMeasuredDeltaTime() * 1000.0);
		fflush(m_Log);
	}

	if (m_Frame >= m_NumFrames)
	{
		Finish(Renderer);
	}
}


void FTemporalAATestDriver::Finish(FSceneRenderer& Renderer)
{
	// 最終フレームで記録したコピーを WaitGPU して書き出す
	Renderer.FlushScreenshots();

	const FScreenshotCapture* capture = Renderer.GetScreenshotCapture();
	const int expected = (int)m_CaptureFrames.size();
	const int written = capture ? capture->GetNumWritten() : 0;
	const int failed = capture ? capture->GetNumFailed() : 1;

	int exitCode = ExitSuccess;
	if (failed > 0 || written != expected)
	{
		Message("capture write failed: written=" + std::to_string(written) +
			" expected=" + std::to_string(expected) + " failed=" + std::to_string(failed));
		exitCode = ExitCaptureWriteFailed;
	}
	if (m_bSelfTest && Renderer.GetTemporalAAStats().LastSelfTestFailures < 0)
	{
		// -taaselftest を指定したのに一度も走らなかった
		Message("self test was requested but did not run");
		SetFailure(ExitSelfTestFailed);
	}

	m_bFinished = true;
	RequestExit(exitCode);

	char msg[256];
	sprintf_s(msg, "[TAA Test] finished: frames=%d captures=%d/%d exit=%d\n", m_Frame, written, expected, m_ExitCode);
	OutputDebugStringA(msg);

	if (m_Log)
	{
		fclose(m_Log);
		m_Log = nullptr;
	}
}


void FTemporalAATestDriver::DrainSelfTestLog(FSceneRenderer& Renderer)
{
	const std::vector<std::string> lines = Renderer.TakeSelfTestLog();
	if (lines.empty() || !m_bActive)
		return;

	// このフレームの BeginFrame 先頭で自己テストが走った。行をログへ ("# " コメント)
	if (m_Log)
	{
		for (const std::string& line : lines)
			fprintf(m_Log, "# %s\n", line.c_str());
		fflush(m_Log);
	}

	// -taatest 中の失敗は終了コード 1 (テストは最後まで走らせる)
	if (Renderer.GetTemporalAAStats().LastSelfTestFailures > 0)
	{
		SetFailure(ExitSelfTestFailed);
	}
}


bool FTemporalAATestDriver::IsCaptureFrame(int Frame) const
{
	return std::binary_search(m_CaptureFrames.begin(), m_CaptureFrames.end(), Frame);
}

std::string FTemporalAATestDriver::MakeCapturePath(int Frame) const
{
	// <out>/<scenario>_f<frame>.bmp
	const std::filesystem::path path = std::filesystem::path(m_OutDir) /
		(m_ScenarioName + "_f" + std::to_string(Frame) + ".bmp");
	return path.u8string();		// UTF-8 (FScreenshotCapture::Request の Path 規約。ANSI への縮退で別フォルダへ書かれないように)
}
