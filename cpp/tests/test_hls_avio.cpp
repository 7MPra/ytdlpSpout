// =============================================================================
// test_hls_avio.cpp - HLS カスタムAVIOContext テスト
// =============================================================================

#include <gtest/gtest.h>

#include "hls/HlsCustomAVIOContext.h"
#include "hls/HlsSegmentCache.h"
#include "hls/M3U8Parser.h"

extern "C" {
#include <libavformat/avformat.h>
}

#include <vector>
#include <thread>
#include <chrono>

using namespace ytdlpspout::hls;

// =============================================================================
// テスト用ヘルパー
// =============================================================================

namespace {

/// @brief テスト用プレイリストを作成
M3U8Playlist CreateTestPlaylist(int segmentCount, double segmentDuration = 6.0) {
    M3U8Playlist playlist;
    playlist.version = 3;
    playlist.targetDuration = segmentDuration;
    playlist.mediaSequence = 0;
    playlist.isEndList = true;
    playlist.isLive = false;
    playlist.totalDuration = segmentCount * segmentDuration;
    
    for (int i = 0; i < segmentCount; ++i) {
        HlsSegment segment;
        segment.index = i;
        segment.url = "https://example.com/segment" + std::to_string(i) + ".ts";
        segment.duration = segmentDuration;
        segment.mediaSequence = i;
        playlist.segments.push_back(segment);
    }
    
    return playlist;
}

/// @brief テスト用セグメントデータを作成
std::vector<uint8_t> CreateTestSegmentData(int64_t segmentIndex, size_t size) {
    std::vector<uint8_t> data(size);
    // パターン: セグメントインデックスに基づく繰り返しデータ
    for (size_t i = 0; i < size; ++i) {
        data[i] = static_cast<uint8_t>((segmentIndex * 100 + i) % 256);
    }
    return data;
}

/// @brief キャッシュにテストデータを追加
void PopulateCacheWithSegments(HlsSegmentCache& cache, int segmentCount, size_t segmentSize) {
    for (int i = 0; i < segmentCount; ++i) {
        auto data = CreateTestSegmentData(i, segmentSize);
        cache.WriteSegment(i, std::move(data), false);
    }
}

}  // namespace

// =============================================================================
// テストフィクスチャ
// =============================================================================

class HlsCustomAVIOContextTest : public ::testing::Test {
protected:
    void SetUp() override {
        // キャッシュ設定
        HlsSegmentCacheConfig cacheConfig;
        cacheConfig.maxMemoryBytes = 64 * 1024 * 1024;  // 64MB
        cacheConfig.maxSegments = 50;
        
        cache_ = std::make_unique<HlsSegmentCache>(cacheConfig);
    }
    
    void TearDown() override {
        avioContext_.Close();
        cache_.reset();
    }
    
    std::unique_ptr<HlsSegmentCache> cache_;
    HlsCustomAVIOContext avioContext_;
};

// =============================================================================
// 初期化とクローズ
// =============================================================================

TEST_F(HlsCustomAVIOContextTest, InitializeWithValidInput) {
    // 3セグメントのプレイリスト
    auto playlist = CreateTestPlaylist(3, 6.0);
    cache_->Initialize(playlist);
    
    // 初期化
    ASSERT_TRUE(avioContext_.Initialize(cache_.get(), playlist));
    
    // AVIOContextが取得できる
    EXPECT_NE(nullptr, avioContext_.GetAVIOContext());
}

TEST_F(HlsCustomAVIOContextTest, InitializeWithNullCache) {
    auto playlist = CreateTestPlaylist(3, 6.0);
    
    // nullキャッシュでの初期化は失敗
    EXPECT_FALSE(avioContext_.Initialize(nullptr, playlist));
}

TEST_F(HlsCustomAVIOContextTest, InitializeWithEmptyPlaylist) {
    auto playlist = CreateTestPlaylist(0);
    cache_->Initialize(playlist);
    
    // 空のプレイリストでの初期化は失敗
    EXPECT_FALSE(avioContext_.Initialize(cache_.get(), playlist));
}

