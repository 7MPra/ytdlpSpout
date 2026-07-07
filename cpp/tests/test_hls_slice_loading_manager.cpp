// =============================================================================
// test_hls_slice_loading_manager.cpp - HLSスライス読み込みマネージャー テスト
// =============================================================================

#include <gtest/gtest.h>

#include "hls/HlsSliceLoadingManager.h"
#include "hls/M3U8Parser.h"
#include "hls/HlsSegmentCache.h"
#include "hls/HlsCustomAVIOContext.h"

extern "C" {
#include <libavformat/avformat.h>
}

#include <vector>
#include <thread>
#include <chrono>
#include <fstream>

using namespace ytdlpspout::hls;

// =============================================================================
// テストフィクスチャ
// =============================================================================

class HlsSliceLoadingManagerTest : public ::testing::Test {
protected:
    void SetUp() override {
        // テスト用一時ディレクトリを作成
    }
    
    void TearDown() override {
        manager_.Close();
    }
    
    HlsSliceLoadingManager manager_;
};

// =============================================================================
// IsHlsUrl() 判定テスト
// =============================================================================

TEST(HlsSliceLoadingManagerStaticTest, IsHlsUrl_M3U8Extension) {
    EXPECT_TRUE(HlsSliceLoadingManager::IsHlsUrl("https://example.com/playlist.m3u8"));
    EXPECT_TRUE(HlsSliceLoadingManager::IsHlsUrl("https://example.com/stream.M3U8"));
    EXPECT_TRUE(HlsSliceLoadingManager::IsHlsUrl("http://example.com/video.m3u8?token=abc"));
    EXPECT_TRUE(HlsSliceLoadingManager::IsHlsUrl("https://cdn.example.com/path/to/master.m3u8"));
}

TEST(HlsSliceLoadingManagerStaticTest, IsHlsUrl_M3UExtension) {
    EXPECT_TRUE(HlsSliceLoadingManager::IsHlsUrl("https://example.com/playlist.m3u"));
    EXPECT_TRUE(HlsSliceLoadingManager::IsHlsUrl("https://example.com/audio.M3U"));
}

TEST(HlsSliceLoadingManagerStaticTest, IsHlsUrl_NotHls) {
    EXPECT_FALSE(HlsSliceLoadingManager::IsHlsUrl("https://example.com/video.mp4"));
    EXPECT_FALSE(HlsSliceLoadingManager::IsHlsUrl("https://example.com/video.ts"));
    EXPECT_FALSE(HlsSliceLoadingManager::IsHlsUrl("https://www.youtube.com/watch?v=abc123"));
    EXPECT_FALSE(HlsSliceLoadingManager::IsHlsUrl("C:\\videos\\local.mp4"));
    EXPECT_FALSE(HlsSliceLoadingManager::IsHlsUrl(""));
}

TEST(HlsSliceLoadingManagerStaticTest, IsHlsUrl_EdgeCases) {
    // URLにm3u8が含まれるがパスとしては無効なケース
    EXPECT_TRUE(HlsSliceLoadingManager::IsHlsUrl("https://m3u8.example.com/video.m3u8"));
    // クエリパラメータにm3u8が含まれるケース（値が"m3u8"そのものではないので誤検知しない）
    EXPECT_FALSE(HlsSliceLoadingManager::IsHlsUrl("https://example.com/video?format=m3u8like"));
}

// =============================================================================
// IsHlsUrl() クエリパラメータ判定テスト（問題L-5: 判定基準の統一）
// =============================================================================

TEST(HlsSliceLoadingManagerStaticTest, IsHlsUrl_FormatQueryParam) {
    // format=m3u8 が末尾にあるケース
    EXPECT_TRUE(HlsSliceLoadingManager::IsHlsUrl("https://example.com/videoplayback?format=m3u8"));
    // format=m3u8 の後に他のクエリパラメータが続くケース
    EXPECT_TRUE(HlsSliceLoadingManager::IsHlsUrl("https://example.com/videoplayback?format=m3u8&sig=abc"));
    // 大文字小文字を無視
    EXPECT_TRUE(HlsSliceLoadingManager::IsHlsUrl("https://example.com/videoplayback?FORMAT=M3U8"));
}

