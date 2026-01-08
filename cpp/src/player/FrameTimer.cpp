// =============================================================================
// FrameTimer.cpp - フレームタイミング制御 実装
// =============================================================================

#include "FrameTimer.h"
#include "utils/Logger.h"

#include <Windows.h>
#include <mmsystem.h>
#include <thread>
#include <algorithm>

#pragma comment(lib, "winmm.lib")

namespace ytdlpspout {

// =============================================================================
// 内部実装クラス
// =============================================================================

struct FrameTimer::Impl {
    // 設定
    double targetFPS = 30.0;
    int64_t frameIntervalMicros = 33333;  // 1/30秒

    // 状態
    bool started = false;
    bool paused = false;

    // タイミング
    std::chrono::steady_clock::time_point startTime;
    std::chrono::steady_clock::time_point lastFrameTime;
    std::chrono::steady_clock::time_point pauseStartTime;
    int64_t pausedDuration = 0;  // マイクロ秒

    // 統計
    uint64_t frameCount = 0;
    uint64_t droppedFrames = 0;
    int64_t accumulatedDelay = 0;

    // FPS計算用
    std::chrono::steady_clock::time_point fpsCalcTime;
    uint64_t fpsCalcFrameCount = 0;
    double actualFPS = 0.0;

    // 高精度スリープ用（Windows固有）
    HANDLE timerHandle = nullptr;

    Impl() {
        // Windowsの高精度ウェイタブルタイマーを作成
        timerHandle = CreateWaitableTimerExW(
            nullptr,
            nullptr,
            CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
            TIMER_ALL_ACCESS
        );

        if (!timerHandle) {
            // フォールバック: 通常のタイマー
            timerHandle = CreateWaitableTimerW(nullptr, TRUE, nullptr);
        }
    }

    ~Impl() {
        if (timerHandle) {
            CloseHandle(timerHandle);
        }
    }

    // 高精度スリープ
    void PreciseSleep(int64_t microseconds) {
        if (microseconds <= 0) {
            return;
        }

        if (timerHandle) {
            // 100ナノ秒単位（負の値は相対時間）
            LARGE_INTEGER dueTime;
            dueTime.QuadPart = -static_cast<LONGLONG>(microseconds * 10);

            if (SetWaitableTimer(timerHandle, &dueTime, 0, nullptr, nullptr, FALSE)) {
                WaitForSingleObject(timerHandle, INFINITE);
                return;
            }
        }

        // フォールバック: std::this_thread::sleep_for
        std::this_thread::sleep_for(std::chrono::microseconds(microseconds));
    }
};

// =============================================================================
// コンストラクタ・デストラクタ
// =============================================================================

FrameTimer::FrameTimer() : m_impl(std::make_unique<Impl>()) {
    // タイマー精度を最高に設定（Windows）
    timeBeginPeriod(1);
}

FrameTimer::~FrameTimer() {
    // タイマー精度を元に戻す
    timeEndPeriod(1);
}

// =============================================================================
// 設定
// =============================================================================

void FrameTimer::SetTargetFPS(double fps) {
    if (fps <= 0.0) {
        fps = 30.0;
    }

    m_impl->targetFPS = fps;
    m_impl->frameIntervalMicros = static_cast<int64_t>(1000000.0 / fps);

    LOG_DEBUG("Target FPS set to {:.2f} (interval: {} us)", fps, m_impl->frameIntervalMicros);
}

double FrameTimer::GetTargetFPS() const {
    return m_impl->targetFPS;
}

int64_t FrameTimer::GetFrameIntervalMicros() const {
    return m_impl->frameIntervalMicros;
}

// =============================================================================
// タイミング制御
// =============================================================================

void FrameTimer::Start() {
    auto now = std::chrono::steady_clock::now();
    
    m_impl->startTime = now;
    m_impl->lastFrameTime = now;
    m_impl->fpsCalcTime = now;
    m_impl->started = true;
    m_impl->paused = false;
    m_impl->pausedDuration = 0;
    m_impl->frameCount = 0;
    m_impl->droppedFrames = 0;
    m_impl->accumulatedDelay = 0;
    m_impl->fpsCalcFrameCount = 0;

    LOG_DEBUG("FrameTimer started");
}

void FrameTimer::Reset() {
    Start();
}

void FrameTimer::SeekTo(double pts) {
    auto now = std::chrono::steady_clock::now();
    
    // startTimeをPTS分だけ過去にオフセットする
    // これにより、WaitUntilPTS(pts) が呼ばれたとき、
    // elapsedMicros ≒ pts となり、待機時間が0になる
    int64_t ptsMicros = static_cast<int64_t>(pts * 1000000.0);
    m_impl->startTime = now - std::chrono::microseconds(ptsMicros);
    
    // シーク後は1フレーム分待機させるため、lastFrameTimeを
    // 「今から1フレーム間隔を引いた時点」ではなく「現在」に設定
    // WaitForNextFrame()で targetTime = now + interval となり、
    // 次フレームまで適切に待機する
    m_impl->lastFrameTime = now;
    m_impl->pausedDuration = 0;
    m_impl->accumulatedDelay = 0;
    
    // FPS計算もリセット（シーク後の一時的な高FPSを防ぐ）
    m_impl->fpsCalcTime = now;
    m_impl->fpsCalcFrameCount = 0;
    
    LOG_DEBUG("FrameTimer seeked to {:.3f}s", pts);
}

int64_t FrameTimer::WaitForNextFrame() {
    if (!m_impl->started || m_impl->paused) {
        return 0;
    }

    auto now = std::chrono::steady_clock::now();

    // 次のフレームの目標時刻を計算（前回の目標時刻 + インターバル）
    auto targetTime = m_impl->lastFrameTime + 
                      std::chrono::microseconds(m_impl->frameIntervalMicros);

    // 現在時刻と目標時刻の差を計算
    auto waitDuration = std::chrono::duration_cast<std::chrono::microseconds>(
        targetTime - now).count();

    if (waitDuration > 0) {
        // 待機が必要
        m_impl->PreciseSleep(waitDuration);
        m_impl->accumulatedDelay = 0;
        // 次のフレームは目標時刻基準で計算（ドリフト防止）
        m_impl->lastFrameTime = targetTime;
    } else {
        // 遅延している
        m_impl->accumulatedDelay += (-waitDuration);
        
        // 1フレーム以上遅れている場合
        if (-waitDuration > m_impl->frameIntervalMicros) {
            int64_t droppedCount = -waitDuration / m_impl->frameIntervalMicros;
            m_impl->droppedFrames += static_cast<uint64_t>(droppedCount);
            // 大幅に遅れている場合は現在時刻にリセット
            m_impl->lastFrameTime = now;
        } else {
            // 軽度の遅延は目標時刻を維持
            m_impl->lastFrameTime = targetTime;
        }
    }

    m_impl->frameCount++;

    // FPS計算（1秒ごと）
    auto fpsElapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - m_impl->fpsCalcTime).count();
    
