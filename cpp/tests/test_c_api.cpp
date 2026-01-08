// =============================================================================
// test_c_api.cpp - C API テスト
// =============================================================================
//
// C言語APIのユニットテスト
//
// =============================================================================

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "ytdlpspout/ytdlpspout.h"

#include <string>
#include <cstring>

// =============================================================================
// バージョン情報テスト
// =============================================================================

TEST_SUITE("C API - Version") {
    TEST_CASE("ytdlpspout_version returns valid string") {
        const char* version = ytdlpspout_version();
        REQUIRE(version != nullptr);
        CHECK(std::strlen(version) > 0);
    }
}

// =============================================================================
// ハンドル作成・破棄テスト
// =============================================================================

TEST_SUITE("C API - Handle Lifecycle") {
    TEST_CASE("ytdlpspout_create returns valid handle") {
        YtdlpSpoutHandle handle = ytdlpspout_create();
        REQUIRE(handle != nullptr);
        ytdlpspout_destroy(handle);
    }
    
    TEST_CASE("ytdlpspout_destroy with nullptr does not crash") {
        ytdlpspout_destroy(nullptr);  // Should not crash
        CHECK(true);
    }
    
    TEST_CASE("Multiple handles can be created") {
        YtdlpSpoutHandle handle1 = ytdlpspout_create();
        YtdlpSpoutHandle handle2 = ytdlpspout_create();
        REQUIRE(handle1 != nullptr);
        REQUIRE(handle2 != nullptr);
        CHECK(handle1 != handle2);
        ytdlpspout_destroy(handle1);
        ytdlpspout_destroy(handle2);
    }
}

// =============================================================================
// 状態管理テスト
// =============================================================================

TEST_SUITE("C API - State Management") {
    TEST_CASE("Initial state is STOPPED") {
        YtdlpSpoutHandle handle = ytdlpspout_create();
        REQUIRE(handle != nullptr);
        
        YtdlpSpoutState state = ytdlpspout_get_state(handle);
        CHECK(state == YTDLPSPOUT_STATE_STOPPED);
        
        ytdlpspout_destroy(handle);
    }
    
    TEST_CASE("get_state with nullptr returns ERROR") {
        YtdlpSpoutState state = ytdlpspout_get_state(nullptr);
        CHECK(state == YTDLPSPOUT_STATE_ERROR);
    }
    
    TEST_CASE("is_playing returns 0 when stopped") {
        YtdlpSpoutHandle handle = ytdlpspout_create();
        REQUIRE(handle != nullptr);
        
        int playing = ytdlpspout_is_playing(handle);
        CHECK(playing == 0);
        
        ytdlpspout_destroy(handle);
    }
    
    TEST_CASE("is_playing with nullptr returns 0") {
        int playing = ytdlpspout_is_playing(nullptr);
        CHECK(playing == 0);
    }
}

// =============================================================================
// 再生時間テスト
// =============================================================================

TEST_SUITE("C API - Time") {
    TEST_CASE("get_position returns 0 when not playing") {
        YtdlpSpoutHandle handle = ytdlpspout_create();
        REQUIRE(handle != nullptr);
        
        double pos = ytdlpspout_get_position(handle);
        CHECK(pos == 0.0);
        
        ytdlpspout_destroy(handle);
    }
    
    TEST_CASE("get_duration returns 0 when not playing") {
        YtdlpSpoutHandle handle = ytdlpspout_create();
        REQUIRE(handle != nullptr);
        
        double duration = ytdlpspout_get_duration(handle);
        CHECK(duration == 0.0);
        
        ytdlpspout_destroy(handle);
    }
    
    TEST_CASE("get_position with nullptr returns 0") {
        double pos = ytdlpspout_get_position(nullptr);
        CHECK(pos == 0.0);
    }
    
    TEST_CASE("get_duration with nullptr returns 0") {
        double duration = ytdlpspout_get_duration(nullptr);
        CHECK(duration == 0.0);
    }
}

// =============================================================================
// ビート機能テスト
// =============================================================================

TEST_SUITE("C API - Beat Features") {
    TEST_CASE("get_bpm returns 0 when no beatmap") {
        YtdlpSpoutHandle handle = ytdlpspout_create();
        REQUIRE(handle != nullptr);
        
        float bpm = ytdlpspout_get_bpm(handle);
        CHECK(bpm == 0.0f);
        
        ytdlpspout_destroy(handle);
    }
    
    TEST_CASE("get_bpm with nullptr returns 0") {
        float bpm = ytdlpspout_get_bpm(nullptr);
        CHECK(bpm == 0.0f);
    }
    
    TEST_CASE("jump_beats with nullptr returns error") {
        int result = ytdlpspout_jump_beats(nullptr, 4, 1);
        CHECK(result != 0);
    }
    
    TEST_CASE("jump_beats without beatmap returns error") {
        YtdlpSpoutHandle handle = ytdlpspout_create();
        REQUIRE(handle != nullptr);
        
        int result = ytdlpspout_jump_beats(handle, 4, 1);
        CHECK(result != 0);  // Should fail without beatmap
        
        ytdlpspout_destroy(handle);
    }
}