TEST(HlsSliceLoadingManagerStaticTest, IsHlsUrl_MimeQueryParam) {
    EXPECT_TRUE(HlsSliceLoadingManager::IsHlsUrl(
        "https://example.com/videoplayback?mime=application%2Fvnd.apple.mpegurl"));
}

// =============================================================================
// 初期化テスト（無効入力）
// =============================================================================

TEST_F(HlsSliceLoadingManagerTest, InitializeWithEmptyUrl) {
    HlsSliceConfig config;
    
    EXPECT_FALSE(manager_.Open("", config));
    EXPECT_FALSE(manager_.IsOpen());
}

TEST_F(HlsSliceLoadingManagerTest, InitializeWithInvalidUrl) {
    HlsSliceConfig config;
    
    // 無効なURLでの初期化は失敗
    EXPECT_FALSE(manager_.Open("not-a-valid-url", config));
    EXPECT_FALSE(manager_.IsOpen());
}

TEST_F(HlsSliceLoadingManagerTest, CloseWithoutOpen) {
    // Open せずに Close しても安全
    manager_.Close();
    EXPECT_FALSE(manager_.IsOpen());
    EXPECT_EQ(nullptr, manager_.GetAVIOContext());
}

TEST_F(HlsSliceLoadingManagerTest, CloseIdempotent) {
    // 複数回 Close しても安全
    manager_.Close();
    manager_.Close();
    manager_.Close();
    
    EXPECT_FALSE(manager_.IsOpen());
}

// =============================================================================
// 統計情報テスト（未オープン時）
// =============================================================================

TEST_F(HlsSliceLoadingManagerTest, StatisticsBeforeOpen) {
    EXPECT_EQ(0.0, manager_.GetDuration());
    EXPECT_EQ(0.0, manager_.GetDownloadProgress());
    EXPECT_EQ(0.0, manager_.GetBandwidth());
    EXPECT_FALSE(manager_.IsFullyCached());
    EXPECT_EQ(0u, manager_.GetCachedSegmentCount());
    EXPECT_EQ(0u, manager_.GetTotalSegmentCount());
}

// =============================================================================
// プリフェッチロジックテスト（モック対象）
// =============================================================================

TEST_F(HlsSliceLoadingManagerTest, UpdatePlaybackPositionBeforeOpen) {
    // Open前にUpdatePlaybackPositionを呼び出しても例外が発生しない
    EXPECT_NO_THROW(manager_.UpdatePlaybackPosition(0.0));
    EXPECT_NO_THROW(manager_.UpdatePlaybackPosition(10.0));
}

TEST_F(HlsSliceLoadingManagerTest, NotifySeekBeforeOpen) {
    // Open前にNotifySeekを呼び出しても例外が発生しない
    EXPECT_NO_THROW(manager_.NotifySeek(0.0));
    EXPECT_NO_THROW(manager_.NotifySeek(30.0));
}

// =============================================================================
// 設定テスト
// =============================================================================

TEST(HlsSliceConfigTest, DefaultValues) {
    HlsSliceConfig config;
    
    EXPECT_EQ(256u * 1024 * 1024, config.maxCacheMemory);
    EXPECT_EQ(4, config.maxConcurrentDownloads);
    EXPECT_EQ(5, config.prefetchSegmentsAhead);
    EXPECT_EQ(30000, config.readTimeoutMs);
    EXPECT_TRUE(config.httpHeaders.empty());
}

TEST(HlsSliceConfigTest, CustomValues) {
    HlsSliceConfig config;
    config.maxCacheMemory = 512 * 1024 * 1024;
    config.maxConcurrentDownloads = 8;
    config.prefetchSegmentsAhead = 10;
    config.readTimeoutMs = 60000;
    config.httpHeaders["User-Agent"] = "CustomAgent/1.0";
    config.httpHeaders["Cookie"] = "session=abc123";
    
    EXPECT_EQ(512u * 1024 * 1024, config.maxCacheMemory);
    EXPECT_EQ(8, config.maxConcurrentDownloads);
    EXPECT_EQ(10, config.prefetchSegmentsAhead);
    EXPECT_EQ(60000, config.readTimeoutMs);
    EXPECT_EQ(2u, config.httpHeaders.size());
}

// =============================================================================
// メイン関数
// =============================================================================

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
