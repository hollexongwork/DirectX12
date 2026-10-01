#pragma once
#include <string>
#include <vector>
#include <functional>
#include <cstdio>
#include <DirectXMath.h>

class UWorld;
class FSceneRenderer;
class SettingsManager;
class RenderManager;
class ACameraActor;
class AStaticMeshActor;
struct FAntiAliasingParams;
struct FTemporalAADebugSettings;

// ============================================================
//  FTemporalAATestDriver
//  TAA / TAAU 検証用のコマンドラインシナリオドライバ (-taatest)。
//  固定デルタタイム + スクリプト化したカメラ / アクターの動きで
//  決定的なフレーム列を描き、指定フレームのバックバッファ (UI 無し)
//  を BMP に保存し、フレーム毎の CSV ログを書いて終了コードで終わる。
//  INI (EngineSettings.ini / imgui.ini) は一切書き換えない。
//
//  コマンドライン (キーは大文字小文字を区別。wWinMain の lpCmdLine):
//    -taatest=<scenario>  テストモード: 設定の上書きは SettingsManager::Initialize の後、
//                         INI 保存停止 / ImGui IniFilename = nullptr / 固定 dt /
//                         カメラ入力停止 + 速度リセット / ImGui ウィンドウ非構築 / グレイン Off
//    -taaframes=N         実行フレーム数 (フレーム 1 = 最初の Draw)       既定 120
//    -taacapture=a,b,...  BMP を保存するフレーム                          既定 最終フレーム
//    -taaout=<dir>        出力フォルダ (作成する)                          既定 Saved/TAATest
//    -taaselftest         フレーム 1 の BeginFrame 先頭で自己テスト (§9.2) を実行。
//                         -taatest と併用すると失敗で終了コード 1。単独 (対話起動) ではログのみ
//    -fixeddt=<s>         固定デルタタイム [s]                             既定 1/60
//    -grain=0/1, -dof=0/1 ポストプロセスボリュームの PP_FLAG_GRAIN / PP_FLAG_DOF
//                         (保存しない)                                     既定 grain 0 / DOF 変更なし
//    FAntiAliasingParams の上書き (保存しない。INI 適用後に書き、§7.1 の範囲へ丸める):
//      -aa=<int>          AntiAliasingMethod (1/3 -> 0, 4 -> 2)
//      -sp=<float>        ScreenPercentage
//      -upsampling=0/1    bTemporalAAUpsampling
//      -taaq=<int>        TemporalAAQuality
//      -samples=<int>     TemporalAASamples (-forcejitter のジッタ列にも効く)
//      -cfw=<float>       TemporalAACurrentFrameWeight
//      -catmullrom=0/1    bTemporalAACatmullRom
//      -hsp=<float>       TemporalAAHistoryScreenPercentage
//      -r11=0/1           bTemporalAAR11G11B10History
//      -allowdown=0/1     bTemporalAAAllowDownsampling
//      -upscaleq=<int>    UpscaleQuality
//      -merge=<int>       TonemapperMergeWithUpscaleMode
//      -mipoffset=<float> ViewTextureMipBiasOffset
//      -mipmin=<float>    ViewTextureMipBiasMin
//    FTemporalAADebugSettings の上書き (非永続。OnBegin で適用):
//      -forcejitter=0/1   bForceJitterWithoutTAA (TAA 無効でもジッタを掛ける。ジッタ比較用)
//      -nojitter=0/1      bDisableJitter (ジッタ 0)
//      -overrideindex=<int> OverrideTemporalIndex (-1 = 無効, >= 0 でジッタ添字を固定)
//      -debugvis=<int>    DebugView (ETemporalAADebugView 0..13)
//      -visscale=<float>  VisualizeScale ([0.1, 64] へ丸める)
//      -ftwmode=<int>     FilteredTemporalWeightMode (0 = 空間重み総和, 1 = 最近傍重み, 2 = 1.0)
//    -reallocperiod=N     realloc シナリオの強制再確保の間隔 [フレーム] (>= 1)       既定 90
//    -responsive=0/1      translucent シナリオ: 猫 (スポーン順 9) のスロット 0 の
//                         Material::SetEnableResponsiveAA (UMaterial::bEnableResponsiveAA)     既定 0
//    未知のキーはログに出して終了コード 2。
//
//  シナリオ (C0 = INI 適用後のカメラ姿勢, t = (frame - 1) * dt):
//    static    : 動きなし
//    pan       : カメラ位置 = C0 + (2t, 0, 0) m
//    rotate    : カメラヨー = C0.yaw + 45 deg * t
//    cut       : 静止。フレーム 60 でカメラを C0 + (3, 0, 3) m / ヨー +90 deg へ移し NotifyCameraCut
//    largemove : 静止。フレーム 60 でカメラを C0 + (150, 0, 0) m へ移す (カット要求なし = 大移動判定のみ)
//    realloc   : 静止。-reallocperiod (既定 90) の倍数フレームで bRequestReallocate = true
//                (同一サイズのまま R / P のターゲットを ResizeRenderTargets で作り直す。
//                 リサイズ後の画像 / 履歴フラグ / リーク検査用)
//    resize    : 静止。ScreenPercentage を 100 (フレーム 1) -> 50 (30) -> 71 (60) -> 150 (90)
//                -> 200 (120) -> 100 (150) と変える (実行時の解像度変更 / リーク検査。>= 180 フレーム推奨。
//                -sp の指定はこのスケジュールで上書きされる)
//    moveobj   : カメラ静止。Table (スポーン順 8) の位置 = 基準 + (cos(2 pi t / 2) - 1, 0, sin(2 pi t / 2)) m
//                (半径 1 m, 周期 2 s)。フレーム 91 以降はフレーム 90 の位置で停止 (古い Prev の検査)
//    sky       : カメラ位置 C0、ピッチ -15 deg (見上げ)、ヨー = C0.yaw + 90 deg * t
//    aatoggle  : 静止。AntiAliasingMethod = 0 (フレーム 1-59) -> 2 (フレーム 60 以降)。
//                AA 手法の変化はカメラカット扱い (フレーム 60 は履歴無し)。-aa の指定は上書きされる
//    translucent : カメラ静止。猫 (スポーン順 9) のスロット 0 を BLEND_Translucent / Opacity 0.5 にし
//                (-responsive で bEnableResponsiveAA)、moveobj と同じ円運動 (基準 + (cos(2 pi t / 2) - 1, 0,
//                sin(2 pi t / 2)) m) を止めずに続ける (動く半透明の残像 / Responsive AA の検査)
//    qswitch   : 静止。TemporalAAQuality = 1 + bTemporalAAR11G11B10History = 1 (フレーム 1-59) ->
//                Quality 2 (フレーム 60 以降)。R11G11B10 履歴 (alpha = 1) を High (アンチゴースト有り) が
//                読む最初のフレームで HISTORY_HAS_ALPHA ガードが効くかの検査。-taaq / -r11 の指定は上書きされる
//
//  出力:
//    <out>/<scenario>_f<frame>.bmp   キャプチャ (1920x1080, 24bit ボトムアップ)
//    <out>/taatest_log.txt           CSV 1 フレーム 1 行 (ヘッダ行なし)。列:
//      frame,t,R,S,H,P,method,pass,quality,format,
//      jitter_index,jitter_n,jitter_x,jitter_y,mip_bias,
//      bCameraCut,bPrevTransformsReset,bLumenHistoryValid,bFogHistoryValid,bHistoryValidThisFrame,
//      velocity_draws,taa_ran,merge,free_srv,free_rtv,deferred_queue,vram_mb,resizes,selftest_failures,
//      cam_x,cam_y,cam_z,cam_pitch,cam_yaw,cam_roll,capture,ae_exposure,ae_avg_luminance,
//      num_visible,frame_ms
//      (R / S / H はビューファミリの今フレーム値、P は後段が実際に処理した解像度。
//       pass / quality / format は TAA を実行しない構成では "-"。selftest_failures は -1 = 未実行。
//       カメラは World.Tick 後の位置 [m] / 回転 [deg]。ae_exposure / ae_avg_luminance は AutoExposure の
//       READBACK 値 (露出スケール / 測光した平均輝度。このフレームで AutoExposure が書いた値
//       (PostFrame で WaitGPU 後に読む)。TAA ハーフ解像度出力 (AutoExposure の入力) の検証用)。
//       num_visible は FCullingStats::NumVisible (NoAA 行列による視錐台カリング後の可視プリミティブ数。
//       ジッタでカリングが揺れないことの検証用)。frame_ms は Time::GetMeasuredDeltaTime (直前のフレーム
//       間隔の実測値 [ms]。固定 dt とは無関係。Present は垂直同期 (SyncInterval 1) なので表示の
//       リフレッシュ間隔が下限))
//    "# " で始まる行はコメント (コマンドラインの注記 / 上書き指定時の実効 FAntiAliasingParams /
//    自己テストの出力)。通常の実行 (注記なし) のログはフレーム行のみ
//
//  終了: フレーム N の後に FlushScreenshots -> ShouldExit = true。
//    終了コード 0 = 成功, 1 = 自己テスト失敗, 2 = コマンドライン不正, 3 = BMP 書き出し失敗
//    (複数該当時は最初に確定した失敗を返す)
//
//  呼び出し順 (GameManager):
//    コンストラクタ (コマンドライン解析) -> Begin: SettingsManager::Initialize の後に OnBegin
//    -> 毎フレーム Update: PreWorldTick (World.Tick の前) -> Draw: EndFrame の後に PostFrame
// ============================================================

