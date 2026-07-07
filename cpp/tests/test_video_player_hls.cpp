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
// PLY-1: 統計getterとStop()の並行アクセス（use-after-free回帰テスト）
// =============================================================================
// GUI側の統計ポーリングスレッド（GetDownloadProgress/IsFullyCached/IsHlsMode/
// GetHlsCacheStats）とStop()（sliceManager/hlsManagerのreset()を行う）が同一の
// mutexで保護されていることを確認する。修正前はgetter側がロックを取らずに
// hlsManager/sliceManagerを参照していたため、Stop()と競合するとreset()直後の
// 破棄済みポインタを参照しクラッシュ（UAF）する可能性があった。
// ここではThreadSanitizer等が無い環境でも「デッドロックしない」「クラッシュしない」
// ことを確認するスモークテストとして、統計取得スレッドとStop()スレッドを
// 短時間並行実行する。
TEST_F(VideoPlayerHlsTest, ConcurrentStatsPollingDuringStop_NoCrashOrDeadlock) {
    std::atomic<bool> stopPolling{ false };

    std::thread poller([&]() {
        while (!stopPolling.load(std::memory_order_relaxed)) {
            // Stop()と同一mutexで保護されているため、reset()途中/直後の
            // ポインタを安全に読める（例外・クラッシュなしで既定値を返す）はず。
            volatile double progress = player_.GetDownloadProgress();
            volatile bool fullyCached = player_.IsFullyCached();
            volatile bool hlsMode = player_.IsHlsMode();
            VideoPlayer::HlsCacheStats stats = player_.GetHlsCacheStats();
            (void)progress;
            (void)fullyCached;
            (void)hlsMode;
            (void)stats;
        }
    });

    // Stop()を短時間繰り返し呼び出し、pollerスレッドと競合させる
    for (int i = 0; i < 200; ++i) {
        player_.Stop();
        std::this_thread::sleep_for(std::chrono::microseconds(50));
    }

    stopPolling.store(true, std::memory_order_relaxed);
    poller.join();

    // クラッシュ・デッドロックせずここに到達すれば成功
    EXPECT_EQ(PlayerState::Stopped, player_.GetState());
}

// =============================================================================
// P-7: HLSプレイリスト総時間のVideoInfo反映（回帰テスト）
// =============================================================================
// 注: decoder側でduration/totalFramesが取得できない場合にHLSプレイリストの
// 総時間で補完するロジック自体は、実際のHLSセグメント配信サーバーが必要なため
// 統合テストで検証する。ここではStart前のデフォルト値（0）が期待どおりである
// ことのみを回帰テストとして確認する。
TEST_F(VideoPlayerHlsTest, DurationAndTotalFramesAreZeroBeforeStart) {
    EXPECT_DOUBLE_EQ(0.0, player_.GetDuration());
    EXPECT_EQ(0, player_.GetTotalFrames());
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
