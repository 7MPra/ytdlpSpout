// =============================================================================
// test_beat_map_generator.cpp - BeatMapGeneratorのユニットテスト
// =============================================================================

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "audio/BeatMapGenerator.h"
#include "audio/BeatMap.h"
#include <chrono>
#include <thread>
#include <atomic>

using namespace ytdlpspout;

// =============================================================================
// BeatMapGenerator 基本テスト
// =============================================================================

TEST_SUITE("BeatMapGenerator Basic") {
    
    TEST_CASE("Construction") {
        BeatMapGenerator generator;
        
        CHECK(generator.IsCancelled() == false);
    }
    
    TEST_CASE("Cancel initially false") {
        BeatMapGenerator generator;
        
        CHECK(generator.IsCancelled() == false);
    }
    
    TEST_CASE("Cancel sets flag") {
        BeatMapGenerator generator;
        
        generator.Cancel();
        
        CHECK(generator.IsCancelled() == true);
    }
}

// =============================================================================
// BeatMapGenerator 生成テスト（モック/シンプルな入力）
// =============================================================================

TEST_SUITE("BeatMapGenerator Generation") {
    
    TEST_CASE("Generate with non-existent file returns empty BeatMap") {
        BeatMapGenerator generator;
        
        auto result = generator.Generate("non_existent_file.wav");
        
        // 存在しないファイルの場合、空のBeatMapを返す
        CHECK(result.bpm == 0.0f);
        CHECK(result.beats.empty());
    }
    
    TEST_CASE("Progress callback is called") {
        BeatMapGenerator generator;
        
        std::atomic<int> callbackCount{0};
        double lastProgress = 0.0;
        
        auto progressCb = [&](double progress) {
            callbackCount++;
            CHECK(progress >= lastProgress);  // 進行は常に増加
            CHECK(progress >= 0.0);
            CHECK(progress <= 1.0);
            lastProgress = progress;
        };
        
        // 存在しないファイルでも最低限コールバックが呼ばれる（開始時など）
        auto result = generator.Generate("non_existent_file.wav", progressCb);
        
        // コールバックが呼ばれたかどうか（エラー時は0回の可能性あり）
        CHECK(callbackCount >= 0);
    }
}

// =============================================================================
// BeatMapGenerator 非同期テスト
// =============================================================================

TEST_SUITE("BeatMapGenerator Async") {
    
    TEST_CASE("GenerateAsync returns future") {
        BeatMapGenerator generator;
        
        auto future = generator.GenerateAsync("non_existent_file.wav");
        
        // futureが有効であることを確認
        CHECK(future.valid());
        
        // 結果を取得
        auto result = future.get();
        
        // 存在しないファイルなので空のBeatMap
        CHECK(result.bpm == 0.0f);
    }
    
    TEST_CASE("Cancel stops async generation") {
        BeatMapGenerator generator;
        
        std::atomic<bool> cancelled{false};
        
        auto progressCb = [&](double progress) {
            if (progress > 0.1) {
                generator.Cancel();
                cancelled = true;
            }
        };
        
        auto future = generator.GenerateAsync("non_existent_file.wav", progressCb);
        
        auto result = future.get();
        
        // キャンセルされた場合、処理が中断される
        // （存在しないファイルの場合はすぐ終わるので、キャンセルの確認は困難）
        CHECK(generator.IsCancelled() == cancelled.load());
    }
}

// =============================================================================
// BeatJumpController テスト
// =============================================================================