TEST_F(HlsCustomAVIOContextTest, CloseIdempotent) {
    auto playlist = CreateTestPlaylist(3, 6.0);
    cache_->Initialize(playlist);
    avioContext_.Initialize(cache_.get(), playlist);
    
    // 複数回Closeしても安全
    avioContext_.Close();
    avioContext_.Close();
    
    EXPECT_EQ(nullptr, avioContext_.GetAVIOContext());
}

TEST_F(HlsCustomAVIOContextTest, CloseWithoutInitialize) {
    // 初期化なしでCloseしても安全
    avioContext_.Close();
    EXPECT_EQ(nullptr, avioContext_.GetAVIOContext());
}

// =============================================================================
// 連続読み取り（複数セグメントをまたぐ）
// =============================================================================

TEST_F(HlsCustomAVIOContextTest, ReadSingleSegment) {
    const int segmentCount = 3;
    const size_t segmentSize = 1024;
    
    auto playlist = CreateTestPlaylist(segmentCount, 6.0);
    cache_->Initialize(playlist);
    PopulateCacheWithSegments(*cache_, segmentCount, segmentSize);
    
    avioContext_.Initialize(cache_.get(), playlist);
    avioContext_.SetReadTimeout(1000);
    
    // 最初のセグメントを読み取り
    std::vector<uint8_t> buffer(segmentSize);
    AVIOContext* avio = avioContext_.GetAVIOContext();
    ASSERT_NE(nullptr, avio);
    
    int bytesRead = avio_read(avio, buffer.data(), static_cast<int>(buffer.size()));
    ASSERT_EQ(static_cast<int>(segmentSize), bytesRead);
    
    // データを検証
    auto expected = CreateTestSegmentData(0, segmentSize);
    EXPECT_EQ(expected, buffer);
}

TEST_F(HlsCustomAVIOContextTest, ReadAcrossSegments) {
    const int segmentCount = 3;
    const size_t segmentSize = 1024;
    
    auto playlist = CreateTestPlaylist(segmentCount, 6.0);
    cache_->Initialize(playlist);
    PopulateCacheWithSegments(*cache_, segmentCount, segmentSize);
    
    avioContext_.Initialize(cache_.get(), playlist);
    avioContext_.SetReadTimeout(1000);
    
    // 全セグメントをまたいで読み取り
    const size_t totalSize = segmentCount * segmentSize;
    std::vector<uint8_t> buffer(totalSize);
    
    AVIOContext* avio = avioContext_.GetAVIOContext();
    int bytesRead = avio_read(avio, buffer.data(), static_cast<int>(buffer.size()));
    ASSERT_EQ(static_cast<int>(totalSize), bytesRead);
    
    // 各セグメントのデータを検証
    for (int i = 0; i < segmentCount; ++i) {
        auto expected = CreateTestSegmentData(i, segmentSize);
        std::vector<uint8_t> actual(buffer.begin() + i * segmentSize, 
                                     buffer.begin() + (i + 1) * segmentSize);
        EXPECT_EQ(expected, actual) << "Segment " << i << " data mismatch";
    }
}

TEST_F(HlsCustomAVIOContextTest, ReadPartialSegment) {
    const int segmentCount = 2;
    const size_t segmentSize = 1024;
    
    auto playlist = CreateTestPlaylist(segmentCount, 6.0);
    cache_->Initialize(playlist);
    PopulateCacheWithSegments(*cache_, segmentCount, segmentSize);
    
    avioContext_.Initialize(cache_.get(), playlist);
    avioContext_.SetReadTimeout(1000);
    
    // セグメントの一部を読み取り
    const size_t partialSize = 512;
    std::vector<uint8_t> buffer(partialSize);
    
    AVIOContext* avio = avioContext_.GetAVIOContext();
    int bytesRead = avio_read(avio, buffer.data(), static_cast<int>(buffer.size()));
    ASSERT_EQ(static_cast<int>(partialSize), bytesRead);
    
    // 読み取り後は全セグメントを読んでしまう（FFmpegのAVIOはバッファリングする）
    // 代わりに読み取ったバイト数とデータを検証
    auto expected = CreateTestSegmentData(0, segmentSize);
    std::vector<uint8_t> expectedPartial(expected.begin(), expected.begin() + partialSize);
    EXPECT_EQ(expectedPartial, buffer);
}

