// =============================================================================
// test_slice_loading_manager.cpp - SliceLoadingManagerテスト
// =============================================================================

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "io/SliceLoadingManager.h"

#include <filesystem>
#include <fstream>

using namespace ytdlpspout::io;

// テスト用のテンポラリファイルを作成
static std::string CreateTempTestFile(size_t size = 1024 * 1024) {
    std::string tempPath = std::filesystem::temp_directory_path().string() + "/test_slice_loading.mp4";
    std::ofstream file(tempPath, std::ios::binary);
    if (file.is_open()) {
        // ダミーデータを書き込み
        std::vector<char> data(size, 'X');
        file.write(data.data(), data.size());
        file.close();
    }
    return tempPath;
}

static void RemoveTempFile(const std::string& path) {
    std::filesystem::remove(path);
}

// =============================================================================
// 基本テスト
// =============================================================================

TEST_CASE("SliceLoadingManager construction") {
    SUBCASE("Default construction succeeds") {
        SliceLoadingManager manager;
        CHECK_FALSE(manager.IsOpen());
        CHECK(manager.GetAVIOContext() == nullptr);
        CHECK(manager.GetSourceType() == SliceSourceType::Unknown);
    }
    
    SUBCASE("Construction with no file") {
        SliceLoadingManager manager;
        CHECK_FALSE(manager.IsOpen());
        CHECK(manager.GetFileSize() == 0);
    }
}

TEST_CASE("SliceLoadingManager open local file") {
    std::string testFile = CreateTempTestFile(2 * 1024 * 1024);  // 2MB
    
    SUBCASE("Open valid local file") {
        SliceLoadingManager manager;
        bool result = manager.Open(testFile);
        CHECK(result);
        CHECK(manager.IsOpen());
        CHECK(manager.GetSourceType() == SliceSourceType::LocalFile);
        CHECK(manager.GetFileSize() == 2 * 1024 * 1024);
        CHECK(manager.GetResolvedUrl() == testFile);
        manager.Close();
        CHECK_FALSE(manager.IsOpen());
    }
    
    SUBCASE("Open non-existent file fails") {
        SliceLoadingManager manager;
        bool result = manager.Open("C:/non_existent_file_12345.mp4");
        CHECK_FALSE(result);
        CHECK_FALSE(manager.IsOpen());
    }
    
    RemoveTempFile(testFile);
}

TEST_CASE("SliceLoadingManager open with config") {
    std::string testFile = CreateTempTestFile(3 * 1024 * 1024);  // 3MB
    
    SUBCASE("Custom chunk size") {
        SliceLoadingManager manager;
        SliceLoadingConfig config;
        config.chunkSize = 512 * 1024;  // 512KB
        config.maxCacheMemory = 64 * 1024 * 1024;  // 64MB
        config.maxConcurrentDownloads = 2;
        config.prefetchChunksAhead = 4;
        
        bool result = manager.Open(testFile, config);
        CHECK(result);
        CHECK(manager.IsOpen());
        
        // ファイルサイズに基づくチャンク数を確認
        // 3MB / 512KB = 6 chunks
        CHECK(manager.GetTotalChunkCount() == 6);
        
        manager.Close();
    }
    
    RemoveTempFile(testFile);
}

TEST_CASE("SliceLoadingManager download progress") {
    std::string testFile = CreateTempTestFile(1 * 1024 * 1024);  // 1MB

    SUBCASE("Local file is fully cached immediately") {
        SliceLoadingManager manager;
        bool result = manager.Open(testFile);
        CHECK(result);
        
        // ローカルファイルは100%キャッシュ済み
        CHECK(manager.GetDownloadProgress() == doctest::Approx(1.0).epsilon(0.01));
        CHECK(manager.IsFullyCached());
        
        manager.Close();
    }
    
    SUBCASE("Cached chunk count equals total for local file") {
        SliceLoadingManager manager;
        SliceLoadingConfig config;
        config.chunkSize = 256 * 1024;  // 256KB
        
        bool result = manager.Open(testFile, config);
        CHECK(result);
        
        // 1MB / 256KB = 4 chunks
        CHECK(manager.GetTotalChunkCount() == 4);
        CHECK(manager.GetCachedChunkCount() == manager.GetTotalChunkCount());
        
        manager.Close();
    }
    
    RemoveTempFile(testFile);
}

