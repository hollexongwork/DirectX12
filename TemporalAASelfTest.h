#pragma once

class FSceneRenderer;

// ============================================================
//  TemporalAASelfTest
//  TAA / TAAU の数式の自己テスト (§9.2, Appendix A のテストベクタ)。
//
//  実行契機 (いずれも BeginFrame 先頭。ImGui の構築中には走らない):
//    - Debug ビルドの最初の BeginFrame (FSceneRenderer のコンストラクタが要求)
//    - コマンドライン -taaselftest (テストドライバがフレーム 1 に要求)
//    - ImGui "Run Self Test" ボタン
//
//  出力: "[TAA SelfTest] <id> PASS|FAIL got=... exp=..." を OutputDebugStringA と
//  FSceneRenderer の自己テストログ (テストドライバがログファイルへ移す) の両方へ書き、
//  最後に "[TAA SelfTest] n/m PASS" の集計行を書く。
//
//  CPU テスト (ジッタ / ビューファミリ): T1 Halton / T2 ジッタのサンプル列 / T3 射影ジッタ /
//                   T12 ビューファミリ (§4.2 の表, N = ジッタサンプル数を含む)
//  CPU テスト (再投影): T4 ClipToPrevClip (原点) /
//                   T4b ClipToPrevClip (原点から離れた位置, カメラ相対 double 合成) /
//                   T5 回転のみの再投影 (深度非依存) / T13 LinearDepth <-> デバイス Z / T15 大きなカメラ移動
//  CPU テスト (ベロシティ): T6 ベロシティのエンコード (UNORM16 量子化 / ±2 クランプ / 復号) /
//                   T16 FSceneVelocityData のライフサイクル (テレポート / 停止 / 削除)
//  CPU テスト (Main カーネル): T7 Main 構成のサンプル重み (ガウス / Catmull-Rom + 悪条件ガード) /
//                   T14 YCoCg / HDR 重み / 加重 lerp / 量子化誤差 (HLSL ヘルパーの CPU 鏡像)
//  CPU テスト (TAAU カーネル): T8 TAAU 空間重み曲線 / T9 TAAU の入力写像 (K / dKO / FTW) /
//                   T10 Catmull-Rom 1D 重み / T11 Mitchell-Netravali のカーネルと正規化タップ
//  GPU パリティ (bIncludeGPU): TemporalAASelfTest_CS を 1 回ディスパッチして読み戻し、
//                   HLSL ヘルパー (標準 Z の最近傍深度 / 遠方規則を含む) が期待値と一致するか
//
//  戻り値: 失敗したテストの数 (0 = 全合格)
// ============================================================
int RunTemporalAASelfTests(FSceneRenderer& Renderer, bool bIncludeGPU);
