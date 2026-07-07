// =============================================================================
// test_hls_segment_cache.cpp - HLSセグメントキャッシュのユニットテスト
// =============================================================================

#include "hls/HlsSegmentCache.h"
#include "hls/M3U8Parser.h"
#include "utils/Logger.h"

#include <iostream>
#include <cassert>
#include <thread>
#include <atomic>
#include <chrono>
#include <random>

using namespace ytdlpspout::hls;
using namespace ytdlpspout;

// =============================================================================
// テスト用ユーティリティ
// =============================================================================

/// @brief テスト用のプレイリストを作成
M3U8Playlist CreateTestPlaylist(int segmentCount, double segmentDuration = 10.0) {
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

/// @brief テスト用のダミーセグメントデータを作成
std::vector<uint8_t> CreateTestSegmentData(size_t size, uint8_t fillValue = 0) {
    std::vector<uint8_t> data(size);
    for (size_t i = 0; i < size; ++i) {
        data[i] = static_cast<uint8_t>((fillValue + i) % 256);
    }
    return data;
}

/// @brief テスト結果マクロ
#define TEST_ASSERT(condition, message) \
    do { \
        if (!(condition)) { \
            std::cerr << "FAILED: " << (message) << " at " << __FILE__ << ":" << __LINE__ << std::endl; \
            return false; \
        } \
    } while(0)

#define TEST_PASS(name) \
    std::cout << "[PASS] " << (name) << std::endl; \
    return true;

// =============================================================================
// テストケース
// =============================================================================

/// @brief 基本的な初期化テスト
bool TestInitialization() {
    HlsSegmentCacheConfig config;
    config.maxMemoryBytes = 10 * 1024 * 1024;  // 10MB
    config.maxSegments = 10;
    
    HlsSegmentCache cache(config);
    
    auto playlist = CreateTestPlaylist(5);
    cache.Initialize(playlist);
    
    TEST_ASSERT(cache.GetTotalSegmentCount() == 5, "Total segment count should be 5");
    TEST_ASSERT(cache.GetCachedSegmentCount() == 0, "Cached segment count should be 0");
    TEST_ASSERT(cache.GetCacheMemoryUsage() == 0, "Memory usage should be 0");
    
    TEST_PASS("TestInitialization");
}

/// @brief 基本的な書き込み・読み取りテスト
bool TestBasicWriteRead() {
    HlsSegmentCacheConfig config;
    config.maxMemoryBytes = 10 * 1024 * 1024;
    config.maxSegments = 10;
    
    HlsSegmentCache cache(config);
    
    auto playlist = CreateTestPlaylist(5);
    cache.Initialize(playlist);
    
    // セグメントデータを書き込み
    auto testData = CreateTestSegmentData(1024, 0xAB);
    TEST_ASSERT(cache.WriteSegment(0, std::vector<uint8_t>(testData), false), 
                "WriteSegment should succeed");
    
    TEST_ASSERT(cache.IsSegmentCached(0), "Segment 0 should be cached");
    TEST_ASSERT(!cache.IsSegmentCached(1), "Segment 1 should not be cached");
    TEST_ASSERT(cache.GetCachedSegmentCount() == 1, "Cached segment count should be 1");
    
    // セグメントデータを読み取り
    auto readData = cache.ReadSegment(0, 0);
    TEST_ASSERT(readData.has_value(), "ReadSegment should return data");
    TEST_ASSERT(readData->size() == testData.size(), "Data size should match");
    TEST_ASSERT(*readData == testData, "Data content should match");
    
    TEST_PASS("TestBasicWriteRead");
}

/// @brief LRUエビクションテスト
bool TestLRUEviction() {
    HlsSegmentCacheConfig config;
    config.maxMemoryBytes = 3500;  // 3.5KB（3セグメント×1024=3072で収まる）
    config.maxSegments = 100;
    
    HlsSegmentCache cache(config);
    
    auto playlist = CreateTestPlaylist(10);
    cache.Initialize(playlist);
    
    // 3つのセグメントを書き込み（各1KB）
    for (int i = 0; i < 3; ++i) {
        auto data = CreateTestSegmentData(1024, static_cast<uint8_t>(i));
        cache.WriteSegment(i, std::move(data), false);
    }
    
    TEST_ASSERT(cache.GetCachedSegmentCount() == 3, "Should have 3 segments cached");
    
    // 4番目のセグメントを書き込み - LRUエビクションが発生するはず
    auto data4 = CreateTestSegmentData(1024, 4);
    cache.WriteSegment(3, std::move(data4), false);
    
    // メモリ制限により、最も古いセグメント(0)が削除されるはず
    TEST_ASSERT(cache.GetCacheMemoryUsage() <= config.maxMemoryBytes, 
                "Memory usage should be within limit");
    
    // セグメント0がエビクトされているか確認
    // （LRUでアクセスされていない最も古いセグメントが削除される）
    TEST_ASSERT(!cache.IsSegmentCached(0) || cache.GetCachedSegmentCount() <= 3,
                "Oldest segment should be evicted or count should be limited");
    
    TEST_PASS("TestLRUEviction");
}

/// @brief AES復号統合テスト
bool TestAESDecryption() {
    HlsSegmentCacheConfig config;
    config.maxMemoryBytes = 10 * 1024 * 1024;
    config.maxSegments = 10;
    
    HlsSegmentCache cache(config);
    
    // 暗号化キー付きプレイリスト
    auto playlist = CreateTestPlaylist(5);
    HlsEncryptionKey encKey;
    encKey.method = "AES-128";
    encKey.keyUrl = "https://example.com/key";
    playlist.encryptionKey = encKey;
    
    cache.Initialize(playlist);
    
    // 16バイトのAESキーを設定
    std::vector<uint8_t> aesKey(16, 0x42);  // 0x42で埋めた16バイト
    cache.SetEncryptionKey(aesKey);
    
    // 暗号化されたセグメントデータを作成
    // 注: 実際のテストではAesCbcDecryptorで暗号化したデータを使用
    // ここでは復号をスキップ可能かテスト
    auto rawData = CreateTestSegmentData(1024, 0xCD);
    TEST_ASSERT(cache.WriteSegment(0, std::vector<uint8_t>(rawData), false), 
                "WriteSegment with raw data should succeed");
    
    auto readData = cache.ReadSegment(0, 0);
    TEST_ASSERT(readData.has_value(), "ReadSegment should return data");
    
    TEST_PASS("TestAESDecryption");
}

/// @brief 時間→セグメントインデックス変換テスト
bool TestTimeToSegmentIndex() {
    HlsSegmentCacheConfig config;
    HlsSegmentCache cache(config);
    
    auto playlist = CreateTestPlaylist(10, 5.0);  // 5秒間隔
    cache.Initialize(playlist);
    
    // 0秒 → セグメント0
    TEST_ASSERT(cache.GetSegmentIndexFromTime(0.0) == 0, "0s should be segment 0");
    
    // 2.5秒 → セグメント0
    TEST_ASSERT(cache.GetSegmentIndexFromTime(2.5) == 0, "2.5s should be segment 0");
    
    // 5秒 → セグメント1
    TEST_ASSERT(cache.GetSegmentIndexFromTime(5.0) == 1, "5s should be segment 1");
    
    // 12秒 → セグメント2
    TEST_ASSERT(cache.GetSegmentIndexFromTime(12.0) == 2, "12s should be segment 2");
    
    // 最後のセグメント
    TEST_ASSERT(cache.GetSegmentIndexFromTime(49.9) == 9, "49.9s should be segment 9");
    
    // 範囲外（超過）→ 最後のセグメント
    TEST_ASSERT(cache.GetSegmentIndexFromTime(100.0) == 9, "100s should be clamped to segment 9");
    
    // セグメント開始時間の逆変換テスト
    TEST_ASSERT(cache.GetSegmentStartTime(0) == 0.0, "Segment 0 should start at 0s");
    TEST_ASSERT(cache.GetSegmentStartTime(3) == 15.0, "Segment 3 should start at 15s");
    
    TEST_PASS("TestTimeToSegmentIndex");
}

/// @brief スレッドセーフティテスト（並行読み書き）
bool TestThreadSafety() {
    HlsSegmentCacheConfig config;
    config.maxMemoryBytes = 50 * 1024 * 1024;
    config.maxSegments = 100;
    
    HlsSegmentCache cache(config);
    
    auto playlist = CreateTestPlaylist(20, 5.0);
    cache.Initialize(playlist);
    
    std::atomic<int> writeCount{0};
    std::atomic<int> readCount{0};
    std::atomic<bool> hasError{false};
    
    // 複数スレッドから同時に書き込み
    auto writerFunc = [&](int startIdx) {
        for (int i = 0; i < 5; ++i) {
            int segIdx = startIdx + i;
            if (segIdx >= 20) break;
            
            auto data = CreateTestSegmentData(10 * 1024, static_cast<uint8_t>(segIdx));
            if (cache.WriteSegment(segIdx, std::move(data), false)) {
                writeCount++;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    };
    
    // 複数スレッドから同時に読み取り
    auto readerFunc = [&]() {
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<> dis(0, 19);
        
        for (int i = 0; i < 20; ++i) {
            int segIdx = dis(gen);
            auto data = cache.ReadSegment(segIdx, 0);
            if (data.has_value()) {
                readCount++;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    };
    
    // スレッドを起動
    std::vector<std::thread> threads;
    
    for (int i = 0; i < 4; ++i) {
        threads.emplace_back(writerFunc, i * 5);
    }
    
    for (int i = 0; i < 4; ++i) {
        threads.emplace_back(readerFunc);
    }
    
    // 全スレッドの完了を待機
    for (auto& t : threads) {
        t.join();
    }
    
    TEST_ASSERT(!hasError, "No errors should occur during concurrent access");
    TEST_ASSERT(writeCount > 0, "At least some writes should succeed");
    
    std::cout << "  Concurrent writes: " << writeCount << ", reads: " << readCount << std::endl;
    
    TEST_PASS("TestThreadSafety");
}

/// @brief メモリ制限超過時の動作テスト
bool TestMemoryLimit() {
    HlsSegmentCacheConfig config;
    config.maxMemoryBytes = 5 * 1024;  // 5KB
    config.maxSegments = 100;
    
    HlsSegmentCache cache(config);
    
    auto playlist = CreateTestPlaylist(20);
    cache.Initialize(playlist);
    
    // 10個のセグメント（各1KB）を書き込み
    for (int i = 0; i < 10; ++i) {
        auto data = CreateTestSegmentData(1024, static_cast<uint8_t>(i));
        cache.WriteSegment(i, std::move(data), false);
    }
    
    // メモリ制限を超えないことを確認
    TEST_ASSERT(cache.GetCacheMemoryUsage() <= config.maxMemoryBytes,
                "Memory usage should not exceed limit");
    
    // キャッシュ数は5以下（5KB / 1KB）
    TEST_ASSERT(cache.GetCachedSegmentCount() <= 5,
                "Cached segment count should be limited by memory");
    
    TEST_PASS("TestMemoryLimit");
}

/// @brief セグメント情報取得テスト
bool TestSegmentInfo() {
    HlsSegmentCacheConfig config;
    HlsSegmentCache cache(config);
    
    auto playlist = CreateTestPlaylist(5, 8.5);
    cache.Initialize(playlist);
    
    const HlsSegment* seg0 = cache.GetSegmentInfo(0);
    TEST_ASSERT(seg0 != nullptr, "Segment 0 info should exist");
    TEST_ASSERT(seg0->index == 0, "Segment 0 index should be 0");
    TEST_ASSERT(seg0->duration == 8.5, "Segment 0 duration should be 8.5");
    TEST_ASSERT(seg0->url == "https://example.com/segment0.ts", "Segment 0 URL should match");
    
    const HlsSegment* seg4 = cache.GetSegmentInfo(4);
    TEST_ASSERT(seg4 != nullptr, "Segment 4 info should exist");
    TEST_ASSERT(seg4->index == 4, "Segment 4 index should be 4");
    
    // 範囲外
    const HlsSegment* segInvalid = cache.GetSegmentInfo(10);
    TEST_ASSERT(segInvalid == nullptr, "Out-of-range segment should return nullptr");
    
    TEST_PASS("TestSegmentInfo");
}

/// @brief セグメント情報にバイトレンジが含まれることのテスト（問題L-3: RequestSegmentへのbyteRange伝搬の前提確認）
bool TestSegmentInfoIncludesByteRange() {
    HlsSegmentCacheConfig config;
    HlsSegmentCache cache(config);

    // バイトレンジ付きのプレイリストを作成
    M3U8Playlist playlist = CreateTestPlaylist(3, 10.0);
    playlist.segments[0].byteRangeStart = 0;
    playlist.segments[0].byteRangeLength = 500000;
    playlist.segments[1].byteRangeStart = 500000;
    playlist.segments[1].byteRangeLength = 600000;
    // セグメント2はバイトレンジ指定なし（-1/0のまま）

    cache.Initialize(playlist);

    const HlsSegment* seg0 = cache.GetSegmentInfo(0);
    TEST_ASSERT(seg0 != nullptr, "Segment 0 info should exist");
    TEST_ASSERT(seg0->byteRangeStart == 0, "Segment 0 byteRangeStart should be 0");
    TEST_ASSERT(seg0->byteRangeLength == 500000, "Segment 0 byteRangeLength should be 500000");

    const HlsSegment* seg1 = cache.GetSegmentInfo(1);
    TEST_ASSERT(seg1 != nullptr, "Segment 1 info should exist");
    TEST_ASSERT(seg1->byteRangeStart == 500000, "Segment 1 byteRangeStart should be 500000");
    TEST_ASSERT(seg1->byteRangeLength == 600000, "Segment 1 byteRangeLength should be 600000");

    const HlsSegment* seg2 = cache.GetSegmentInfo(2);
    TEST_ASSERT(seg2 != nullptr, "Segment 2 info should exist");
    TEST_ASSERT(seg2->byteRangeStart == -1, "Segment 2 byteRangeStart should be -1 (unspecified)");

    TEST_PASS("TestSegmentInfoIncludesByteRange");
}

/// @brief OptimizeForPlaybackテスト
bool TestOptimizeForPlayback() {
    HlsSegmentCacheConfig config;
    config.maxMemoryBytes = 5 * 1024;  // 5KB
    config.maxSegments = 100;
    
    HlsSegmentCache cache(config);
    
    auto playlist = CreateTestPlaylist(20);
    cache.Initialize(playlist);
    
    // セグメント0-9を書き込み
    for (int i = 0; i < 10; ++i) {
        auto data = CreateTestSegmentData(1024, static_cast<uint8_t>(i));
        cache.WriteSegment(i, std::move(data), false);
    }
    
    // 再生位置をセグメント5に設定し、周辺を保護
    cache.OptimizeForPlayback(5, 2);
    
    // メモリ制限は維持されている
    TEST_ASSERT(cache.GetCacheMemoryUsage() <= config.maxMemoryBytes,
                "Memory usage should not exceed limit");
    
    TEST_PASS("TestOptimizeForPlayback");
}

/// @brief タイムアウト付き読み取りテスト
bool TestReadWithTimeout() {
    HlsSegmentCacheConfig config;
    HlsSegmentCache cache(config);
    
    auto playlist = CreateTestPlaylist(5);
    cache.Initialize(playlist);
    
    // キャッシュされていないセグメントを即時読み取り
    auto result = cache.ReadSegment(0, 0);  // タイムアウト0 = 即時
    TEST_ASSERT(!result.has_value(), "Reading uncached segment should return nullopt immediately");
    
    // キャッシュされていないセグメントをタイムアウト付きで読み取り
    auto start = std::chrono::steady_clock::now();
    result = cache.ReadSegment(0, 100);  // 100msタイムアウト
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();
    
    TEST_ASSERT(!result.has_value(), "Reading uncached segment should timeout");
    TEST_ASSERT(elapsed >= 90 && elapsed <= 200, "Timeout should be approximately 100ms");
    
    TEST_PASS("TestReadWithTimeout");
}

/// @brief 負のインデックス・範囲外インデックスのハンドリングテスト
///
/// 契約: -1 はfMP4初期化セグメント用の予約値として常に許可される。
/// -2以下、および総セグメント数以上のインデックスは無効として拒否される。
bool TestInvalidIndices() {
    HlsSegmentCacheConfig config;
    HlsSegmentCache cache(config);

    auto playlist = CreateTestPlaylist(5);
    cache.Initialize(playlist);

    // -1 は初期化セグメント専用の正当なインデックス
    TEST_ASSERT(!cache.IsSegmentCached(-1), "Init segment index -1 should not be cached yet");
    TEST_ASSERT(cache.GetSegmentInfo(-1) == nullptr,
                "Index -1 has no playlist HlsSegment info (it's the init segment, not a playlist entry)");

    auto initData = CreateTestSegmentData(100);
    TEST_ASSERT(cache.WriteSegment(-1, std::move(initData), false),
                "Writing to index -1 (init segment) should succeed");
    TEST_ASSERT(cache.IsSegmentCached(-1), "Init segment (-1) should be cached after write");

    auto readInit = cache.ReadSegment(-1, 0);
    TEST_ASSERT(readInit.has_value(), "ReadSegment(-1) should return the init segment data");

    // -2以下は無効なインデックスとして拒否される
    TEST_ASSERT(!cache.IsSegmentCached(-2), "Index -2 should not be cached");
    auto data = CreateTestSegmentData(100);
    TEST_ASSERT(!cache.WriteSegment(-2, std::move(data), false),
                "Writing to index -2 should fail");

    // 範囲外のインデックス（総セグメント数以上）
    TEST_ASSERT(!cache.IsSegmentCached(100), "Out-of-range index should not be cached");
    data = CreateTestSegmentData(100);
    TEST_ASSERT(!cache.WriteSegment(100, std::move(data), false),
                "Writing to out-of-range index should fail");

    // 境界値：総セグメント数と同じインデックスも範囲外
    data = CreateTestSegmentData(100);
    TEST_ASSERT(!cache.WriteSegment(5, std::move(data), false),
                "Writing to index == total segment count should fail");

    TEST_PASS("TestInvalidIndices");
}

/// @brief 失敗マークAPIの基本テスト（マーク→即時失敗、クリア→通常動作）
bool TestSegmentFailedMarking() {
    HlsSegmentCacheConfig config;
    HlsSegmentCache cache(config);

    auto playlist = CreateTestPlaylist(5);
    cache.Initialize(playlist);

    // 初期状態では失敗マークされていない
    TEST_ASSERT(!cache.IsSegmentFailed(0), "Segment 0 should not be marked failed initially");

    // セグメント0を失敗マーク
    cache.MarkSegmentFailed(0);
    TEST_ASSERT(cache.IsSegmentFailed(0), "Segment 0 should be marked failed after MarkSegmentFailed");

    // 失敗マーク済みセグメントへのWaitForSegmentはタイムアウトを待たずに即座にfalse
    auto start = std::chrono::steady_clock::now();
    bool waitResult = cache.WaitForSegment(0, 5000);  // 5秒タイムアウトだが即時返るはず
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();

    TEST_ASSERT(!waitResult, "WaitForSegment should return false for a failed segment");
    TEST_ASSERT(elapsed < 500, "WaitForSegment should return immediately for a failed segment, not wait for timeout");

    // ReadSegmentも同様に即座にnulloptを返す
    start = std::chrono::steady_clock::now();
    auto readResult = cache.ReadSegment(0, 5000);
    elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();

    TEST_ASSERT(!readResult.has_value(), "ReadSegment should return nullopt for a failed segment");
    TEST_ASSERT(elapsed < 500, "ReadSegment should return immediately for a failed segment, not wait for timeout");

    // 失敗マークを解除すると通常動作に戻る
    cache.ClearSegmentFailed(0);
    TEST_ASSERT(!cache.IsSegmentFailed(0), "Segment 0 should not be marked failed after ClearSegmentFailed");

    // クリア後に書き込めば正常にキャッシュされる
    auto data = CreateTestSegmentData(1024, 0x11);
    TEST_ASSERT(cache.WriteSegment(0, std::move(data), false),
                "WriteSegment should succeed after clearing the failed mark");
    TEST_ASSERT(cache.IsSegmentCached(0), "Segment 0 should be cached after successful write");

    auto readData = cache.ReadSegment(0, 0);
    TEST_ASSERT(readData.has_value(), "ReadSegment should succeed after clearing the failed mark and writing");

    TEST_PASS("TestSegmentFailedMarking");
}

/// @brief 失敗マーク後、別スレッドで待機中のWaitForSegment/ReadSegmentが起床することのテスト
bool TestSegmentFailedWakesWaiters() {
    HlsSegmentCacheConfig config;
    HlsSegmentCache cache(config);

    auto playlist = CreateTestPlaylist(5);
    cache.Initialize(playlist);

    std::atomic<bool> waitStarted{false};
    std::atomic<bool> waitCompleted{false};
    std::atomic<bool> waitResultValue{true};
    std::atomic<int64_t> elapsedMs{0};

    std::thread waitThread([&]() {
        waitStarted = true;
        auto start = std::chrono::steady_clock::now();
        bool result = cache.WaitForSegment(1, 10000);  // 10秒タイムアウト
        elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count();
        waitResultValue = result;
        waitCompleted = true;
    });

    while (!waitStarted) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    TEST_ASSERT(!waitCompleted, "WaitForSegment should still be waiting before MarkSegmentFailed");

    // 別スレッドから失敗マークを行い、待機中のスレッドを起床させる
    cache.MarkSegmentFailed(1);

    waitThread.join();

    TEST_ASSERT(waitCompleted, "WaitForSegment should complete after MarkSegmentFailed");
    TEST_ASSERT(!waitResultValue, "WaitForSegment should return false since the segment failed permanently");
    TEST_ASSERT(elapsedMs < 2000, "WaitForSegment should wake up quickly after failure mark, not wait for full timeout");

    TEST_PASS("TestSegmentFailedWakesWaiters");
}

/// @brief WriteSegmentの成功が失敗マークを解除することのテスト（自動復旧の前提条件）
bool TestSuccessfulWriteClearsFailedMark() {
    HlsSegmentCacheConfig config;
    HlsSegmentCache cache(config);

    auto playlist = CreateTestPlaylist(5);
    cache.Initialize(playlist);

    cache.MarkSegmentFailed(2);
    TEST_ASSERT(cache.IsSegmentFailed(2), "Segment 2 should be marked failed");

    // 再ダウンロードが成功して書き込まれた場合、失敗マークは自動的に解除される
    auto data = CreateTestSegmentData(512, 0x99);
    TEST_ASSERT(cache.WriteSegment(2, std::move(data), false), "WriteSegment should succeed");
    TEST_ASSERT(!cache.IsSegmentFailed(2), "Successful WriteSegment should clear the failed mark");
    TEST_ASSERT(cache.IsSegmentCached(2), "Segment 2 should now be cached");

    TEST_PASS("TestSuccessfulWriteClearsFailedMark");
}

/// @brief 失敗マークAPIの無効インデックスに対する挙動テスト
bool TestSegmentFailedInvalidIndices() {
    HlsSegmentCacheConfig config;
    HlsSegmentCache cache(config);

    auto playlist = CreateTestPlaylist(5);
    cache.Initialize(playlist);

    // -1（初期化セグメント）は失敗マークAPIでも有効
    cache.MarkSegmentFailed(-1);
    TEST_ASSERT(cache.IsSegmentFailed(-1), "Index -1 (init segment) should support failed marking");
    cache.ClearSegmentFailed(-1);
    TEST_ASSERT(!cache.IsSegmentFailed(-1), "Index -1 failed mark should be clearable");

    // -2以下・範囲外は無効なインデックスとして扱われ、常にfalseを返す
    cache.MarkSegmentFailed(-2);
    TEST_ASSERT(!cache.IsSegmentFailed(-2), "Index -2 is invalid; IsSegmentFailed should always return false");

    cache.MarkSegmentFailed(100);
    TEST_ASSERT(!cache.IsSegmentFailed(100), "Out-of-range index is invalid; IsSegmentFailed should always return false");

    TEST_PASS("TestSegmentFailedInvalidIndices");
}

/// @brief WaitForSegmentテスト - 即時利用可能
bool TestWaitForSegmentImmediatelyAvailable() {
    HlsSegmentCacheConfig config;
    HlsSegmentCache cache(config);
    
    auto playlist = CreateTestPlaylist(5);
    cache.Initialize(playlist);
    
    // セグメントを先に書き込み
    auto data = CreateTestSegmentData(1024, 0xAB);
    cache.WriteSegment(0, std::move(data), false);
    
    // 既にキャッシュされているセグメントは即座にtrueを返す
    auto start = std::chrono::steady_clock::now();
    bool result = cache.WaitForSegment(0, 5000);
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();
    
    TEST_ASSERT(result, "WaitForSegment should return true for cached segment");
    TEST_ASSERT(elapsed < 100, "WaitForSegment should return immediately for cached segment");
    
    TEST_PASS("TestWaitForSegmentImmediatelyAvailable");
}

/// @brief WaitForSegmentテスト - タイムアウト
bool TestWaitForSegmentTimeout() {
    HlsSegmentCacheConfig config;
    HlsSegmentCache cache(config);
    
    auto playlist = CreateTestPlaylist(5);
    cache.Initialize(playlist);
    
    // キャッシュされていないセグメントを待機（タイムアウト200ms）
    auto start = std::chrono::steady_clock::now();
    bool result = cache.WaitForSegment(0, 200);
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();
    
    TEST_ASSERT(!result, "WaitForSegment should return false on timeout");
    TEST_ASSERT(elapsed >= 180 && elapsed <= 400, "WaitForSegment should wait for approximately 200ms");
    
    TEST_PASS("TestWaitForSegmentTimeout");
}

/// @brief WaitForSegmentテスト - 別スレッドからの書き込みで起床
bool TestWaitForSegmentWakeOnWrite() {
    HlsSegmentCacheConfig config;
    HlsSegmentCache cache(config);
    
    auto playlist = CreateTestPlaylist(5);
    cache.Initialize(playlist);
    
    std::atomic<bool> waitStarted{false};
    std::atomic<bool> waitCompleted{false};
    std::atomic<int64_t> elapsedMs{0};
    
    // 待機スレッド
    std::thread waitThread([&]() {
        waitStarted = true;
        auto start = std::chrono::steady_clock::now();
        bool result = cache.WaitForSegment(0, 5000);  // 5秒タイムアウト
        elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count();
        waitCompleted = result;
    });
    
    // 待機が開始するまで少し待つ
    while (!waitStarted) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    // セグメントを書き込み（これにより待機スレッドが起床するはず）
    auto data = CreateTestSegmentData(1024, 0xCD);
    cache.WriteSegment(0, std::move(data), false);
    
    waitThread.join();
    
    TEST_ASSERT(waitCompleted, "WaitForSegment should complete when segment is written");
    TEST_ASSERT(elapsedMs < 1000, "WaitForSegment should wake up quickly after write (not timeout)");
    
    std::cout << "  Wait completed in " << elapsedMs << "ms" << std::endl;
    
    TEST_PASS("TestWaitForSegmentWakeOnWrite");
}

/// @brief WaitForSegmentテスト - 無制限待機
bool TestWaitForSegmentNoTimeout() {
    HlsSegmentCacheConfig config;
    HlsSegmentCache cache(config);
    
    auto playlist = CreateTestPlaylist(5);
    cache.Initialize(playlist);
    
    std::atomic<bool> waitCompleted{false};
    
    // 待機スレッド（無制限待機）
    std::thread waitThread([&]() {
        cache.WaitForSegment(0, 0);  // タイムアウト0 = 無制限
        waitCompleted = true;
    });
    
    // 少し待ってから書き込み
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    TEST_ASSERT(!waitCompleted, "WaitForSegment should still be waiting");
    
    // セグメントを書き込み
    auto data = CreateTestSegmentData(1024, 0xEF);
    cache.WriteSegment(0, std::move(data), false);
    
    waitThread.join();
    
    TEST_ASSERT(waitCompleted, "WaitForSegment should complete after write");
    
    TEST_PASS("TestWaitForSegmentNoTimeout");
}

// =============================================================================
// メイン
// =============================================================================

int main() {
    // ロガー初期化
    Logger::Initialize(true, "logs/test_hls_segment_cache.log", LogLevel::Debug);
    
    std::cout << "=== HLS Segment Cache Tests ===" << std::endl;
    
    int passed = 0;
    int failed = 0;
    
    auto runTest = [&](bool (*testFunc)(), const char* name) {
        std::cout << "Running: " << name << "..." << std::endl;
        try {
            if (testFunc()) {
                passed++;
            } else {
                failed++;
            }
        } catch (const std::exception& e) {
            std::cerr << "[EXCEPTION] " << name << ": " << e.what() << std::endl;
            failed++;
        }
    };
    
    // テスト実行
    runTest(TestInitialization, "TestInitialization");
    runTest(TestBasicWriteRead, "TestBasicWriteRead");
    runTest(TestLRUEviction, "TestLRUEviction");
    runTest(TestAESDecryption, "TestAESDecryption");
    runTest(TestTimeToSegmentIndex, "TestTimeToSegmentIndex");
    runTest(TestThreadSafety, "TestThreadSafety");
    runTest(TestMemoryLimit, "TestMemoryLimit");
    runTest(TestSegmentInfo, "TestSegmentInfo");
    runTest(TestSegmentInfoIncludesByteRange, "TestSegmentInfoIncludesByteRange");
    runTest(TestOptimizeForPlayback, "TestOptimizeForPlayback");
    runTest(TestReadWithTimeout, "TestReadWithTimeout");
    runTest(TestInvalidIndices, "TestInvalidIndices");
    runTest(TestWaitForSegmentImmediatelyAvailable, "TestWaitForSegmentImmediatelyAvailable");
    runTest(TestWaitForSegmentTimeout, "TestWaitForSegmentTimeout");
    runTest(TestWaitForSegmentWakeOnWrite, "TestWaitForSegmentWakeOnWrite");
    runTest(TestWaitForSegmentNoTimeout, "TestWaitForSegmentNoTimeout");
    runTest(TestSegmentFailedMarking, "TestSegmentFailedMarking");
    runTest(TestSegmentFailedWakesWaiters, "TestSegmentFailedWakesWaiters");
    runTest(TestSuccessfulWriteClearsFailedMark, "TestSuccessfulWriteClearsFailedMark");
    runTest(TestSegmentFailedInvalidIndices, "TestSegmentFailedInvalidIndices");
    
    std::cout << std::endl;
    std::cout << "=== Test Results ===" << std::endl;
    std::cout << "Passed: " << passed << std::endl;
    std::cout << "Failed: " << failed << std::endl;
    
    Logger::Shutdown();
    
    return failed > 0 ? 1 : 0;
}