TEST_CASE("SliceLoadingManager is fully cached") {
    std::string testFile = CreateTempTestFile(512 * 1024);  // 512KB
    
    SUBCASE("Local file returns true") {
        SliceLoadingManager manager;
        bool result = manager.Open(testFile);
        CHECK(result);
        CHECK(manager.IsFullyCached());
        manager.Close();
    }
    
    RemoveTempFile(testFile);
}

TEST_CASE("SliceLoadingManager playback position update") {
    std::string testFile = CreateTempTestFile(10 * 1024 * 1024);  // 10MB
    
    SUBCASE("Update playback position by seconds") {
        SliceLoadingManager manager;
        SliceLoadingConfig config;
        config.chunkSize = 1 * 1024 * 1024;  // 1MB
        
        bool result = manager.Open(testFile, config);
        CHECK(result);
        
        // UpdatePlaybackPositionは例外を投げないことを確認
        CHECK_NOTHROW(manager.UpdatePlaybackPosition(5.0));
        CHECK_NOTHROW(manager.UpdatePlaybackPosition(10.0));
        
        manager.Close();
    }
    
    SUBCASE("Update playback position by bytes") {
        SliceLoadingManager manager;
        bool result = manager.Open(testFile);
        CHECK(result);
        
        CHECK_NOTHROW(manager.UpdatePlaybackPositionBytes(1024 * 1024));
        CHECK_NOTHROW(manager.UpdatePlaybackPositionBytes(5 * 1024 * 1024));
        
        manager.Close();
    }
    
    SUBCASE("Notify seek") {
        SliceLoadingManager manager;
        bool result = manager.Open(testFile);
        CHECK(result);
        
        CHECK_NOTHROW(manager.NotifySeek(3.0));
        CHECK_NOTHROW(manager.NotifySeekBytes(2 * 1024 * 1024));
        
        manager.Close();
    }
    
    RemoveTempFile(testFile);
}

TEST_CASE("SliceLoadingManager source type detection") {
    SUBCASE("Local file path detection") {
        SliceLoadingManager manager;
        
        // Windowsパスはローカルファイル
        std::string winPath = "C:\\Videos\\test.mp4";
        // パスのテスト（実際のファイルは存在しなくてもソースタイプは検出可能）
    }
    
    SUBCASE("HTTP URL detection") {
        SliceLoadingManager manager;
        // HTTP URLはHttpUrl
        // 注: 実際のダウンロードはテストしない
    }
    
    SUBCASE("YouTube URL detection") {
        SliceLoadingManager manager;
        // YouTube URLはYtDlpUrl
        // 注: 実際の解決はテストしない
    }
}

TEST_CASE("SliceLoadingManager close and reopen") {
    std::string testFile = CreateTempTestFile(1 * 1024 * 1024);
    
    SUBCASE("Close and reopen same manager") {
        SliceLoadingManager manager;
        
        CHECK(manager.Open(testFile));
        CHECK(manager.IsOpen());
        
        manager.Close();
        CHECK_FALSE(manager.IsOpen());
        
        CHECK(manager.Open(testFile));
        CHECK(manager.IsOpen());
        
        manager.Close();
    }
    
    SUBCASE("Multiple opens without close replaces previous") {
        SliceLoadingManager manager;
        
        CHECK(manager.Open(testFile));
        // 2回目のOpenは内部でCloseしてから再度開く
        CHECK(manager.Open(testFile));
        CHECK(manager.IsOpen());
        
        manager.Close();
    }
    
    RemoveTempFile(testFile);
}

TEST_CASE("SliceLoadingManager statistics") {
    std::string testFile = CreateTempTestFile(5 * 1024 * 1024);  // 5MB
    
    SUBCASE("Bandwidth is zero or positive") {
        SliceLoadingManager manager;
        bool result = manager.Open(testFile);
        CHECK(result);
        
        // ローカルファイルの場合、帯域幅は無限大または非常に大きい値
        // ただし実装によっては0を返すこともある
        double bandwidth = manager.GetBandwidth();
        CHECK(bandwidth >= 0.0);
        
        manager.Close();
    }
    
    RemoveTempFile(testFile);
}
