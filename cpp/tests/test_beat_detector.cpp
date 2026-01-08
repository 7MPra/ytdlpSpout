// =============================================================================
// test_beat_detector.cpp - BeatDetectorのユニットテスト
// =============================================================================

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "audio/BeatDetector.h"
#include <cmath>
#include <vector>

using namespace ytdlpspout;

// =============================================================================
// ヘルパー関数
// =============================================================================

namespace {

/// @brief サイン波を生成
std::vector<float> GenerateSineWave(int sampleRate, float frequency, float duration,
                                     float amplitude = 1.0f) {
    int numSamples = static_cast<int>(sampleRate * duration);
    std::vector<float> samples(numSamples);
    
    for (int i = 0; i < numSamples; ++i) {
        float t = static_cast<float>(i) / sampleRate;
        samples[i] = amplitude * std::sin(2.0f * 3.14159265f * frequency * t);
    }
    
    return samples;
}

/// @brief ビートパルスを生成（BPM指定）
/// @details 指定BPMでクリック/パルスを生成
std::vector<float> GenerateBeatPulses(int sampleRate, float bpm, float duration,
                                       float pulseWidth = 0.01f) {
    int numSamples = static_cast<int>(sampleRate * duration);
    std::vector<float> samples(numSamples, 0.0f);
    
    float beatInterval = 60.0f / bpm;  // 秒
    int beatIntervalSamples = static_cast<int>(beatInterval * sampleRate);
    int pulseWidthSamples = static_cast<int>(pulseWidth * sampleRate);
    
    for (int beatStart = 0; beatStart < numSamples; beatStart += beatIntervalSamples) {
        for (int i = 0; i < pulseWidthSamples && (beatStart + i) < numSamples; ++i) {
            // パルス形状（三角波）
            float t = static_cast<float>(i) / pulseWidthSamples;
            float envelope = (t < 0.5f) ? (2.0f * t) : (2.0f * (1.0f - t));
            samples[beatStart + i] = envelope;
        }
    }
    
    return samples;
}

/// @brief ビートパルス + ノイズを生成
std::vector<float> GenerateBeatPulsesWithNoise(int sampleRate, float bpm, float duration,
                                                float noiseLevel = 0.1f) {
    auto samples = GenerateBeatPulses(sampleRate, bpm, duration);
    
    // 簡易ノイズ追加（線形合同法）
    unsigned int seed = 12345;
    for (size_t i = 0; i < samples.size(); ++i) {
        seed = seed * 1103515245 + 12345;
        float noise = (static_cast<float>(seed % 10000) / 10000.0f - 0.5f) * 2.0f * noiseLevel;
        samples[i] += noise;
    }
    
    return samples;
}

} // anonymous namespace

// =============================================================================
// BeatDetector 基本テスト
// =============================================================================

TEST_SUITE("BeatDetector Basic") {
    
    TEST_CASE("Construction with default sample rate") {
        BeatDetector detector;
        
        CHECK(detector.GetEstimatedBPM() == 0.0f);
        CHECK(detector.GetBeatPositions().empty());
    }
    
    TEST_CASE("Construction with custom sample rate") {
        BeatDetector detector(48000);
        
        CHECK(detector.GetEstimatedBPM() == 0.0f);
        CHECK(detector.GetBeatPositions().empty());
    }
    
    TEST_CASE("Reset clears state") {
        BeatDetector detector;
        
        // ダミーサンプルを処理
        auto samples = GenerateBeatPulses(44100, 120.0f, 5.0f);
        detector.ProcessSamples(samples.data(), static_cast<int>(samples.size()));
        
        // リセット
        detector.Reset();
        
        CHECK(detector.GetEstimatedBPM() == 0.0f);
        CHECK(detector.GetBeatPositions().empty());
    }
}

// =============================================================================
// BeatDetector ビート検出テスト
// =============================================================================

