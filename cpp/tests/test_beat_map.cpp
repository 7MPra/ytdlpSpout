// =============================================================================
// test_beat_map.cpp - BeatMapのユニットテスト
// =============================================================================

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "audio/BeatMap.h"
#include <filesystem>
#include <cmath>

using namespace ytdlpspout;

// =============================================================================
// BeatInfo テスト
// =============================================================================

TEST_SUITE("BeatInfo") {
    
    TEST_CASE("Default construction") {
        BeatInfo beat;
        
        CHECK(beat.timestamp == 0.0);
        CHECK(beat.confidence == 0.0f);
        CHECK(beat.beatNumber == 0);
        CHECK(beat.isDownbeat == false);
    }
    
    TEST_CASE("Constructed with values") {
        BeatInfo beat{1.5, 0.9f, 2, false};
        
        CHECK(beat.timestamp == 1.5);
        CHECK(beat.confidence == doctest::Approx(0.9f));
        CHECK(beat.beatNumber == 2);
        CHECK(beat.isDownbeat == false);
    }
}

// =============================================================================
// BeatMap 基本テスト
// =============================================================================

TEST_SUITE("BeatMap") {
    
    TEST_CASE("Default construction") {
        BeatMap beatMap;
        
        CHECK(beatMap.bpm == 0.0f);
        CHECK(beatMap.bpmConfidence == 0.0f);
        CHECK(beatMap.beats.empty());
        CHECK(beatMap.duration == 0.0);
    }
    
    TEST_CASE("Add beats") {
        BeatMap beatMap;
        beatMap.bpm = 120.0f;
        beatMap.bpmConfidence = 0.95f;
        beatMap.duration = 10.0;
        
        beatMap.beats.push_back({0.0, 1.0f, 0, true});
        beatMap.beats.push_back({0.5, 0.9f, 1, false});
        beatMap.beats.push_back({1.0, 0.95f, 2, false});
        beatMap.beats.push_back({1.5, 0.85f, 3, false});
        beatMap.beats.push_back({2.0, 1.0f, 0, true});
        
        CHECK(beatMap.beats.size() == 5);
        CHECK(beatMap.bpm == doctest::Approx(120.0f));
    }
}

// =============================================================================
// BeatMap クエリテスト
// =============================================================================

