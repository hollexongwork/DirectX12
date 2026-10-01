#pragma once
#include <chrono>

class Time
{
private:
    static std::chrono::steady_clock::time_point lastFrameTime;
    static float time;
    static float unscaledTime;
    static float deltaTime;
    static float unscaledDeltaTime;
    static float timeScale;
    static float fixedDeltaTime;    // 0 = 無効 (実測値を使う)
    static float measuredDeltaTime; // 実測のフレーム間隔 [s] (固定 dt / MAX_DELTA クランプの影響を受けない)

public:
    Time();

    static void Update();

    static float GetTime();
    static float GetUnscaledTime();
    static float GetDeltaTime();
    static float GetUnscaledDeltaTime();
    static float GetTimeScale();
    static void  SetTimeScale(float scale);
    static void  Reset();

    // 固定デルタタイム (テストドライバ -taatest / -fixeddt)。
    // 0 以外の間は Update が実測値の代わりに unscaledDeltaTime = Seconds,
    // deltaTime = Seconds x timeScale を使う (lastFrameTime の更新は継続)。
    static void  SetFixedDeltaTime(float Seconds);

    // 直前の Update 間の実測時間 [s] (壁時計。固定デルタタイム中も実測値。
    // テストドライバの frame_ms 列 = 平均フレーム時間の計測用)
    static float GetMeasuredDeltaTime();
};