// =============================================================================
// BeatDetector.h - エネルギーベースのビート検出
// =============================================================================
//
// 機能:
//   - フレームごとのエネルギー計算
//   - ローパスフィルタで平滑化
//   - ピーク検出でビート位置特定
//   - ビート間隔からBPM推定
//
// 使用例:
//   BeatDetector detector(44100);
//   detector.ProcessSamples(audioData, numSamples);
//   float bpm = detector.GetEstimatedBPM();
//   auto beats = detector.GetBeatPositions();
//
// =============================================================================

#pragma once

#include <vector>
#include <deque>

namespace ytdlpspout {

/// @brief シンプルなエネルギーベースのビート検出器
/// @details Aubioなどの外部ライブラリを使わず、基本的なDSPでビート検出を行う
class BeatDetector {
public:
    /// @brief コンストラクタ
    /// @param sampleRate サンプルレート（デフォルト44100Hz）
    explicit BeatDetector(int sampleRate = 44100);
    
    /// @brief デストラクタ
    ~BeatDetector() = default;
    
    /// @brief サンプルを処理してビート検出
    /// @param samples オーディオサンプル（モノラル float）
    /// @param count サンプル数
    void ProcessSamples(const float* samples, int count);
    
    /// @brief 推定BPMを取得
    /// @return BPM（ビート未検出時は0.0f）
    float GetEstimatedBPM() const;
    
    /// @brief 検出されたビート位置を取得
    /// @return ビート位置のリスト（秒単位）
    std::vector<double> GetBeatPositions() const;
    
    /// @brief 状態をリセット
    void Reset();
    
private:
    // =========================================================================
    // 設定
    // =========================================================================
    
    int sampleRate_;                    ///< サンプルレート
    int frameSize_;                     ///< エネルギー計算フレームサイズ
    int hopSize_;                       ///< フレーム間のホップサイズ
    
    // =========================================================================
    // 状態
    // =========================================================================
    
    std::vector<float> buffer_;         ///< 入力バッファ
    std::vector<double> energyHistory_; ///< エネルギー履歴
    std::vector<double> beatPositions_; ///< 検出されたビート位置
    double totalSamplesProcessed_;      ///< 処理済みサンプル総数
    
    // =========================================================================
    // 内部メソッド
    // =========================================================================
    
    /// @brief フレームのエネルギーを計算
    float CalculateEnergy(const float* samples, int count) const;
    
    /// @brief エネルギー履歴からピークを検出
    void DetectPeaks();
    
    /// @brief ビート間隔からBPMを推定
    float EstimateBPMFromIntervals() const;
    
    /// @brief ローパスフィルタを適用
    void ApplyLowPassFilter(std::vector<double>& data, double alpha) const;
};

} // namespace ytdlpspout