TEST_F(HlsCustomAVIOContextTest, ReadCrossingSegmentBoundary) {
    const int segmentCount = 2;
    const size_t segmentSize = 1024;
    
    auto playlist = CreateTestPlaylist(segmentCount, 6.0);
    cache_->Initialize(playlist);
    PopulateCacheWithSegments(*cache_, segmentCount, segmentSize);
    
    avioContext_.Initialize(cache_.get(), playlist);
    avioContext_.SetReadTimeout(1000);
    
    AVIOContext* avio = avioContext_.GetAVIOContext();
    
    // セグメント境界をまたぐ読み取り
    // 最初に800バイト読み取り
    std::vector<uint8_t> buffer1(800);
    int bytesRead1 = avio_read(avio, buffer1.data(), 800);
    ASSERT_EQ(800, bytesRead1);
    
    // 次に500バイト読み取り
    std::vector<uint8_t> buffer2(500);
    int bytesRead2 = avio_read(avio, buffer2.data(), 500);
    ASSERT_EQ(500, bytesRead2);
    
    // データの検証（セグメント境界をまたいだデータが正しく読める）
    auto expected0 = CreateTestSegmentData(0, segmentSize);
    auto expected1 = CreateTestSegmentData(1, segmentSize);
    
    // buffer1は最初のセグメントの先頭800バイト
    std::vector<uint8_t> expectedBuffer1(expected0.begin(), expected0.begin() + 800);
    EXPECT_EQ(expectedBuffer1, buffer1);
    
    // buffer2はセグメント0の残り224バイト + セグメント1の276バイト
    std::vector<uint8_t> expectedBuffer2;
    expectedBuffer2.insert(expectedBuffer2.end(), expected0.begin() + 800, expected0.end());
    expectedBuffer2.insert(expectedBuffer2.end(), expected1.begin(), expected1.begin() + 276);
    EXPECT_EQ(expectedBuffer2, buffer2);
}

// =============================================================================
// シーク（SEEK_SET, SEEK_CUR, SEEK_END）
// =============================================================================

TEST_F(HlsCustomAVIOContextTest, SeekSet) {
    const int segmentCount = 3;
    const size_t segmentSize = 1024;
    
    auto playlist = CreateTestPlaylist(segmentCount, 6.0);
    cache_->Initialize(playlist);
    PopulateCacheWithSegments(*cache_, segmentCount, segmentSize);
    
    avioContext_.Initialize(cache_.get(), playlist);
    avioContext_.SetReadTimeout(1000);
    
    // まず最初のセグメントを読み込んでサイズを更新
    std::vector<uint8_t> buffer(segmentSize);
    AVIOContext* avio = avioContext_.GetAVIOContext();
    avio_read(avio, buffer.data(), static_cast<int>(segmentSize));
    
    // SEEK_SETでセグメント1の先頭へシーク
    int64_t newPos = avioContext_.Seek(segmentSize, SEEK_SET);
    EXPECT_EQ(static_cast<int64_t>(segmentSize), newPos);
    EXPECT_EQ(static_cast<int64_t>(segmentSize), avioContext_.GetPosition());
    EXPECT_EQ(1, avioContext_.GetCurrentSegmentIndex());
}

TEST_F(HlsCustomAVIOContextTest, SeekCur) {
    const int segmentCount = 3;
    const size_t segmentSize = 1024;
    
    auto playlist = CreateTestPlaylist(segmentCount, 6.0);
    cache_->Initialize(playlist);
    PopulateCacheWithSegments(*cache_, segmentCount, segmentSize);
    
    avioContext_.Initialize(cache_.get(), playlist);
    avioContext_.SetReadTimeout(1000);
    
    // SEEK_SETで512バイト位置へ
    avioContext_.Seek(512, SEEK_SET);
    EXPECT_EQ(512, avioContext_.GetPosition());
    
    // SEEK_CURで256バイト進む
    int64_t newPos = avioContext_.Seek(256, SEEK_CUR);
    EXPECT_EQ(768, newPos);
    EXPECT_EQ(768, avioContext_.GetPosition());
}