class FTemporalAATestDriver
{
public:
	enum class EScenario : int
	{
		Static = 0,
		Pan,
		Rotate,
		Cut,
		LargeMove,
		Realloc,
		Resize,
		MoveObj,
		Sky,
		AAToggle,
		Translucent,
		QSwitch,
	};

	enum EExitCode : int
	{
		ExitSuccess = 0,
		ExitSelfTestFailed = 1,
		ExitBadCommandLine = 2,
		ExitCaptureWriteFailed = 3,
	};

	// ワールドのスポーン順インデックス (GameManager のコンストラクタ)
	static constexpr int kCameraActorIndex = 5;
	static constexpr int kTableActorIndex = 8;		// moveobj (AStaticMeshActor "Table")
	static constexpr int kCatActorIndex = 9;		// translucent (AStaticMeshActor "Cat")

	// translucent シナリオ: 猫のスロット 0 の不透明度
	static constexpr float kTranslucentOpacity = 0.5f;

	// moveobj シナリオ: この番号のフレームまで動かし、以降はその位置で止める
	static constexpr int kMoveObjLastMovingFrame = 90;

	// テストの出力解像度 O (= バックバッファ)。§0.1 の固定値
	static constexpr int kOutputWidth = 1920;
	static constexpr int kOutputHeight = 1080;

