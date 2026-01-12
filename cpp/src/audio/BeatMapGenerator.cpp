// =============================================================================
// BeatMapGenerator.cpp - ビートマップ生成実装
// =============================================================================

#include "BeatMapGenerator.h"
#include "AudioDecoder.h"
#include "utils/Logger.h"

#include <algorithm>
#include <cmath>

namespace ytdlpspout {

// =============================================================================
// BeatMapGenerator
// =============================================================================

BeatMapGenerator::BeatMapGenerator()
    : cancelled_(false)
{
}

BeatMap BeatMapGenerator::Generate(const std::string& audioPath,
                                   ProgressCallback progressCb) {
    cancelled_ = false;
    
    BeatMap result;
    
    // 進捗報告：開始
    if (progressCb) {
        progressCb(0.0);
    }
    
    // AudioDecoderを使用してファイルを開く
    AudioDecoder decoder;
    if (!decoder.Open(audioPath)) {
        LOG_ERROR("BeatMapGenerator: Failed to open audio file: {}", audioPath);
        if (progressCb) {
            progressCb(1.0);
        }
        return result;
    }
    
    // 音声情報取得
    int sampleRate = decoder.GetSampleRate();
    int channels = decoder.GetChannels();
    double duration = decoder.GetDuration();
    
    if (sampleRate <= 0 || duration <= 0) {
        LOG_ERROR("BeatMapGenerator: Invalid audio parameters");
        if (progressCb) {
            progressCb(1.0);
        }
        return result;
    }
    
    LOG_INFO("BeatMapGenerator: Processing {} ({}Hz, {}ch, {:.1f}s)",
             audioPath, sampleRate, channels, duration);
    
    // BeatDetectorを初期化
    BeatDetector detector(sampleRate);
    
    // チャンク単位でデコード・処理
    constexpr int kChunkSize = 4096;
    std::vector<float> samples(kChunkSize);
    
    double processedDuration = 0.0;
    
    while (!cancelled_ && !decoder.IsEOF()) {
        int samplesRead = decoder.GetSamples(samples.data(), kChunkSize);
        if (samplesRead <= 0) {
            break;
        }
        
        // AudioDecoderは既にモノラル・float32で出力するため、直接渡せる
        detector.ProcessSamples(samples.data(), samplesRead);
        
        // 進捗報告
        processedDuration += static_cast<double>(samplesRead) / sampleRate;
        if (progressCb && duration > 0) {
            double progress = std::min(processedDuration / duration, 0.99);
            progressCb(progress);
        }
    }
    
    // キャンセルされた場合
    if (cancelled_) {
        LOG_WARN("BeatMapGenerator: Cancelled");
        if (progressCb) {
            progressCb(1.0);
        }
        return result;
    }
    
    // 結果を取得
    result.bpm = detector.GetEstimatedBPM();
    result.duration = duration;
    
    // ビート位置をBeatInfoに変換
    auto positions = detector.GetBeatPositions();
    result.beats.reserve(positions.size());
    
    for (size_t i = 0; i < positions.size(); ++i) {
        BeatInfo beat;
        beat.timestamp = positions[i];
        beat.confidence = 0.9f;  // 固定値（今後改善可能）
        beat.beatNumber = i % 4;
        beat.isDownbeat = (i % 4 == 0);
        result.beats.push_back(beat);
    }
    
    // BPM信頼度を計算（ビート数に基づく簡易計算）
    if (result.bpm > 0 && result.beats.size() >= 4) {
        // 期待されるビート数との比較
        double expectedBeats = (result.bpm / 60.0) * duration;
        double ratio = static_cast<double>(result.beats.size()) / expectedBeats;
        result.bpmConfidence = std::min(1.0f, static_cast<float>(ratio));
    } else {
        result.bpmConfidence = 0.0f;
    }
    
    LOG_INFO("BeatMapGenerator: Detected BPM={:.1f} (confidence={:.2f}), {} beats",
             result.bpm, result.bpmConfidence, result.beats.size());
    
    // 進捗報告：完了
    if (progressCb) {
        progressCb(1.0);
    }
    
    return result;
}

std::future<BeatMap> BeatMapGenerator::GenerateAsync(const std::string& audioPath,
                                                     ProgressCallback progressCb) {
    return std::async(std::launch::async, [this, audioPath, progressCb]() {
        return Generate(audioPath, progressCb);
    });
}

void BeatMapGenerator::Cancel() {
    cancelled_ = true;
}

bool BeatMapGenerator::IsCancelled() const {
    return cancelled_;
}

// =============================================================================
// BeatJumpController
// =============================================================================

BeatJumpController::BeatJumpController()
    : beatMap_(nullptr)
{
}

void BeatJumpController::SetBeatMap(std::shared_ptr<BeatMap> beatMap) {
    beatMap_ = beatMap;
}

double BeatJumpController::CalculateJumpTarget(double currentTime, JumpUnit unit, bool forward) {
    if (!beatMap_ || beatMap_->beats.empty()) {
        return currentTime;
    }
    
    // 現在位置に最も近いビートを探す
    const BeatInfo* nearestBeat = beatMap_->GetNearestBeat(currentTime);
    if (!nearestBeat) {
        return currentTime;
    }
    
    // 現在のビートインデックスを探す
    int currentIndex = -1;
    for (size_t i = 0; i < beatMap_->beats.size(); ++i) {
        if (&beatMap_->beats[i] == nearestBeat) {
            currentIndex = static_cast<int>(i);
            break;
        }
    }
    
    if (currentIndex < 0) {
        return currentTime;
    }
    
    // ジャンプ先インデックスを計算
    int jumpBeats = UnitToBeats(unit);
    int targetIndex = forward ? (currentIndex + jumpBeats) : (currentIndex - jumpBeats);
    
    // 範囲チェック
    if (targetIndex < 0) {
        return 0.0;
    }
    if (targetIndex >= static_cast<int>(beatMap_->beats.size())) {
        // duration または 最後のビートを返す
        return std::min(beatMap_->duration, beatMap_->beats.back().timestamp);
    }
    
    return beatMap_->beats[targetIndex].timestamp;
}

double BeatJumpController::QuantizeToNearestBeat(double currentTime) {
    if (!beatMap_ || beatMap_->beats.empty()) {
        return currentTime;
    }
    
    const BeatInfo* nearestBeat = beatMap_->GetNearestBeat(currentTime);
    if (!nearestBeat) {
        return currentTime;
    }
    
    return nearestBeat->timestamp;
}

int BeatJumpController::UnitToBeats(JumpUnit unit) const {
    switch (unit) {
        case JumpUnit::Beat_1:  return 1;
        case JumpUnit::Beat_2:  return 2;
        case JumpUnit::Beat_4:  return 4;
        case JumpUnit::Beat_8:  return 8;
        case JumpUnit::Beat_16: return 16;
        default:                return 1;
    }
}

} // namespace ytdlpspout