TEST_F(HlsCustomAVIOContextTest, SeekEnd) {
    const int segmentCount = 3;
    const size_t segmentSize = 1024;
    
    auto playlist = CreateTestPlaylist(segmentCount, 6.0);
    cache_->Initialize(playlist);
    PopulateCacheWithSegments(*cache_, segmentCount, segmentSize);
    
    avioContext_.Initialize(cache_.get(), playlist);
    avioContext_.SetReadTimeout(1000);
    
    // まず全セグメントを読み込んでサイズを更新
    const size_t totalSize = segmentCount * segmentSize;
    std::vector<uint8_t> buffer(totalSize);
    AVIOContext* avio = avioContext_.GetAVIOContext();
    avio_read(avio, buffer.data(), static_cast<int>(totalSize));
    
    // サイズが更新されていることを確認
    EXPECT_EQ(static_cast<int64_t>(totalSize), avioContext_.GetSize());
    
    // SEEK_ENDで末尾から512バイト前へシーク
    int64_t newPos = avioContext_.Seek(-512, SEEK_END);
    EXPECT_EQ(static_cast<int64_t>(totalSize - 512), newPos);
}

TEST_F(HlsCustomAVIOContextTest, SeekNegativePosition) {
    const int segmentCount = 3;
    const size_t segmentSize = 1024;
    
    auto playlist = CreateTestPlaylist(segmentCount, 6.0);
    cache_->Initialize(playlist);
    PopulateCacheWithSegments(*cache_, segmentCount, segmentSize);
    
    avioContext_.Initialize(cache_.get(), playlist);
    
    // 負の位置へのシークは失敗
    int64_t newPos = avioContext_.Seek(-100, SEEK_SET);
    EXPECT_EQ(-1, newPos);
}

TEST_F(HlsCustomAVIOContextTest, SeekBeyondEnd) {
    const int segmentCount = 3;
    const size_t segmentSize = 1024;
    const size_t totalSize = segmentCount * segmentSize;
    
    auto playlist = CreateTestPlaylist(segmentCount, 6.0);
    cache_->Initialize(playlist);
    PopulateCacheWithSegments(*cache_, segmentCount, segmentSize);
    
    avioContext_.Initialize(cache_.get(), playlist);
    
    // 末尾を超えてシーク（許容するが、次の読み取りでEOFになる）
    int64_t newPos = avioContext_.Seek(totalSize + 100, SEEK_SET);
    EXPECT_EQ(static_cast<int64_t>(totalSize + 100), newPos);
}

// =============================================================================
// 時間ベースシーク
// =============================================================================

TEST_F(HlsCustomAVIOContextTest, SeekToTimeZero) {
    const int segmentCount = 3;
    const size_t segmentSize = 1024;
    const double segmentDuration = 6.0;
    
    auto playlist = CreateTestPlaylist(segmentCount, segmentDuration);
    cache_->Initialize(playlist);
    PopulateCacheWithSegments(*cache_, segmentCount, segmentSize);
    
    avioContext_.Initialize(cache_.get(), playlist);
    
    // 最初に読み取り位置を進める
    avioContext_.Seek(2000, SEEK_SET);
    
    // 0秒へシーク
    int64_t newPos = avioContext_.SeekToTime(0.0);
    EXPECT_EQ(0, newPos);
    EXPECT_EQ(0, avioContext_.GetCurrentSegmentIndex());
}

TEST_F(HlsCustomAVIOContextTest, SeekToTimeMiddle) {
    const int segmentCount = 3;
    const size_t segmentSize = 1024;
    const double segmentDuration = 6.0;
    
    auto playlist = CreateTestPlaylist(segmentCount, segmentDuration);
    cache_->Initialize(playlist);
    PopulateCacheWithSegments(*cache_, segmentCount, segmentSize);
    
    avioContext_.Initialize(cache_.get(), playlist);
    avioContext_.SetReadTimeout(1000);
    
    // まず最初のセグメントを読み込んでサイズを更新
    std::vector<uint8_t> buffer(segmentSize);
    AVIOContext* avio = avioContext_.GetAVIOContext();
    avio_read(avio, buffer.data(), static_cast<int>(segmentSize));
    
    // 7秒へシーク（セグメント1の1秒目）
    int64_t newPos = avioContext_.SeekToTime(7.0);
    EXPECT_EQ(1, avioContext_.GetCurrentSegmentIndex());
    
    // セグメント1の先頭へシークされることを確認
    // 最初のセグメントのサイズが更新されたので、それに基づいたオフセット
    EXPECT_EQ(static_cast<int64_t>(segmentSize), newPos);
}