	// cut / largemove / aatoggle / qswitch シナリオのイベントフレーム
	static constexpr int kEventFrame = 60;

	// コマンドラインに -taatest キーがあるか (wWinMain がウィンドウ生成前に参照する。
	// テストモードはバックバッファが画面サイズ / DPI 設定に左右されないよう
	// 1920x1080 クライアントのポップアップウィンドウで起動する)
	static bool IsTestCommandLine(const wchar_t* CmdLine);

	explicit FTemporalAATestDriver(const wchar_t* CmdLine);
	~FTemporalAATestDriver();

	FTemporalAATestDriver(const FTemporalAATestDriver&) = delete;
	FTemporalAATestDriver& operator=(const FTemporalAATestDriver&) = delete;

	// -taatest が指定されているか (ImGui ウィンドウ非構築 / F9 無効の判定にも使う)
	bool IsActive() const { return m_bActive; }
	// メインループを抜けるべきか (最終フレーム後 / コマンドライン不正)
	bool ShouldExit() const { return m_bExitRequested; }
	// wWinMain の戻り値。テスト途中で閉じられた場合は 3 (キャプチャ未完了)
	int  GetExitCode() const;

	// SettingsManager::Initialize の後: 上書き適用 / INI 保存停止 / 固定 dt / カメラ入力停止 / ログ作成
	void OnBegin(UWorld& World, FSceneRenderer& Renderer, SettingsManager& Settings);
	// Update 内、World.Tick の前: フレーム番号を進め、シナリオのトランスフォームとキャプチャ要求を適用
	void PreWorldTick(FSceneRenderer& Renderer);
	// Draw 内、EndFrame の後: ログ 1 行、最終フレームならキャプチャを書き出して終了要求
	void PostFrame(FSceneRenderer& Renderer, RenderManager& RHI);

private:
	// ---- コマンドライン ----
	bool        m_bActive = false;
	bool        m_bCommandLineError = false;
	std::string m_ScenarioName;
	EScenario   m_Scenario = EScenario::Static;
	int         m_NumFrames = 120;
	std::vector<int> m_CaptureFrames;			// 昇順・重複なし (空 = 最終フレーム)
	std::wstring m_OutDir = L"Saved/TAATest";
	float       m_FixedDeltaTime = 1.0f / 60.0f;
	bool        m_bGrain = false;				// -grain (テストモード既定 0)
	int         m_DOFOverride = -1;				// -dof (-1 = 変更なし)
	bool        m_bSelfTest = false;			// -taaselftest
	int         m_ReallocPeriod = 90;			// -reallocperiod (realloc シナリオの再確保間隔 [フレーム])
	bool        m_bResponsive = false;			// -responsive (translucent シナリオの bEnableResponsiveAA)

