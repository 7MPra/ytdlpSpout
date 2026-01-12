// =============================================================================
// BeatDetector.cpp - エネルギーベースのビート検出実装
// =============================================================================

#include "BeatDetector.h"
#include "utils/Logger.h"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <map>

namespace ytdlpspout {

// =============================================================================
// 定数
// =============================================================================

namespace {
    constexpr int kDefaultFrameSize = 1024;         // フレームサイズ
    constexpr int kDefaultHopSize = 512;            // ホップサイズ
    constexpr double kPeakThresholdMultiplier = 1.5; // ピーク検出閾値の倍率
    constexpr double kMinBPM = 60.0;                 // 最小BPM
    constexpr double kMaxBPM = 200.0;                // 最大BPM
    constexpr double kLowPassAlpha = 0.3;            // ローパスフィルタ係数
    constexpr int kMinBeatsForBPM = 4;               // BPM推定に必要な最小ビート数
}

// =============================================================================
// コンストラクタ/デストラクタ
// =============================================================================

BeatDetector::BeatDetector(int sampleRate)
    : sampleRate_(sampleRate)
    , frameSize_(kDefaultFrameSize)
    , hopSize_(kDefaultHopSize)
    , totalSamplesProcessed_(0.0)
{
    buffer_.reserve(frameSize_ * 2);
}

// =============================================================================
// 公開メソッド
// =============================================================================

void BeatDetector::ProcessSamples(const float* samples, int count) {
    if (samples == nullptr || count <= 0) {
        return;
    }
    
    // バッファに追加
    buffer_.insert(buffer_.end(), samples, samples + count);
    
    // フレーム単位で処理
    while (buffer_.size() >= static_cast<size_t>(frameSize_)) {
        // エネルギー計算
        float energy = CalculateEnergy(buffer_.data(), frameSize_);
        energyHistory_.push_back(energy);
        
        // ホップサイズ分進める
        buffer_.erase(buffer_.begin(), buffer_.begin() + hopSize_);
        totalSamplesProcessed_ += hopSize_;
    }
    
    // ピーク検出（十分なデータがある場合）
    if (energyHistory_.size() >= 10) {
        DetectPeaks();
    }
}

float BeatDetector::GetEstimatedBPM() const {
    if (beatPositions_.size() < static_cast<size_t>(kMinBeatsForBPM)) {
        return 0.0f;
    }
    
    return EstimateBPMFromIntervals();
}

std::vector<double> BeatDetector::GetBeatPositions() const {
    return beatPositions_;
}

void BeatDetector::Reset() {
    buffer_.clear();
    energyHistory_.clear();
    beatPositions_.clear();
    totalSamplesProcessed_ = 0.0;
}

// =============================================================================
// 内部メソッド
// =============================================================================

float BeatDetector::CalculateEnergy(const float* samples, int count) const {
    if (count <= 0) {
        return 0.0f;
    }
    
    // RMSエネルギー計算
    double sumSquared = 0.0;
    for (int i = 0; i < count; ++i) {
        sumSquared += samples[i] * samples[i];
    }
    
    return static_cast<float>(std::sqrt(sumSquared / count));
}

void BeatDetector::DetectPeaks() {
    if (energyHistory_.size() < 3) {
        return;
    }
    
    // 平滑化したエネルギーを計算
    std::vector<double> smoothedEnergy = energyHistory_;
    ApplyLowPassFilter(smoothedEnergy, kLowPassAlpha);
    
    // 移動平均を計算（局所的な閾値）
    size_t windowSize = std::min(static_cast<size_t>(20), energyHistory_.size() / 2);
    if (windowSize < 3) {
        windowSize = 3;
    }
    
    // 新しいビートのみを検出（最後に検出したビート以降から）
    size_t startIndex = 0;
    if (!beatPositions_.empty()) {
        double lastBeatTime = beatPositions_.back();
        double frameTime = static_cast<double>(hopSize_) / sampleRate_;
        startIndex = static_cast<size_t>(lastBeatTime / frameTime) + 1;
    }
    
    if (startIndex >= smoothedEnergy.size()) {
        return;
    }
    
    for (size_t i = std::max(startIndex, windowSize); i < smoothedEnergy.size() - 1; ++i) {
        // 局所平均を計算
        double localSum = 0.0;
        for (size_t j = i - windowSize; j < i; ++j) {
            localSum += smoothedEnergy[j];
        }
        double localMean = localSum / windowSize;
        
        // ピーク検出条件
        // 1. 現在値が局所平均より十分大きい
        // 2. 現在値が前後より大きい（局所最大）
        double threshold = localMean * kPeakThresholdMultiplier;
        
        if (smoothedEnergy[i] > threshold &&
            smoothedEnergy[i] > smoothedEnergy[i - 1] &&
            smoothedEnergy[i] > smoothedEnergy[i + 1]) {
            
            // フレームインデックスを秒に変換
            double beatTime = static_cast<double>(i * hopSize_) / sampleRate_;
            
            // 既に近くにビートがない場合のみ追加
            // （BPM 200 で 0.3秒ごと = 最小間隔）
            double minInterval = 60.0 / kMaxBPM;
            
            if (beatPositions_.empty() || 
                (beatTime - beatPositions_.back()) >= minInterval) {
                beatPositions_.push_back(beatTime);
            }
        }
    }
}

float BeatDetector::EstimateBPMFromIntervals() const {
    if (beatPositions_.size() < 2) {
        return 0.0f;
    }
    
    // ビート間隔を計算
    std::vector<double> intervals;
    intervals.reserve(beatPositions_.size() - 1);
    
    for (size_t i = 1; i < beatPositions_.size(); ++i) {
        double interval = beatPositions_[i] - beatPositions_[i - 1];
        
        // 妥当な範囲のインターバルのみ使用
        double bpm = 60.0 / interval;
        if (bpm >= kMinBPM && bpm <= kMaxBPM) {
            intervals.push_back(interval);
        }
    }
    
    if (intervals.empty()) {
        return 0.0f;
    }
    
    // ヒストグラムベースでモードを探す（最頻値を使用）
    // インターバルを10ms単位でクォンタイズ
    std::map<int, int> histogram;
    for (double interval : intervals) {
        int quantized = static_cast<int>(interval * 100);  // 10msの精度
        histogram[quantized]++;
    }
    
    // 最頻値を探す
    int maxCount = 0;
    int bestInterval = 0;
    for (const auto& [interval, count] : histogram) {
        if (count > maxCount) {
            maxCount = count;
            bestInterval = interval;
        }
    }
    
    if (bestInterval == 0) {
        return 0.0f;
    }
    
    // インターバルからBPMに変換
    double intervalSec = bestInterval / 100.0;
    double bpm = 60.0 / intervalSec;
    
    // BPM範囲を正規化（ダブルタイム/ハーフタイム調整）
    while (bpm < kMinBPM && bpm > 0) {
        bpm *= 2.0;
    }
    while (bpm > kMaxBPM) {
        bpm /= 2.0;
    }
    
    return static_cast<float>(bpm);
}

void BeatDetector::ApplyLowPassFilter(std::vector<double>& data, double alpha) const {
    if (data.size() < 2) {
        return;
    }
    
    // 単純な1次IIRローパスフィルタ
    for (size_t i = 1; i < data.size(); ++i) {
        data[i] = alpha * data[i] + (1.0 - alpha) * data[i - 1];
    }
}

} // namespace ytdlpspout