TEST_F(HlsCustomAVIOContextTest, SeekToTimeEnd) {
    const int segmentCount = 3;
    const size_t segmentSize = 1024;
    const double segmentDuration = 6.0;
    const double totalDuration = segmentCount * segmentDuration;
    
    auto playlist = CreateTestPlaylist(segmentCount, segmentDuration);
    cache_->Initialize(playlist);
    PopulateCacheWithSegments(*cache_, segmentCount, segmentSize);
    
    avioContext_.Initialize(cache_.get(), playlist);
    
    // 末尾を超える時間へシーク（最後のセグメントの先頭にクランプ）
    int64_t newPos = avioContext_.SeekToTime(totalDuration + 10.0);
    EXPECT_EQ(segmentCount - 1, avioContext_.GetCurrentSegmentIndex());
}

TEST_F(HlsCustomAVIOContextTest, SeekToTimeNegative) {
    const int segmentCount = 3;
    const size_t segmentSize = 1024;
    
    auto playlist = CreateTestPlaylist(segmentCount, 6.0);
    cache_->Initialize(playlist);
    PopulateCacheWithSegments(*cache_, segmentCount, segmentSize);
    
    avioContext_.Initialize(cache_.get(), playlist);
    
    // 負の時間へシーク（0にクランプ）
    int64_t newPos = avioContext_.SeekToTime(-5.0);
    EXPECT_EQ(0, newPos);
    EXPECT_EQ(0, avioContext_.GetCurrentSegmentIndex());
}

// =============================================================================
// EOF処理
// =============================================================================

TEST_F(HlsCustomAVIOContextTest, ReadAtEOF) {
    const int segmentCount = 2;
    const size_t segmentSize = 1024;
    const size_t totalSize = segmentCount * segmentSize;
    
    auto playlist = CreateTestPlaylist(segmentCount, 6.0);
    cache_->Initialize(playlist);
    PopulateCacheWithSegments(*cache_, segmentCount, segmentSize);
    
    avioContext_.Initialize(cache_.get(), playlist);
    avioContext_.SetReadTimeout(1000);
    
    // 全データを読み取り
    std::vector<uint8_t> buffer(totalSize);
    AVIOContext* avio = avioContext_.GetAVIOContext();
    avio_read(avio, buffer.data(), static_cast<int>(totalSize));
    
    // EOFでの追加読み取りはAVERROR_EOF
    std::vector<uint8_t> eofBuffer(100);
    int bytesRead = avio_read(avio, eofBuffer.data(), 100);
    EXPECT_EQ(AVERROR_EOF, bytesRead);
}

TEST_F(HlsCustomAVIOContextTest, ReadPartialAtEnd) {
    const int segmentCount = 2;
    const size_t segmentSize = 1024;
    
    auto playlist = CreateTestPlaylist(segmentCount, 6.0);
    cache_->Initialize(playlist);
    PopulateCacheWithSegments(*cache_, segmentCount, segmentSize);
    
    avioContext_.Initialize(cache_.get(), playlist);
    avioContext_.SetReadTimeout(1000);
    
    // まず全データを読み込んでサイズを更新
    const size_t totalSize = segmentCount * segmentSize;
    std::vector<uint8_t> fullBuffer(totalSize);
    AVIOContext* avio = avioContext_.GetAVIOContext();
    avio_read(avio, fullBuffer.data(), static_cast<int>(totalSize));
    
    // 末尾近くへシーク
    avioContext_.Seek(totalSize - 100, SEEK_SET);
    
    // 残りより多く読み取ろうとしても、実際のバイト数のみ返る
    std::vector<uint8_t> buffer(500);
    int bytesRead = avio_read(avio, buffer.data(), 500);
    EXPECT_EQ(100, bytesRead);  // 残り100バイトのみ
}

