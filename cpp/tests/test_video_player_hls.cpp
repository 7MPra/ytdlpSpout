// =============================================================================
// test_video_player_hls.cpp - VideoPlayer HLSスライス読み込み統合テスト
// =============================================================================
//
// VideoPlayerにHlsSliceLoadingManagerを統合した機能のテスト
//
// =============================================================================

#include <gtest/gtest.h>

#include "player/VideoPlayer.h"
#include "hls/HlsSliceLoadingManager.h"
#include "utils/Logger.h"

#include <thread>
#include <chrono>

using namespace ytdlpspout;
using namespace ytdlpspout::hls;

// =============================================================================
// テストフィクスチャ
// =============================================================================

class VideoPlayerHlsTest : public ::testing::Test {
protected:
    void SetUp() override {
        // ログレベルを設定
        Logger::SetLevel(LogLevel::Debug);
    }
    
    void TearDown() override {
        player_.Stop();
    }
    
    VideoPlayer player_;
};

// =============================================================================
// HLS判定テスト
// =============================================================================

TEST(HlsUrlDetectionTest, IsHlsUrl_M3U8Extension) {
    EXPECT_TRUE(HlsSliceLoadingManager::IsHlsUrl("https://example.com/playlist.m3u8"));
    EXPECT_TRUE(HlsSliceLoadingManager::IsHlsUrl("https://example.com/stream.M3U8"));
    EXPECT_TRUE(HlsSliceLoadingManager::IsHlsUrl("http://example.com/video.m3u8?token=abc"));
}

TEST(HlsUrlDetectionTest, IsHlsUrl_NotHls) {
    EXPECT_FALSE(HlsSliceLoadingManager::IsHlsUrl("https://example.com/video.mp4"));
    EXPECT_FALSE(HlsSliceLoadingManager::IsHlsUrl("https://example.com/video.ts"));
    EXPECT_FALSE(HlsSliceLoadingManager::IsHlsUrl(""));
}

// =============================================================================
// PlayerConfig スライス設定テスト
// =============================================================================

TEST(PlayerConfigTest, SliceConfigDefaults) {
    PlayerConfig config;
    
    // スライス読み込みはデフォルトで有効
    EXPECT_TRUE(config.slice.enabled);
    EXPECT_GT(config.slice.maxCacheMemory, 0u);
    EXPECT_GT(config.slice.maxConcurrentDownloads, 0);
}

TEST(PlayerConfigTest, HttpHeadersConfig) {
    PlayerConfig config;
    config.httpHeaders["User-Agent"] = "TestAgent/1.0";
    config.httpHeaders["Cookie"] = "session=test123";
    
    EXPECT_EQ(2u, config.httpHeaders.size());
    EXPECT_EQ("TestAgent/1.0", config.httpHeaders["User-Agent"]);
    EXPECT_EQ("session=test123", config.httpHeaders["Cookie"]);
}

// =============================================================================
// VideoPlayer HLS統合テスト（無効入力）
// =============================================================================

TEST_F(VideoPlayerHlsTest, StartWithEmptySource) {
    PlayerConfig config;
    config.source = "";
    
    // 空のソースでは開始できない
    EXPECT_FALSE(player_.Start(config));
}

TEST_F(VideoPlayerHlsTest, StartWithInvalidHlsUrl) {
    PlayerConfig config;
    config.source = "https://invalid-server.example.com/nonexistent.m3u8";
    config.slice.enabled = true;
    
    // 無効なHLS URLでは失敗
    EXPECT_FALSE(player_.Start(config));
}

// =============================================================================
// VideoPlayer状態テスト
// =============================================================================

TEST_F(VideoPlayerHlsTest, InitialState) {
    EXPECT_EQ(PlayerState::Stopped, player_.GetState());
    EXPECT_FALSE(player_.IsPlaying());
    EXPECT_FALSE(player_.IsPaused());
}

TEST_F(VideoPlayerHlsTest, StopWithoutStart) {
    // 開始していない状態でStopしても安全
    EXPECT_NO_THROW(player_.Stop());
    EXPECT_EQ(PlayerState::Stopped, player_.GetState());
}

// =============================================================================
// ダウンロード進捗テスト（非HLS時）
// =============================================================================

TEST_F(VideoPlayerHlsTest, DownloadProgressBeforeStart) {
    // 開始前はダウンロード進捗は0.0
    EXPECT_EQ(0.0, player_.GetDownloadProgress());
    EXPECT_FALSE(player_.IsFullyCached());
}

// =============================================================================
// HLSモードフラグテスト
// =============================================================================

// 注: 実際のHLS URLを使用したテストは統合テストで行う
// ここではユニットテストとして設定の受け渡しをテスト

TEST(HlsSliceConfigTest, ConfigCopyToHls) {
    PlayerConfig playerConfig;
    playerConfig.slice.enabled = true;
    playerConfig.slice.maxCacheMemory = 512 * 1024 * 1024;
    playerConfig.slice.maxConcurrentDownloads = 8;
    playerConfig.httpHeaders["User-Agent"] = "TestAgent";
    playerConfig.httpHeaders["Referer"] = "https://example.com/";
    
    // HLS設定に変換
    HlsSliceConfig hlsConfig;
    hlsConfig.maxCacheMemory = playerConfig.slice.maxCacheMemory;
    hlsConfig.maxConcurrentDownloads = playerConfig.slice.maxConcurrentDownloads;
    hlsConfig.prefetchSegmentsAhead = 5;  // デフォルト
    hlsConfig.readTimeoutMs = 30000;
    hlsConfig.httpHeaders = playerConfig.httpHeaders;
    
    EXPECT_EQ(512u * 1024 * 1024, hlsConfig.maxCacheMemory);
    EXPECT_EQ(8, hlsConfig.maxConcurrentDownloads);
    EXPECT_EQ(2u, hlsConfig.httpHeaders.size());
    EXPECT_EQ("TestAgent", hlsConfig.httpHeaders["User-Agent"]);
    EXPECT_EQ("https://example.com/", hlsConfig.httpHeaders["Referer"]);
}

// =============================================================================
// メイン関数
// =============================================================================

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