TEST_SUITE("BeatDetector Detection") {
    
    TEST_CASE("Detect 120 BPM") {
        BeatDetector detector(44100);
        
        // 120 BPMのビートパルスを10秒分生成
        auto samples = GenerateBeatPulses(44100, 120.0f, 10.0f);
        detector.ProcessSamples(samples.data(), static_cast<int>(samples.size()));
        
        float estimatedBpm = detector.GetEstimatedBPM();
        
        // 許容誤差 ±10 BPM
        CHECK(estimatedBpm >= 110.0f);
        CHECK(estimatedBpm <= 130.0f);
    }
    
    TEST_CASE("Detect 90 BPM") {
        BeatDetector detector(44100);
        
        auto samples = GenerateBeatPulses(44100, 90.0f, 10.0f);
        detector.ProcessSamples(samples.data(), static_cast<int>(samples.size()));
        
        float estimatedBpm = detector.GetEstimatedBPM();
        
        // 許容誤差 ±10 BPM
        CHECK(estimatedBpm >= 80.0f);
        CHECK(estimatedBpm <= 100.0f);
    }
    
    TEST_CASE("Detect 150 BPM") {
        BeatDetector detector(44100);
        
        auto samples = GenerateBeatPulses(44100, 150.0f, 10.0f);
        detector.ProcessSamples(samples.data(), static_cast<int>(samples.size()));
        
        float estimatedBpm = detector.GetEstimatedBPM();
        
        // 許容誤差 ±10 BPM
        CHECK(estimatedBpm >= 140.0f);
        CHECK(estimatedBpm <= 160.0f);
    }
    
    TEST_CASE("Detect beats with noise") {
        BeatDetector detector(44100);
        
        // ノイズ付きの120 BPMビートパルス
        auto samples = GenerateBeatPulsesWithNoise(44100, 120.0f, 10.0f, 0.2f);
        detector.ProcessSamples(samples.data(), static_cast<int>(samples.size()));
        
        float estimatedBpm = detector.GetEstimatedBPM();
        
        // ノイズがあっても大体のBPMは検出できる（許容誤差広め）
        CHECK(estimatedBpm >= 100.0f);
        CHECK(estimatedBpm <= 140.0f);
    }
    
    TEST_CASE("Beat positions are detected") {
        BeatDetector detector(44100);
        
        auto samples = GenerateBeatPulses(44100, 120.0f, 5.0f);
        detector.ProcessSamples(samples.data(), static_cast<int>(samples.size()));
        
        auto positions = detector.GetBeatPositions();
        
        // 120 BPM で5秒 = 約10ビート
        // 検出漏れを考慮して少なくとも5個以上
        CHECK(positions.size() >= 5);
    }
    
    TEST_CASE("Incremental processing") {
        BeatDetector detector(44100);
        
        // 10秒分のサンプルを1秒ずつ処理
        for (int sec = 0; sec < 10; ++sec) {
            auto samples = GenerateBeatPulses(44100, 120.0f, 1.0f);
            detector.ProcessSamples(samples.data(), static_cast<int>(samples.size()));
        }
        
        float estimatedBpm = detector.GetEstimatedBPM();
        
        // インクリメンタル処理でもBPMが検出される
        CHECK(estimatedBpm >= 100.0f);
        CHECK(estimatedBpm <= 140.0f);
    }
    
    TEST_CASE("Silence returns 0 BPM") {
        BeatDetector detector(44100);
        
        // 無音
        std::vector<float> silence(44100 * 5, 0.0f);
        detector.ProcessSamples(silence.data(), static_cast<int>(silence.size()));
        
        float estimatedBpm = detector.GetEstimatedBPM();
        
        CHECK(estimatedBpm == 0.0f);
    }
    
    TEST_CASE("Constant tone returns 0 or low BPM") {
        BeatDetector detector(44100);
        
        // 一定のサイン波（ビートなし）
        auto samples = GenerateSineWave(44100, 440.0f, 5.0f, 0.5f);
        detector.ProcessSamples(samples.data(), static_cast<int>(samples.size()));
        
        float estimatedBpm = detector.GetEstimatedBPM();
        
        // ビートがない場合は0か非常に低いBPM
        CHECK(estimatedBpm < 50.0f);  // 明確なビートは検出されない
    }
}

// =============================================================================
// BeatDetector エッジケーステスト
// =============================================================================

TEST_SUITE("BeatDetector Edge Cases") {
    
    TEST_CASE("Empty input") {
        BeatDetector detector;
        
        detector.ProcessSamples(nullptr, 0);
        
        CHECK(detector.GetEstimatedBPM() == 0.0f);
        CHECK(detector.GetBeatPositions().empty());
    }
    
    TEST_CASE("Very short input") {
        BeatDetector detector(44100);
        
        // 0.1秒分のサンプル
        auto samples = GenerateBeatPulses(44100, 120.0f, 0.1f);
        detector.ProcessSamples(samples.data(), static_cast<int>(samples.size()));
        
        // 短すぎるとBPMは信頼性が低い
        float estimatedBpm = detector.GetEstimatedBPM();
        CHECK(estimatedBpm >= 0.0f);  // クラッシュしないことを確認
    }
    
    TEST_CASE("Different sample rates") {
        // 48000 Hz
        {
            BeatDetector detector(48000);
            auto samples = GenerateBeatPulses(48000, 120.0f, 10.0f);
            detector.ProcessSamples(samples.data(), static_cast<int>(samples.size()));
            
            float estimatedBpm = detector.GetEstimatedBPM();
            CHECK(estimatedBpm >= 100.0f);
            CHECK(estimatedBpm <= 140.0f);
        }
        
        // 22050 Hz
        {
            BeatDetector detector(22050);
            auto samples = GenerateBeatPulses(22050, 120.0f, 10.0f);
            detector.ProcessSamples(samples.data(), static_cast<int>(samples.size()));
            
            float estimatedBpm = detector.GetEstimatedBPM();
            CHECK(estimatedBpm >= 100.0f);
            CHECK(estimatedBpm <= 140.0f);
        }
    }
}