// =============================================================================
// AVSEEK_SIZE応答
// =============================================================================

TEST_F(HlsCustomAVIOContextTest, AVSeekSize) {
    const int segmentCount = 3;
    const size_t segmentSize = 1024;
    
    auto playlist = CreateTestPlaylist(segmentCount, 6.0);
    cache_->Initialize(playlist);
    PopulateCacheWithSegments(*cache_, segmentCount, segmentSize);
    
    avioContext_.Initialize(cache_.get(), playlist);
    avioContext_.SetReadTimeout(1000);
    
    // まず全データを読み込んでサイズを更新
    const size_t totalSize = segmentCount * segmentSize;
    std::vector<uint8_t> buffer(totalSize);
    AVIOContext* avio = avioContext_.GetAVIOContext();
    avio_read(avio, buffer.data(), static_cast<int>(totalSize));
    
    // AVSEEK_SIZEで総サイズを取得
    int64_t size = avioContext_.Seek(0, AVSEEK_SIZE);
    EXPECT_EQ(static_cast<int64_t>(totalSize), size);
}

TEST_F(HlsCustomAVIOContextTest, GetSizeEstimated) {
    const int segmentCount = 3;
    const double segmentDuration = 6.0;
    
    auto playlist = CreateTestPlaylist(segmentCount, segmentDuration);
    cache_->Initialize(playlist);
    // セグメントデータは追加しない（推定サイズを使用）
    
    avioContext_.Initialize(cache_.get(), playlist);
    
    // 推定サイズ（1秒あたり約1MB）を取得
    int64_t size = avioContext_.GetSize();
    
    // 推定サイズは totalDuration * 1MB/s
    const double bytesPerSecond = 1.0 * 1024 * 1024;  // 1 MB/s
    int64_t expectedEstimate = static_cast<int64_t>(playlist.totalDuration * bytesPerSecond);
    
    EXPECT_EQ(expectedEstimate, size);
}

TEST_F(HlsCustomAVIOContextTest, GetSizeUpdatesAfterRead) {
    const int segmentCount = 3;
    const size_t segmentSize = 2048;  // 推定より大きいサイズ
    
    auto playlist = CreateTestPlaylist(segmentCount, 6.0);
    cache_->Initialize(playlist);
    PopulateCacheWithSegments(*cache_, segmentCount, segmentSize);
    
    avioContext_.Initialize(cache_.get(), playlist);
    avioContext_.SetReadTimeout(1000);
    
    // 最初のセグメントを読み取り（サイズ更新をトリガー）
    std::vector<uint8_t> buffer(segmentSize);
    AVIOContext* avio = avioContext_.GetAVIOContext();
    avio_read(avio, buffer.data(), static_cast<int>(segmentSize));
    
    // サイズが実データに基づいて更新されている
    // 少なくとも読み取ったセグメントのサイズが反映されている
    int64_t size = avioContext_.GetSize();
    EXPECT_GE(size, static_cast<int64_t>(segmentSize));
}

// =============================================================================
// AVIOContext経由の操作
// =============================================================================

TEST_F(HlsCustomAVIOContextTest, AVIOSeekViaContext) {
    const int segmentCount = 3;
    const size_t segmentSize = 1024;
    
    auto playlist = CreateTestPlaylist(segmentCount, 6.0);
    cache_->Initialize(playlist);
    PopulateCacheWithSegments(*cache_, segmentCount, segmentSize);
    
    avioContext_.Initialize(cache_.get(), playlist);
    avioContext_.SetReadTimeout(1000);
    
    // まず全データを読み込んでサイズを更新
    const size_t totalSize = segmentCount * segmentSize;
    std::vector<uint8_t> buffer(totalSize);
    AVIOContext* avio = avioContext_.GetAVIOContext();
    ASSERT_NE(nullptr, avio);
    avio_read(avio, buffer.data(), static_cast<int>(totalSize));
    
    // avio_seek経由でシーク
    int64_t newPos = avio_seek(avio, 2048, SEEK_SET);
    EXPECT_EQ(2048, newPos);
    
    // シーク後にデータを読んで位置が正しいか確認
    std::vector<uint8_t> readBuffer(100);
    int bytesRead = avio_read(avio, readBuffer.data(), 100);
    ASSERT_EQ(100, bytesRead);
    
    // 読み取ったデータがセグメント2の先頭から100バイトであることを確認
    auto expected = CreateTestSegmentData(2, segmentSize);
    std::vector<uint8_t> expectedData(expected.begin(), expected.begin() + 100);
    EXPECT_EQ(expectedData, readBuffer);
}