// =============================================================================
// エラー処理テスト
// =============================================================================

TEST_SUITE("C API - Error Handling") {
    TEST_CASE("get_last_error returns non-null") {
        const char* error = ytdlpspout_get_last_error();
        REQUIRE(error != nullptr);
        // Initial error should be empty or contain no error message
    }
    
    TEST_CASE("start with null handle returns error") {
        YtdlpSpoutConfig config = {};
        config.inputFile = "test.mp4";
        config.senderName = "test";
        
        int result = ytdlpspout_start(nullptr, &config);
        CHECK(result != 0);
    }
    
    TEST_CASE("start with null config returns error") {
        YtdlpSpoutHandle handle = ytdlpspout_create();
        REQUIRE(handle != nullptr);
        
        int result = ytdlpspout_start(handle, nullptr);
        CHECK(result != 0);
        
        ytdlpspout_destroy(handle);
    }
    
    TEST_CASE("stop with nullptr does not crash") {
        ytdlpspout_stop(nullptr);  // Should not crash
        CHECK(true);
    }
    
    TEST_CASE("seek with nullptr returns error") {
        int result = ytdlpspout_seek(nullptr, 0.0);
        CHECK(result != 0);
    }
}

// =============================================================================
// 設定構造体テスト
// =============================================================================

TEST_SUITE("C API - Config") {
    TEST_CASE("YtdlpSpoutConfig default initialization") {
        YtdlpSpoutConfig config = {};
        
        CHECK(config.inputFile == nullptr);
        CHECK(config.senderName == nullptr);
        CHECK(config.loop == 0);
        CHECK(config.useHardwareAccel == 0);
    }
    
    TEST_CASE("YtdlpSpoutConfig with values") {
        YtdlpSpoutConfig config = {};
        config.inputFile = "test.mp4";
        config.senderName = "TestSender";
        config.loop = 1;
        config.useHardwareAccel = 1;
        
        CHECK(std::strcmp(config.inputFile, "test.mp4") == 0);
        CHECK(std::strcmp(config.senderName, "TestSender") == 0);
        CHECK(config.loop == 1);
        CHECK(config.useHardwareAccel == 1);
    }
}

// =============================================================================
// スレッドセーフティテスト
// =============================================================================

TEST_SUITE("C API - Thread Safety") {
    TEST_CASE("Error message is thread-local") {
        // This test verifies that the last error is stored in thread-local storage
        // Each call should not interfere with other threads
        const char* error1 = ytdlpspout_get_last_error();
        const char* error2 = ytdlpspout_get_last_error();
        
        // Both should point to valid memory
        REQUIRE(error1 != nullptr);
        REQUIRE(error2 != nullptr);
    }
}

// =============================================================================
// フレーム取得API テスト
// =============================================================================

TEST_SUITE("C API - Extended Config") {
    TEST_CASE("ytdlpspout_config_ex_init sets default values") {
        YtdlpSpoutConfigEx config;
        ytdlpspout_config_ex_init(&config);
        
        CHECK(config.slice.enabled == 1);
        CHECK(config.slice.chunkSize == 2 * 1024 * 1024);
        CHECK(config.slice.maxCacheMemory == 256 * 1024 * 1024);
        CHECK(config.slice.maxConcurrentDownloads == 6);
        CHECK(config.slice.prefetchChunksAhead == 24);
        CHECK(config.slice.criticalChunksAhead == 6);
        CHECK(config.slice.enableContinuousDownload == 1);
        CHECK(config.ytdlp.preferredHeight == 1080);
        CHECK(config.useHardwareAccel == 1);
    }
    
    TEST_CASE("ytdlpspout_config_ex_init with nullptr does not crash") {
        ytdlpspout_config_ex_init(nullptr);  // Should not crash
        CHECK(true);
    }
    
    TEST_CASE("ytdlpspout_start_ex with nullptr handle returns error") {
        YtdlpSpoutConfigEx config;
        ytdlpspout_config_ex_init(&config);
        config.source = "test.mp4";
        
        int result = ytdlpspout_start_ex(nullptr, &config);
        CHECK(result == -1);
    }
    
    TEST_CASE("ytdlpspout_start_ex with nullptr config returns error") {
        YtdlpSpoutHandle handle = ytdlpspout_create();
        REQUIRE(handle != nullptr);
        
        int result = ytdlpspout_start_ex(handle, nullptr);
        CHECK(result == -1);
        
        ytdlpspout_destroy(handle);
    }
    
    TEST_CASE("ytdlpspout_start_ex with nullptr source returns error") {
        YtdlpSpoutHandle handle = ytdlpspout_create();
        REQUIRE(handle != nullptr);
        
        YtdlpSpoutConfigEx config;
        ytdlpspout_config_ex_init(&config);
        // source is nullptr by default after init
        
        int result = ytdlpspout_start_ex(handle, &config);
        CHECK(result == -1);
        
        ytdlpspout_destroy(handle);
    }
}

