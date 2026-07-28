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
};