TEST_F(HlsCustomAVIOContextTest, AVIOSizeViaContext) {
    const int segmentCount = 3;
    const size_t segmentSize = 1024;
    
    auto playlist = CreateTestPlaylist(segmentCount, 6.0);
    cache_->Initialize(playlist);
    PopulateCacheWithSegments(*cache_, segmentCount, segmentSize);
    
    avioContext_.Initialize(cache_.get(), playlist);
    avioContext_.SetReadTimeout(1000);
    
    // まず全データを読み込んでサイズを更新
    const size_t totalSize = segmentCount * segmentSize;
    std::vector<uint8_t> buffer(totalSize);
    AVIOContext* avio = avioContext_.GetAVIOContext();
    ASSERT_NE(nullptr, avio);
    avio_read(avio, buffer.data(), static_cast<int>(totalSize));
    
    // avio_size経由でサイズ取得
    int64_t size = avio_size(avio);
    EXPECT_EQ(static_cast<int64_t>(totalSize), size);
}

// =============================================================================
// エッジケース
// =============================================================================

TEST_F(HlsCustomAVIOContextTest, ReadWithZeroBuffer) {
    const int segmentCount = 2;
    const size_t segmentSize = 1024;
    
    auto playlist = CreateTestPlaylist(segmentCount, 6.0);
    cache_->Initialize(playlist);
    PopulateCacheWithSegments(*cache_, segmentCount, segmentSize);
    
    avioContext_.Initialize(cache_.get(), playlist);
    avioContext_.SetReadTimeout(1000);
    
    AVIOContext* avio = avioContext_.GetAVIOContext();
    
    // 0バイト読み取り
    std::vector<uint8_t> buffer(1);
    int bytesRead = avio_read(avio, buffer.data(), 0);
    EXPECT_EQ(0, bytesRead);
    EXPECT_EQ(0, avioContext_.GetPosition());
}

TEST_F(HlsCustomAVIOContextTest, MultipleInitialize) {
    const int segmentCount1 = 2;
    const int segmentCount2 = 3;
    const size_t segmentSize = 1024;
    
    auto playlist1 = CreateTestPlaylist(segmentCount1, 6.0);
    auto playlist2 = CreateTestPlaylist(segmentCount2, 6.0);
    
    cache_->Initialize(playlist1);
    PopulateCacheWithSegments(*cache_, segmentCount1, segmentSize);
    
    // 最初の初期化
    ASSERT_TRUE(avioContext_.Initialize(cache_.get(), playlist1));
    
    // 最初のプレイリストでデータを読み込み
    std::vector<uint8_t> buffer1(segmentCount1 * segmentSize);
    AVIOContext* avio1 = avioContext_.GetAVIOContext();
    avio_read(avio1, buffer1.data(), static_cast<int>(buffer1.size()));
    
    // Close後に再初期化
    avioContext_.Close();
    
    cache_->Initialize(playlist2);
    PopulateCacheWithSegments(*cache_, segmentCount2, segmentSize);
    
    ASSERT_TRUE(avioContext_.Initialize(cache_.get(), playlist2));
    AVIOContext* avio2 = avioContext_.GetAVIOContext();
    
    // 新しいAVIOContextが作成されている
    EXPECT_NE(nullptr, avio2);
    
    // 全セグメントを読み込んでサイズを更新
    std::vector<uint8_t> buffer2(segmentCount2 * segmentSize);
    avio_read(avio2, buffer2.data(), static_cast<int>(buffer2.size()));
    
    // 新しいプレイリストのサイズ
    int64_t size = avioContext_.GetSize();
    EXPECT_EQ(static_cast<int64_t>(segmentCount2 * segmentSize), size);
}

// =============================================================================
// main
// =============================================================================

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