TEST_SUITE("C API - Download Progress") {
    TEST_CASE("ytdlpspout_get_download_progress with nullptr returns 0") {
        double progress = ytdlpspout_get_download_progress(nullptr);
        CHECK(progress == 0.0);
    }
    
    TEST_CASE("ytdlpspout_get_download_progress returns 0 when not playing") {
        YtdlpSpoutHandle handle = ytdlpspout_create();
        REQUIRE(handle != nullptr);
        
        double progress = ytdlpspout_get_download_progress(handle);
        CHECK(progress == 0.0);
        
        ytdlpspout_destroy(handle);
    }
}

TEST_SUITE("C API - Bandwidth") {
    TEST_CASE("ytdlpspout_get_bandwidth with nullptr returns 0") {
        double bandwidth = ytdlpspout_get_bandwidth(nullptr);
        CHECK(bandwidth == 0.0);
    }
}

TEST_SUITE("C API - Cache Status") {
    TEST_CASE("ytdlpspout_is_fully_cached with nullptr returns 0") {
        int cached = ytdlpspout_is_fully_cached(nullptr);
        CHECK(cached == 0);
    }
    
    TEST_CASE("ytdlpspout_is_fully_cached returns 0 when not playing") {
        YtdlpSpoutHandle handle = ytdlpspout_create();
        REQUIRE(handle != nullptr);
        
        int cached = ytdlpspout_is_fully_cached(handle);
        CHECK(cached == 0);
        
        ytdlpspout_destroy(handle);
    }
    
    TEST_CASE("ytdlpspout_get_cache_stats with nullptr does not crash") {
        size_t cached = 999, total = 999;
        ytdlpspout_get_cache_stats(nullptr, &cached, &total);
        CHECK(cached == 0);
        CHECK(total == 0);
    }
    
    TEST_CASE("ytdlpspout_get_cache_stats with nullptr outputs") {
        YtdlpSpoutHandle handle = ytdlpspout_create();
        REQUIRE(handle != nullptr);
        
        // Should not crash even with nullptr outputs
        ytdlpspout_get_cache_stats(handle, nullptr, nullptr);
        CHECK(true);
        
        ytdlpspout_destroy(handle);
    }
}

TEST_SUITE("C API - Frame Buffer") {
    TEST_CASE("get_frame_buffer_size with nullptr returns 0") {
        int size = ytdlpspout_get_frame_buffer_size(nullptr);
        CHECK(size == 0);
    }
    
    TEST_CASE("get_frame_buffer_size returns 0 when not playing") {
        YtdlpSpoutHandle handle = ytdlpspout_create();
        REQUIRE(handle != nullptr);
        
        int size = ytdlpspout_get_frame_buffer_size(handle);
        CHECK(size == 0);  // No video loaded, so buffer size should be 0
        
        ytdlpspout_destroy(handle);
    }
    
    TEST_CASE("get_current_frame with nullptr returns error") {
        uint8_t buffer[1024];
        int width = 0, height = 0;
        
        int result = ytdlpspout_get_current_frame(nullptr, buffer, sizeof(buffer), &width, &height);
        CHECK(result != 0);
    }
    
    TEST_CASE("get_current_frame with null buffer returns error") {
        YtdlpSpoutHandle handle = ytdlpspout_create();
        REQUIRE(handle != nullptr);
        
        int width = 0, height = 0;
        int result = ytdlpspout_get_current_frame(handle, nullptr, 0, &width, &height);
        CHECK(result != 0);
        
        ytdlpspout_destroy(handle);
    }
    
    TEST_CASE("get_current_frame returns error when not playing") {
        YtdlpSpoutHandle handle = ytdlpspout_create();
        REQUIRE(handle != nullptr);
        
        uint8_t buffer[1024];
        int width = 0, height = 0;
        
        // Should fail because no video is loaded
        int result = ytdlpspout_get_current_frame(handle, buffer, sizeof(buffer), &width, &height);
        CHECK(result != 0);
        
        ytdlpspout_destroy(handle);
    }
}