TEST_SUITE("BeatMap Queries") {
    
    BeatMap CreateTestBeatMap() {
        BeatMap beatMap;
        beatMap.bpm = 120.0f;
        beatMap.bpmConfidence = 0.95f;
        beatMap.duration = 10.0;
        
        // 120 BPM = 0.5秒ごと
        for (int i = 0; i < 20; ++i) {
            BeatInfo beat;
            beat.timestamp = i * 0.5;
            beat.confidence = 0.9f;
            beat.beatNumber = i % 4;
            beat.isDownbeat = (i % 4 == 0);
            beatMap.beats.push_back(beat);
        }
        
        return beatMap;
    }
    
    TEST_CASE("GetNearestBeat") {
        auto beatMap = CreateTestBeatMap();
        
        SUBCASE("Exact match") {
            const BeatInfo* beat = beatMap.GetNearestBeat(1.0);
            REQUIRE(beat != nullptr);
            CHECK(beat->timestamp == doctest::Approx(1.0));
        }
        
        SUBCASE("Between beats - closer to earlier") {
            const BeatInfo* beat = beatMap.GetNearestBeat(1.1);
            REQUIRE(beat != nullptr);
            CHECK(beat->timestamp == doctest::Approx(1.0));
        }
        
        SUBCASE("Between beats - closer to later") {
            const BeatInfo* beat = beatMap.GetNearestBeat(1.4);
            REQUIRE(beat != nullptr);
            CHECK(beat->timestamp == doctest::Approx(1.5));
        }
        
        SUBCASE("At start") {
            const BeatInfo* beat = beatMap.GetNearestBeat(0.0);
            REQUIRE(beat != nullptr);
            CHECK(beat->timestamp == doctest::Approx(0.0));
        }
        
        SUBCASE("Before first beat") {
            const BeatInfo* beat = beatMap.GetNearestBeat(-1.0);
            REQUIRE(beat != nullptr);
            CHECK(beat->timestamp == doctest::Approx(0.0));
        }
        
        SUBCASE("After last beat") {
            const BeatInfo* beat = beatMap.GetNearestBeat(100.0);
            REQUIRE(beat != nullptr);
            CHECK(beat->timestamp == doctest::Approx(9.5));  // Last beat
        }
    }
    
    TEST_CASE("GetNextBeat") {
        auto beatMap = CreateTestBeatMap();
        
        SUBCASE("Get next from start") {
            const BeatInfo* beat = beatMap.GetNextBeat(0.0);
            REQUIRE(beat != nullptr);
            CHECK(beat->timestamp == doctest::Approx(0.5));
        }
        
        SUBCASE("Get next from between beats") {
            const BeatInfo* beat = beatMap.GetNextBeat(1.2);
            REQUIRE(beat != nullptr);
            CHECK(beat->timestamp == doctest::Approx(1.5));
        }
        
        SUBCASE("Get next near end - returns nullptr") {
            const BeatInfo* beat = beatMap.GetNextBeat(9.5);
            CHECK(beat == nullptr);
        }
    }
    
    TEST_CASE("GetPreviousBeat") {
        auto beatMap = CreateTestBeatMap();
        
        SUBCASE("Get previous from middle") {
            const BeatInfo* beat = beatMap.GetPreviousBeat(1.2);
            REQUIRE(beat != nullptr);
            CHECK(beat->timestamp == doctest::Approx(1.0));
        }
        
        SUBCASE("Get previous from exact beat") {
            const BeatInfo* beat = beatMap.GetPreviousBeat(1.0);
            REQUIRE(beat != nullptr);
            CHECK(beat->timestamp == doctest::Approx(0.5));
        }
        
        SUBCASE("Get previous from start - returns nullptr") {
            const BeatInfo* beat = beatMap.GetPreviousBeat(0.0);
            CHECK(beat == nullptr);
        }
    }
    
    TEST_CASE("Empty BeatMap queries return nullptr") {
        BeatMap emptyMap;
        
        CHECK(emptyMap.GetNearestBeat(1.0) == nullptr);
        CHECK(emptyMap.GetNextBeat(1.0) == nullptr);
        CHECK(emptyMap.GetPreviousBeat(1.0) == nullptr);
    }
}

// =============================================================================
// BeatMap シリアライズテスト
// =============================================================================

TEST_SUITE("BeatMap Serialization") {
    
    TEST_CASE("Save and load round-trip") {
        BeatMap original;
        original.bpm = 128.0f;
        original.bpmConfidence = 0.92f;
        original.duration = 180.0;
        
        original.beats.push_back({0.0, 1.0f, 0, true});
        original.beats.push_back({0.46875, 0.85f, 1, false});
        original.beats.push_back({0.9375, 0.9f, 2, false});
        original.beats.push_back({1.40625, 0.88f, 3, false});
        original.beats.push_back({1.875, 1.0f, 0, true});
        
        // Save to temp file
        std::string tempPath = "test_beatmap_temp.json";
        REQUIRE(original.SaveToFile(tempPath));
        
        // Load back
        auto loaded = BeatMap::LoadFromFile(tempPath);
        REQUIRE(loaded.has_value());
        
        // Verify
        CHECK(loaded->bpm == doctest::Approx(original.bpm));
        CHECK(loaded->bpmConfidence == doctest::Approx(original.bpmConfidence));
        CHECK(loaded->duration == doctest::Approx(original.duration));
        CHECK(loaded->beats.size() == original.beats.size());
        
        for (size_t i = 0; i < original.beats.size(); ++i) {
            CHECK(loaded->beats[i].timestamp == doctest::Approx(original.beats[i].timestamp));
            CHECK(loaded->beats[i].confidence == doctest::Approx(original.beats[i].confidence));
            CHECK(loaded->beats[i].beatNumber == original.beats[i].beatNumber);
            CHECK(loaded->beats[i].isDownbeat == original.beats[i].isDownbeat);
        }
        
        // Cleanup
        std::filesystem::remove(tempPath);
    }
    
    TEST_CASE("Load non-existent file returns nullopt") {
        auto result = BeatMap::LoadFromFile("non_existent_beatmap.json");
        CHECK(!result.has_value());
    }
    
    TEST_CASE("Save to invalid path returns false") {
        BeatMap beatMap;
        bool result = beatMap.SaveToFile("/invalid/path/that/does/not/exist/beatmap.json");
        CHECK(result == false);
    }
}
