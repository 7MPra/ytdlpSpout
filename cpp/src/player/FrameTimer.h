// =============================================================================
// FrameTimer.h - フレームタイミング制御
// =============================================================================
//
// 機能:
//   - PTS/DTSに基づく表示タイミング調整
//   - 正確なフレームレート維持
//   - 一時停止・再開対応
//   - 高精度タイマー使用
//
// =============================================================================

#pragma once

#include <memory>
#include <chrono>

namespace ytdlpspout {

/// @brief フレームタイミング制御クラス
/// @details 動画のフレームレートに合わせたタイミング制御
class FrameTimer {
public:
    FrameTimer();
    ~FrameTimer();

    // コピー禁止
    FrameTimer(const FrameTimer&) = delete;
    FrameTimer& operator=(const FrameTimer&) = delete;

    // =========================================================================
    // 設定
    // =========================================================================

    /// @brief 目標FPSを設定
    /// @param fps フレームレート
    void SetTargetFPS(double fps);

    /// @brief 目標FPSを取得
    /// @return 設定されたフレームレート
    double GetTargetFPS() const;

    /// @brief フレーム間隔を取得
    /// @return マイクロ秒単位のフレーム間隔
    int64_t GetFrameIntervalMicros() const;

    // =========================================================================
    // タイミング制御
    // =========================================================================

    /// @brief タイマーを開始
    void Start();

    /// @brief タイマーをリセット
    void Reset();

    /// @brief 次のフレームまで待機
    /// @return 実際に待機したマイクロ秒数
    int64_t WaitForNextFrame();

    /// @brief 指定したPTSまで待機（PTS同期）
    /// @param pts 表示時刻（秒）
    /// @return 実際に待機したマイクロ秒数
    int64_t WaitUntilPTS(double pts);

    /// @brief フレームをスキップすべきか判定
    /// @return スキップすべき場合true
    bool ShouldSkipFrame() const;

    // =========================================================================
    // 一時停止
    // =========================================================================

    /// @brief 一時停止
    void Pause();

    /// @brief 再開
    void Resume();

    /// @brief 一時停止中かどうか
    /// @return 一時停止中の場合true
    bool IsPaused() const;

    // =========================================================================
    // シーク対応
    // =========================================================================

    /// @brief シーク時のPTSオフセットを設定
    /// @param pts シーク先のPTS（秒）
    /// @details startTimeをPTS分だけ過去にオフセットし、WaitUntilPTSが正しく動作するようにする
    void SeekTo(double pts);

    // =========================================================================
    // 統計
    // =========================================================================

    /// @brief 実際のFPSを取得（直近の平均）
    /// @return 実測フレームレート
    double GetActualFPS() const;

    /// @brief 処理されたフレーム数を取得
    /// @return フレーム数
    uint64_t GetFrameCount() const;

    /// @brief ドロップしたフレーム数を取得
    /// @return ドロップフレーム数
    uint64_t GetDroppedFrames() const;

    /// @brief 累積遅延を取得
    /// @return マイクロ秒単位の遅延
    int64_t GetAccumulatedDelay() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace ytdlpspout