	// FAntiAliasingParams の上書き (-aa / -sp / ...)。OnBegin で INI 適用後に順に適用する
	std::vector<std::function<void(FAntiAliasingParams&)>> m_AAOverrides;
	// FTemporalAADebugSettings の上書き (-forcejitter / -nojitter / -overrideindex / -debugvis / -visscale / -ftwmode)。OnBegin で適用する
	std::vector<std::function<void(FTemporalAADebugSettings&)>> m_DebugOverrides;

	// OnBegin より前 (コンストラクタ) に出たメッセージ。ログファイル作成後に書き出す
	std::vector<std::string> m_PendingMessages;

	// ---- 実行状態 ----
	int   m_Frame = 0;							// 現在のフレーム番号 (1 始まり)
	bool  m_bExitRequested = false;
	bool  m_bFinished = false;					// 最終フレームまで完走した
	int   m_ExitCode = ExitSuccess;
	bool  m_bCaptureThisFrame = false;

	ACameraActor*     m_Camera = nullptr;
	DirectX::XMFLOAT3 m_C0Location = { 0.0f, 0.0f, 0.0f };
	DirectX::XMFLOAT3 m_C0Rotation = { 0.0f, 0.0f, 0.0f };	// [rad] x = pitch, y = yaw, z = roll

	AStaticMeshActor* m_MovingActor = nullptr;				// moveobj: Table (スポーン順 8) / translucent: Cat (スポーン順 9)
	DirectX::XMFLOAT3 m_MovingActorBase = { 0.0f, 0.0f, 0.0f };	// INI 適用後の位置 (円運動の基準)

	FILE* m_Log = nullptr;

	void ParseCommandLine(const wchar_t* CmdLine);
	// FAntiAliasingParams を書くキーなら解析して m_AAOverrides に積み true を返す
	bool ParseAntiAliasingOverride(const std::wstring& Key, const std::wstring& Value);
	// FTemporalAADebugSettings を書くキーなら解析して m_DebugOverrides に積み true を返す
	bool ParseDebugOverride(const std::wstring& Key, const std::wstring& Value);
	void Message(const std::string& Text);		// OutputDebugStringA + (ログ作成後なら) "# " 行
	void CommandLineError(const std::string& Text);
	void SetFailure(int ExitCode);				// 最初に確定した失敗コードを保持 (終了はしない)
	void RequestExit(int ExitCode);

	bool IsCaptureFrame(int Frame) const;
	std::string MakeCapturePath(int Frame) const;
	void DrainSelfTestLog(FSceneRenderer& Renderer);
	void Finish(FSceneRenderer& Renderer);
};