    if (fpsElapsed >= 1000) {
        uint64_t framesDelta = m_impl->frameCount - m_impl->fpsCalcFrameCount;
        m_impl->actualFPS = static_cast<double>(framesDelta) * 1000.0 / fpsElapsed;
        m_impl->fpsCalcTime = now;
        m_impl->fpsCalcFrameCount = m_impl->frameCount;
    }

    return waitDuration > 0 ? waitDuration : 0;
}

int64_t FrameTimer::WaitUntilPTS(double pts) {
    if (!m_impl->started || m_impl->paused) {
        return 0;
    }

    auto now = std::chrono::steady_clock::now();

    // 再生開始からの経過時間を計算
    auto elapsedMicros = std::chrono::duration_cast<std::chrono::microseconds>(
        now - m_impl->startTime).count() - m_impl->pausedDuration;

    // PTSをマイクロ秒に変換
    int64_t ptsMicros = static_cast<int64_t>(pts * 1000000.0);

    // 待機時間を計算
    int64_t waitDuration = ptsMicros - elapsedMicros;

    LOG_TRACE("PTS sync: pts={:.3f}s, elapsed={:.3f}s, wait={:.3f}ms", 
              pts, elapsedMicros / 1000000.0, waitDuration / 1000.0);

    if (waitDuration > 0) {
        // 待機が必要
        m_impl->PreciseSleep(waitDuration);
        m_impl->accumulatedDelay = 0;
    } else {
        // 遅延している
        m_impl->accumulatedDelay = -waitDuration;
        
        // 1フレーム以上遅れている場合
        if (-waitDuration > m_impl->frameIntervalMicros) {
            int64_t droppedCount = -waitDuration / m_impl->frameIntervalMicros;
            m_impl->droppedFrames += static_cast<uint64_t>(droppedCount);
        }
    }

    m_impl->frameCount++;

    // FPS計算（1秒ごと）
    auto fpsElapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - m_impl->fpsCalcTime).count();
    
    if (fpsElapsed >= 1000) {
        uint64_t framesDelta = m_impl->frameCount - m_impl->fpsCalcFrameCount;
        m_impl->actualFPS = static_cast<double>(framesDelta) * 1000.0 / fpsElapsed;
        m_impl->fpsCalcTime = now;
        m_impl->fpsCalcFrameCount = m_impl->frameCount;
    }

    return waitDuration > 0 ? waitDuration : 0;
}

bool FrameTimer::ShouldSkipFrame() const {
    // 2フレーム以上の遅延がある場合はスキップ推奨
    return m_impl->accumulatedDelay > m_impl->frameIntervalMicros * 2;
}

// =============================================================================
// 一時停止
// =============================================================================

void FrameTimer::Pause() {
    if (!m_impl->paused) {
        m_impl->paused = true;
        m_impl->pauseStartTime = std::chrono::steady_clock::now();
        LOG_TRACE("FrameTimer paused");
    }
}

void FrameTimer::Resume() {
    if (m_impl->paused) {
        auto now = std::chrono::steady_clock::now();
        auto pauseDuration = std::chrono::duration_cast<std::chrono::microseconds>(
            now - m_impl->pauseStartTime).count();
        
        m_impl->pausedDuration += pauseDuration;
        m_impl->lastFrameTime = now;
        m_impl->paused = false;

        LOG_TRACE("FrameTimer resumed (paused for {} us)", pauseDuration);
    }
}

bool FrameTimer::IsPaused() const {
    return m_impl->paused;
}

// =============================================================================
// 統計
// =============================================================================

double FrameTimer::GetActualFPS() const {
    return m_impl->actualFPS;
}

uint64_t FrameTimer::GetFrameCount() const {
    return m_impl->frameCount;
}

uint64_t FrameTimer::GetDroppedFrames() const {
    return m_impl->droppedFrames;
}

int64_t FrameTimer::GetAccumulatedDelay() const {
    return m_impl->accumulatedDelay;
}

} // namespace ytdlpspout