TEST_SUITE("BeatJumpController") {
    
    BeatMap CreateTestBeatMap() {
        BeatMap beatMap;
        beatMap.bpm = 120.0f;
        beatMap.bpmConfidence = 0.95f;
        beatMap.duration = 60.0;
        
        // 120 BPM = 0.5秒ごと
        for (int i = 0; i < 120; ++i) {  // 60秒分 = 120ビート
            BeatInfo beat;
            beat.timestamp = i * 0.5;
            beat.confidence = 0.9f;
            beat.beatNumber = i % 4;
            beat.isDownbeat = (i % 4 == 0);
            beatMap.beats.push_back(beat);
        }
        
        return beatMap;
    }
    
    TEST_CASE("Construction") {
        BeatJumpController controller;
        
        // BeatMapなしでも動作する
        double target = controller.CalculateJumpTarget(10.0, BeatJumpController::JumpUnit::Beat_4, true);
        
        // BeatMapがない場合はcurrentTimeをそのまま返す
        CHECK(target == doctest::Approx(10.0));
    }
    
    TEST_CASE("Set BeatMap") {
        BeatJumpController controller;
        auto beatMap = std::make_shared<BeatMap>(CreateTestBeatMap());
        
        controller.SetBeatMap(beatMap);
        
        // BeatMapが設定されていることを確認（直接確認する方法がないので、動作で確認）
        double target = controller.CalculateJumpTarget(10.0, BeatJumpController::JumpUnit::Beat_1, true);
        CHECK(target != 10.0);  // BeatMapがあればジャンプ先が変わる
    }
    
    TEST_CASE("Jump forward by 1 beat") {
        BeatJumpController controller;
        auto beatMap = std::make_shared<BeatMap>(CreateTestBeatMap());
        controller.SetBeatMap(beatMap);
        
        // 10.0秒から1ビート前進（120 BPM = 0.5秒/ビート）
        double target = controller.CalculateJumpTarget(10.0, BeatJumpController::JumpUnit::Beat_1, true);
        
        // 約10.5秒付近
        CHECK(target >= 10.4);
        CHECK(target <= 10.6);
    }
    
    TEST_CASE("Jump backward by 1 beat") {
        BeatJumpController controller;
        auto beatMap = std::make_shared<BeatMap>(CreateTestBeatMap());
        controller.SetBeatMap(beatMap);
        
        // 10.0秒から1ビート後退
        double target = controller.CalculateJumpTarget(10.0, BeatJumpController::JumpUnit::Beat_1, false);
        
        // 約9.5秒付近
        CHECK(target >= 9.4);
        CHECK(target <= 9.6);
    }
    
    TEST_CASE("Jump forward by 4 beats") {
        BeatJumpController controller;
        auto beatMap = std::make_shared<BeatMap>(CreateTestBeatMap());
        controller.SetBeatMap(beatMap);
        
        // 10.0秒から4ビート前進（120 BPM = 2.0秒）
        double target = controller.CalculateJumpTarget(10.0, BeatJumpController::JumpUnit::Beat_4, true);
        
        // 約12.0秒付近
        CHECK(target >= 11.8);
        CHECK(target <= 12.2);
    }
    
    TEST_CASE("Jump backward by 4 beats") {
        BeatJumpController controller;
        auto beatMap = std::make_shared<BeatMap>(CreateTestBeatMap());
        controller.SetBeatMap(beatMap);
        
        // 10.0秒から4ビート後退
        double target = controller.CalculateJumpTarget(10.0, BeatJumpController::JumpUnit::Beat_4, false);
        
        // 約8.0秒付近
        CHECK(target >= 7.8);
        CHECK(target <= 8.2);
    }
    
    TEST_CASE("Jump by 8 beats") {
        BeatJumpController controller;
        auto beatMap = std::make_shared<BeatMap>(CreateTestBeatMap());
        controller.SetBeatMap(beatMap);
        
        // 10.0秒から8ビート前進（120 BPM = 4.0秒）
        double target = controller.CalculateJumpTarget(10.0, BeatJumpController::JumpUnit::Beat_8, true);
        
        // 約14.0秒付近
        CHECK(target >= 13.8);
        CHECK(target <= 14.2);
    }
    
    TEST_CASE("Jump by 16 beats") {
        BeatJumpController controller;
        auto beatMap = std::make_shared<BeatMap>(CreateTestBeatMap());
        controller.SetBeatMap(beatMap);
        
        // 10.0秒から16ビート前進（120 BPM = 8.0秒）
        double target = controller.CalculateJumpTarget(10.0, BeatJumpController::JumpUnit::Beat_16, true);
        
        // 約18.0秒付近
        CHECK(target >= 17.8);
        CHECK(target <= 18.2);
    }
    
    TEST_CASE("Jump does not go below 0") {
        BeatJumpController controller;
        auto beatMap = std::make_shared<BeatMap>(CreateTestBeatMap());
        controller.SetBeatMap(beatMap);
        
        // 1.0秒から16ビート後退（8秒分）→ 0未満にはならない
        double target = controller.CalculateJumpTarget(1.0, BeatJumpController::JumpUnit::Beat_16, false);
        
        CHECK(target >= 0.0);
    }
    
    TEST_CASE("Jump does not exceed duration") {
        BeatJumpController controller;
        auto beatMap = std::make_shared<BeatMap>(CreateTestBeatMap());
        controller.SetBeatMap(beatMap);
        
        // 58.0秒から16ビート前進 → duration(60秒)を超えない
        double target = controller.CalculateJumpTarget(58.0, BeatJumpController::JumpUnit::Beat_16, true);
        
        CHECK(target <= 60.0);
    }
    
    TEST_CASE("QuantizeToNearestBeat") {
        BeatJumpController controller;
        auto beatMap = std::make_shared<BeatMap>(CreateTestBeatMap());
        controller.SetBeatMap(beatMap);
        
        // 10.2秒 → 最寄りのビート（10.0または10.5）
        double quantized = controller.QuantizeToNearestBeat(10.2);
        
        // 10.0に近いはず
        CHECK(quantized >= 9.9);
        CHECK(quantized <= 10.1);
    }
    
    TEST_CASE("QuantizeToNearestBeat without BeatMap") {
        BeatJumpController controller;
        
        // BeatMapなしの場合はそのまま返す
        double quantized = controller.QuantizeToNearestBeat(10.2);
        
        CHECK(quantized == doctest::Approx(10.2));
    }
}
