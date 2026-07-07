// =============================================================================
// test_sparse_file_cache.cpp - SparseFileCacheのユニットテスト
// =============================================================================

#include <iostream>
#include <cassert>
#include <thread>
#include <chrono>
#include <atomic>
#include <vector>
#include <random>
#include "io/SparseFileCache.h"
#include "utils/Logger.h"

using namespace ytdlpspout;
using namespace ytdlpspout::io;

// =============================================================================
// テストユーティリティ
// =============================================================================

static int s_testsPassed = 0;
static int s_testsFailed = 0;

#define TEST_CASE(name) \
    std::cout << "  Testing: " << name << "..." << std::flush; \
    try {

#define TEST_END() \
        std::cout << " PASSED" << std::endl; \
        s_testsPassed++; \
    } catch (const std::exception& e) { \
        std::cout << " FAILED: " << e.what() << std::endl; \
        s_testsFailed++; \
    }

#define ASSERT_TRUE(expr) \
    if (!(expr)) throw std::runtime_error("Assertion failed: " #expr)

#define ASSERT_FALSE(expr) \
    if (expr) throw std::runtime_error("Assertion failed: !" #expr)

#define ASSERT_EQ(a, b) \
    if ((a) != (b)) throw std::runtime_error("Assertion failed: " #a " != " #b)

#define ASSERT_NE(a, b) \
    if ((a) == (b)) throw std::runtime_error("Assertion failed: " #a " == " #b)

#define ASSERT_GT(a, b) \
    if (!((a) > (b))) throw std::runtime_error("Assertion failed: " #a " > " #b)

#define ASSERT_GE(a, b) \
    if (!((a) >= (b))) throw std::runtime_error("Assertion failed: " #a " >= " #b)

#define ASSERT_LE(a, b) \
    if (!((a) <= (b))) throw std::runtime_error("Assertion failed: " #a " <= " #b)

// =============================================================================
// SparseFileCache 基本テスト
// =============================================================================

void TestConstruction() {
    TEST_CASE("SparseFileCache construction and destruction")
    {
        SparseFileCacheConfig config;
        config.chunkSize = 256 * 1024;  // 256KB
        config.maxMemoryBytes = 64 * 1024 * 1024;  // 64MB
        SparseFileCache cache(config);
        ASSERT_FALSE(cache.IsInitialized());
    }
    TEST_END()
}

void TestInitialization() {
    TEST_CASE("Initialize with file size")
    {
        SparseFileCacheConfig config;
        config.chunkSize = 1024;  // 1KB チャンク（テスト用）
        SparseFileCache cache(config);
        
        cache.Initialize(10 * 1024);  // 10KB ファイル
        
        ASSERT_TRUE(cache.IsInitialized());
        ASSERT_EQ(cache.GetTotalSize(), 10 * 1024);
        ASSERT_EQ(cache.GetChunkSize(), 1024);
        ASSERT_EQ(cache.GetChunkCount(), 10);
    }
    TEST_END()
}

void TestInitializationWithPartialChunk() {
    TEST_CASE("Initialize with partial last chunk")
    {
        SparseFileCacheConfig config;
        config.chunkSize = 1024;
        SparseFileCache cache(config);
        
        cache.Initialize(10 * 1024 + 512);  // 10.5KB ファイル
        
        ASSERT_EQ(cache.GetChunkCount(), 11);  // 10 full + 1 partial
        ASSERT_EQ(cache.GetActualChunkSize(10), 512);  // 最後のチャンクは512バイト
        ASSERT_EQ(cache.GetActualChunkSize(0), 1024);  // 他のチャンクは1024バイト
    }
    TEST_END()
}

void TestChunkIndexCalculation() {
    TEST_CASE("Chunk index calculation")
    {
        SparseFileCacheConfig config;
        config.chunkSize = 1024;
        SparseFileCache cache(config);
        
        cache.Initialize(10 * 1024);
        
        ASSERT_EQ(cache.GetChunkIndex(0), 0);
        ASSERT_EQ(cache.GetChunkIndex(1023), 0);
        ASSERT_EQ(cache.GetChunkIndex(1024), 1);
        ASSERT_EQ(cache.GetChunkIndex(2048), 2);
        ASSERT_EQ(cache.GetChunkIndex(9 * 1024), 9);
    }
    TEST_END()
}

void TestByteOffsetCalculation() {
    TEST_CASE("Byte offset calculation")
    {
        SparseFileCacheConfig config;
        config.chunkSize = 1024;
        SparseFileCache cache(config);
        
        cache.Initialize(10 * 1024);
        
        ASSERT_EQ(cache.GetByteOffset(0), 0);
        ASSERT_EQ(cache.GetByteOffset(1), 1024);
        ASSERT_EQ(cache.GetByteOffset(5), 5 * 1024);
    }
    TEST_END()
}

// =============================================================================
// チャンク状態管理テスト
// =============================================================================

void TestChunkStateInitial() {
    TEST_CASE("Initial chunk state is Empty")
    {
        SparseFileCacheConfig config;
        config.chunkSize = 1024;
        SparseFileCache cache(config);
        
        cache.Initialize(5 * 1024);
        
        for (int i = 0; i < 5; ++i) {
            ASSERT_EQ(cache.GetChunkState(i), ChunkState::Empty);
        }
    }
    TEST_END()
}

void TestSetChunkState() {
    TEST_CASE("Set chunk state")
    {
        SparseFileCacheConfig config;
        config.chunkSize = 1024;
        SparseFileCache cache(config);
        
        cache.Initialize(5 * 1024);
        
        cache.SetChunkState(0, ChunkState::Pending);
        ASSERT_EQ(cache.GetChunkState(0), ChunkState::Pending);
        
        cache.SetChunkState(0, ChunkState::Downloading);
        ASSERT_EQ(cache.GetChunkState(0), ChunkState::Downloading);
        
        cache.SetChunkState(0, ChunkState::Error);
        ASSERT_EQ(cache.GetChunkState(0), ChunkState::Error);
    }
    TEST_END()
}

// =============================================================================
// チャンク読み書きテスト
// =============================================================================

void TestWriteAndReadChunk() {
    TEST_CASE("Write and read chunk data")
    {
        SparseFileCacheConfig config;
        config.chunkSize = 1024;
        SparseFileCache cache(config);
        
        cache.Initialize(5 * 1024);
        
        // テストデータ作成
        std::vector<uint8_t> writeData(1024);
        for (size_t i = 0; i < writeData.size(); ++i) {
            writeData[i] = static_cast<uint8_t>(i % 256);
        }
        
        // 書き込み
        ASSERT_TRUE(cache.WriteChunk(0, writeData.data(), writeData.size()));
        ASSERT_EQ(cache.GetChunkState(0), ChunkState::Cached);
        
        // 読み込み
        std::vector<uint8_t> readData(1024);
        int64_t bytesRead = cache.ReadChunk(0, readData.data(), readData.size());
        
        ASSERT_EQ(bytesRead, 1024);
        ASSERT_EQ(readData, writeData);
    }
    TEST_END()
}

void TestWritePartialChunk() {
    TEST_CASE("Write to partial last chunk")
    {
        SparseFileCacheConfig config;
        config.chunkSize = 1024;
        SparseFileCache cache(config);
        
        cache.Initialize(1024 + 512);  // 1.5KB
        
        // 最後のチャンク（512バイト）に書き込み
        std::vector<uint8_t> writeData(512, 0xAB);
        ASSERT_TRUE(cache.WriteChunk(1, writeData.data(), writeData.size()));
        
        // 読み込み確認
        std::vector<uint8_t> readData(512);
        int64_t bytesRead = cache.ReadChunk(1, readData.data(), readData.size());
        
        ASSERT_EQ(bytesRead, 512);
        ASSERT_EQ(readData, writeData);
    }
    TEST_END()
}

void TestReadUnwrittenChunk() {
    TEST_CASE("Read unwritten chunk returns -1")
    {
        SparseFileCacheConfig config;
        config.chunkSize = 1024;
        SparseFileCache cache(config);
        
        cache.Initialize(5 * 1024);
        
        std::vector<uint8_t> readData(1024);
        int64_t bytesRead = cache.ReadChunk(0, readData.data(), readData.size());
        
        ASSERT_EQ(bytesRead, -1);  // チャンクがキャッシュされていない
    }
    TEST_END()
}

// =============================================================================
// バイト単位読み取りテスト（複数チャンクにまたがる場合）
// =============================================================================

void TestReadAcrossChunks() {
    TEST_CASE("Read data across multiple chunks")
    {
        SparseFileCacheConfig config;
        config.chunkSize = 1024;
        SparseFileCache cache(config);
        
        cache.Initialize(3 * 1024);
        
        // 3つのチャンクにデータを書き込み
        for (int64_t i = 0; i < 3; ++i) {
            std::vector<uint8_t> data(1024, static_cast<uint8_t>(i + 1));
            cache.WriteChunk(i, data.data(), data.size());
        }
        
        // チャンク境界をまたいで読み取り（チャンク0の後半とチャンク1の前半）
        std::vector<uint8_t> readData(1024);
        int64_t bytesRead = cache.Read(512, readData.data(), 1024, 1000);
        
        ASSERT_EQ(bytesRead, 1024);
        
        // 最初の512バイトはチャンク0のデータ（1）
        for (int i = 0; i < 512; ++i) {
            ASSERT_EQ(readData[i], 1);
        }
        // 後半512バイトはチャンク1のデータ（2）
        for (int i = 512; i < 1024; ++i) {
            ASSERT_EQ(readData[i], 2);
        }
    }
    TEST_END()
}

// =============================================================================
// LRUエビクションテスト
// =============================================================================

void TestLRUEviction() {
    TEST_CASE("LRU eviction when memory limit exceeded")
    {
        SparseFileCacheConfig config;
        config.chunkSize = 1024;
        config.maxMemoryBytes = 3 * 1024;  // 3チャンク分のみ
        SparseFileCache cache(config);
        
        cache.Initialize(10 * 1024);
        
        // 5チャンク書き込み（上限超過）
        for (int64_t i = 0; i < 5; ++i) {
            std::vector<uint8_t> data(1024, static_cast<uint8_t>(i));
            cache.WriteChunk(i, data.data(), data.size());
        }
        
        // メモリ使用量が上限を超えないことを確認
        ASSERT_LE(cache.GetMemoryUsage(), 3 * 1024);
        
        // 最新のチャンクはキャッシュされているはず
        ASSERT_EQ(cache.GetChunkState(4), ChunkState::Cached);
        
        // 古いチャンクは削除されているはず
        // 注: LRUなので、最初に書き込んだチャンクが削除される
        ASSERT_EQ(cache.GetChunkState(0), ChunkState::Empty);
        ASSERT_EQ(cache.GetChunkState(1), ChunkState::Empty);
    }
    TEST_END()
}

void TestLRUAccessUpdate() {
    TEST_CASE("LRU access time update on read")
    {
        SparseFileCacheConfig config;
        config.chunkSize = 1024;
        config.maxMemoryBytes = 3 * 1024;  // 3チャンク分のみ
        SparseFileCache cache(config);
        
        cache.Initialize(10 * 1024);
        
        // 3チャンク書き込み
        for (int64_t i = 0; i < 3; ++i) {
            std::vector<uint8_t> data(1024, static_cast<uint8_t>(i));
            cache.WriteChunk(i, data.data(), data.size());
        }
        
        // チャンク0を読み込んでアクセス時刻を更新
        std::vector<uint8_t> readData(1024);
        cache.ReadChunk(0, readData.data(), readData.size());
        
        // 新しいチャンクを書き込み
        std::vector<uint8_t> newData(1024, 0xFF);
        cache.WriteChunk(3, newData.data(), newData.size());
        
        // チャンク0は最近アクセスされたので残る
        ASSERT_EQ(cache.GetChunkState(0), ChunkState::Cached);
        
        // チャンク1が最も古いのでエビクトされる
        ASSERT_EQ(cache.GetChunkState(1), ChunkState::Empty);
    }
    TEST_END()
}

// =============================================================================
// メモリ使用量統計テスト
// =============================================================================

void TestMemoryUsageTracking() {
    TEST_CASE("Memory usage tracking")
    {
        SparseFileCacheConfig config;
        config.chunkSize = 1024;
        config.maxMemoryBytes = 64 * 1024 * 1024;
        SparseFileCache cache(config);
        
        cache.Initialize(5 * 1024);
        
        ASSERT_EQ(cache.GetMemoryUsage(), 0);
        ASSERT_EQ(cache.GetCachedChunkCount(), 0);
        
        // チャンク書き込み
        std::vector<uint8_t> data(1024, 0);
        cache.WriteChunk(0, data.data(), data.size());
        
        ASSERT_EQ(cache.GetMemoryUsage(), 1024);
        ASSERT_EQ(cache.GetCachedChunkCount(), 1);
        
        cache.WriteChunk(1, data.data(), data.size());
        
        ASSERT_EQ(cache.GetMemoryUsage(), 2048);
        ASSERT_EQ(cache.GetCachedChunkCount(), 2);
    }
    TEST_END()
}

// =============================================================================
// スレッドセーフティテスト
// =============================================================================

void TestConcurrentWrite() {
    TEST_CASE("Concurrent write operations")
    {
        SparseFileCacheConfig config;
        config.chunkSize = 1024;
        config.maxMemoryBytes = 64 * 1024 * 1024;
        SparseFileCache cache(config);
        
        cache.Initialize(100 * 1024);  // 100チャンク
        
        std::atomic<int> successCount{0};
        std::vector<std::thread> threads;
        
        // 10スレッドで並行書き込み
        for (int t = 0; t < 10; ++t) {
            threads.emplace_back([&cache, &successCount, t]() {
                for (int i = 0; i < 10; ++i) {
                    int64_t chunkIndex = t * 10 + i;
                    std::vector<uint8_t> data(1024, static_cast<uint8_t>(chunkIndex));
                    if (cache.WriteChunk(chunkIndex, data.data(), data.size())) {
                        successCount++;
                    }
                }
            });
        }
        
        for (auto& t : threads) {
            t.join();
        }
        
        ASSERT_EQ(successCount.load(), 100);
        ASSERT_EQ(cache.GetCachedChunkCount(), 100);
    }
    TEST_END()
}

void TestConcurrentReadWrite() {
    TEST_CASE("Concurrent read and write operations")
    {
        SparseFileCacheConfig config;
        config.chunkSize = 1024;
        config.maxMemoryBytes = 64 * 1024 * 1024;
        SparseFileCache cache(config);
        
        cache.Initialize(10 * 1024);
        
        // まず全チャンクを書き込み
        for (int64_t i = 0; i < 10; ++i) {
            std::vector<uint8_t> data(1024, static_cast<uint8_t>(i));
            cache.WriteChunk(i, data.data(), data.size());
        }
        
        std::atomic<bool> hasError{false};
        std::vector<std::thread> threads;
        
        // 読み込みスレッド
        for (int t = 0; t < 5; ++t) {
            threads.emplace_back([&cache, &hasError]() {
                for (int i = 0; i < 100; ++i) {
                    int64_t chunkIndex = i % 10;
                    std::vector<uint8_t> data(1024);
                    int64_t bytesRead = cache.ReadChunk(chunkIndex, data.data(), data.size());
                    if (bytesRead != 1024) {
                        hasError = true;
                    }
                }
            });
        }
        
        // 書き込みスレッド（同じチャンクに再書き込み）
        for (int t = 0; t < 5; ++t) {
            threads.emplace_back([&cache]() {
                for (int i = 0; i < 100; ++i) {
                    int64_t chunkIndex = i % 10;
                    std::vector<uint8_t> data(1024, static_cast<uint8_t>(chunkIndex));
                    cache.WriteChunk(chunkIndex, data.data(), data.size());
                }
            });
        }
        
        for (auto& t : threads) {
            t.join();
        }
        
        ASSERT_FALSE(hasError);
    }
    TEST_END()
}

// =============================================================================
// チャンク要求コールバックテスト
// =============================================================================

void TestChunkRequestCallback() {
    TEST_CASE("Chunk request callback")
    {
        SparseFileCacheConfig config;
        config.chunkSize = 1024;
        SparseFileCache cache(config);
        
        cache.Initialize(5 * 1024);
        
        std::vector<int64_t> requestedChunks;
        cache.SetChunkRequestCallback([&requestedChunks](int64_t chunkIndex, [[maybe_unused]] int64_t offset, [[maybe_unused]] size_t size) {
            requestedChunks.push_back(chunkIndex);
        });
        
        cache.RequestChunk(0);
        cache.RequestChunk(2);
        cache.RequestChunk(4);
        
        ASSERT_EQ(requestedChunks.size(), 3);
        ASSERT_EQ(requestedChunks[0], 0);
        ASSERT_EQ(requestedChunks[1], 2);
        ASSERT_EQ(requestedChunks[2], 4);
    }
    TEST_END()
}

// =============================================================================
// Clearテスト
// =============================================================================

void TestClear() {
    TEST_CASE("Clear cache")
    {
        SparseFileCacheConfig config;
        config.chunkSize = 1024;
        SparseFileCache cache(config);
        
        cache.Initialize(5 * 1024);
        
        // データ書き込み
        std::vector<uint8_t> data(1024, 0xAB);
        cache.WriteChunk(0, data.data(), data.size());
        cache.WriteChunk(1, data.data(), data.size());
        
        ASSERT_EQ(cache.GetCachedChunkCount(), 2);
        
        // クリア
        cache.Clear();
        
        ASSERT_FALSE(cache.IsInitialized());
        ASSERT_EQ(cache.GetMemoryUsage(), 0);
    }
    TEST_END()
}

// =============================================================================
// WaitForChunkテスト
// =============================================================================

void TestWaitForChunk() {
    TEST_CASE("Wait for chunk with timeout")
    {
        SparseFileCacheConfig config;
        config.chunkSize = 1024;
        SparseFileCache cache(config);
        
        cache.Initialize(5 * 1024);
        
        // 別スレッドで遅延書き込み
        std::thread writer([&cache]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            std::vector<uint8_t> data(1024, 0xAB);
            cache.WriteChunk(0, data.data(), data.size());
        });
        
        // チャンクを待機
        bool result = cache.WaitForChunk(0, 5000);  // 5秒タイムアウト
        
        writer.join();
        
        ASSERT_TRUE(result);
        ASSERT_EQ(cache.GetChunkState(0), ChunkState::Cached);
    }
    TEST_END()
}

void TestWaitForChunkTimeout() {
    TEST_CASE("Wait for chunk timeout")
    {
        SparseFileCacheConfig config;
        config.chunkSize = 1024;
        SparseFileCache cache(config);
        
        cache.Initialize(5 * 1024);
        
        // 短いタイムアウトで待機（書き込みなし）
        auto start = std::chrono::steady_clock::now();
        bool result = cache.WaitForChunk(0, 100);  // 100msタイムアウト
        auto elapsed = std::chrono::steady_clock::now() - start;
        
        ASSERT_FALSE(result);
        ASSERT_GE(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(), 90);
    }
    TEST_END()
}

// =============================================================================
// チャンク失敗カウント・恒久失敗テスト (Issue B-IO)
// =============================================================================

void TestChunkFailCountIncrementsOnError() {
    TEST_CASE("Chunk fail count increments on each Error transition")
    {
        SparseFileCacheConfig config;
        config.chunkSize = 1024;
        SparseFileCache cache(config);

        cache.Initialize(5 * 1024);

        ASSERT_EQ(cache.GetChunkFailCount(0), 0);

        cache.SetChunkState(0, ChunkState::Error);
        ASSERT_EQ(cache.GetChunkFailCount(0), 1);

        cache.SetChunkState(0, ChunkState::Error);
        ASSERT_EQ(cache.GetChunkFailCount(0), 2);
    }
    TEST_END()
}

void TestChunkFailCountResetOnSuccess() {
    TEST_CASE("Chunk fail count resets to 0 after successful WriteChunk")
    {
        SparseFileCacheConfig config;
        config.chunkSize = 1024;
        SparseFileCache cache(config);

        cache.Initialize(5 * 1024);

        cache.SetChunkState(0, ChunkState::Error);
        cache.SetChunkState(0, ChunkState::Error);
        ASSERT_EQ(cache.GetChunkFailCount(0), 2);

        std::vector<uint8_t> data(1024, 0xEF);
        ASSERT_TRUE(cache.WriteChunk(0, data.data(), data.size()));

        ASSERT_EQ(cache.GetChunkFailCount(0), 0);
        ASSERT_EQ(cache.GetChunkState(0), ChunkState::Cached);
    }
    TEST_END()
}

void TestChunkFailCountPreservedAcrossEmptyReset() {
    TEST_CASE("Chunk fail count is preserved when state is reset to Empty (PrefetchScheduler retry pattern)")
    {
        SparseFileCacheConfig config;
        config.chunkSize = 1024;
        SparseFileCache cache(config);

        cache.Initialize(5 * 1024);

        cache.SetChunkState(0, ChunkState::Error);
        ASSERT_EQ(cache.GetChunkFailCount(0), 1);

        // PrefetchSchedulerが再リクエスト前に行うのと同様にEmptyへリセット
        cache.SetChunkState(0, ChunkState::Empty);
        ASSERT_EQ(cache.GetChunkState(0), ChunkState::Empty);

        // リセットしても失敗カウントは保持される
        ASSERT_EQ(cache.GetChunkFailCount(0), 1);
    }
    TEST_END()
}

void TestReadReturnsErrorImmediatelyAfterMaxFailures() {
    TEST_CASE("Read returns -1 immediately (no timeout wait) once chunk exceeds max fail count")
    {
        SparseFileCacheConfig config;
        config.chunkSize = 1024;
        SparseFileCache cache(config);

        cache.Initialize(5 * 1024);

        // kMaxChunkFailCount+1回、Error状態に遷移させて恒久的失敗をシミュレートする
        for (int i = 0; i <= kMaxChunkFailCount; ++i) {
            cache.SetChunkState(0, ChunkState::Error);
        }
        ASSERT_GT(cache.GetChunkFailCount(0), kMaxChunkFailCount);

        std::vector<uint8_t> readData(1024);
        auto start = std::chrono::steady_clock::now();
        // 大きめのタイムアウトを指定しても、待たずに即座に返るはず
        int64_t bytesRead = cache.Read(0, readData.data(), readData.size(), 5000);
        auto elapsed = std::chrono::steady_clock::now() - start;

        ASSERT_EQ(bytesRead, -1);
        ASSERT_LE(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(), 1000);
    }
    TEST_END()
}

void TestReadWakesImmediatelyOnErrorTransition() {
    TEST_CASE("Read wakes immediately (not after full timeout) when a chunk reaches permanent Error while waiting")
    {
        SparseFileCacheConfig config;
        config.chunkSize = 1024;
        SparseFileCache cache(config);

        cache.Initialize(5 * 1024);

        std::thread failer([&cache]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
            for (int i = 0; i <= kMaxChunkFailCount; ++i) {
                cache.SetChunkState(0, ChunkState::Error);
            }
        });

        std::vector<uint8_t> readData(1024);
        auto start = std::chrono::steady_clock::now();
        int64_t bytesRead = cache.Read(0, readData.data(), readData.size(), 10000);  // 10秒タイムアウト
        auto elapsed = std::chrono::steady_clock::now() - start;

        failer.join();

        ASSERT_EQ(bytesRead, -1);
        // 10秒のタイムアウトを待たず、Error遷移直後（150ms程度+マージン）に返ったはず
        ASSERT_LE(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(), 2000);
    }
    TEST_END()
}

void TestReadSucceedsAfterErrorRecovery() {
    TEST_CASE("Read succeeds after chunk recovers from Error via Empty then Cached (below fail threshold)")
    {
        SparseFileCacheConfig config;
        config.chunkSize = 1024;
        SparseFileCache cache(config);

        cache.Initialize(5 * 1024);

        // 上限未満の失敗（1回）
        cache.SetChunkState(0, ChunkState::Error);
        ASSERT_LE(cache.GetChunkFailCount(0), kMaxChunkFailCount);

        // PrefetchSchedulerが行うのと同様にEmptyへリセットしてから再ダウンロードする
        cache.SetChunkState(0, ChunkState::Empty);

        // 別スレッドで遅延書き込み（再ダウンロード成功をシミュレート）
        std::thread writer([&cache]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            std::vector<uint8_t> data(1024, 0xCD);
            cache.WriteChunk(0, data.data(), data.size());
        });

        std::vector<uint8_t> readData(1024);
        int64_t bytesRead = cache.Read(0, readData.data(), readData.size(), 5000);

        writer.join();

        ASSERT_EQ(bytesRead, 1024);
        ASSERT_EQ(cache.GetChunkState(0), ChunkState::Cached);
        ASSERT_EQ(cache.GetChunkFailCount(0), 0);  // 成功でリセットされている
    }
    TEST_END()
}

// =============================================================================
// メイン関数
// =============================================================================

int main() {
    std::cout << "=== SparseFileCache Unit Tests ===" << std::endl;
    
    // ロガー初期化
    Logger::Initialize(false, "", LogLevel::Warn);
    
    std::cout << "\n--- Basic Tests ---" << std::endl;
    TestConstruction();
    TestInitialization();
    TestInitializationWithPartialChunk();
    TestChunkIndexCalculation();
    TestByteOffsetCalculation();
    
    std::cout << "\n--- Chunk State Tests ---" << std::endl;
    TestChunkStateInitial();
    TestSetChunkState();
    
    std::cout << "\n--- Read/Write Tests ---" << std::endl;
    TestWriteAndReadChunk();
    TestWritePartialChunk();
    TestReadUnwrittenChunk();
    TestReadAcrossChunks();
    
    std::cout << "\n--- LRU Eviction Tests ---" << std::endl;
    TestLRUEviction();
    TestLRUAccessUpdate();
    
    std::cout << "\n--- Memory Usage Tests ---" << std::endl;
    TestMemoryUsageTracking();
    
    std::cout << "\n--- Thread Safety Tests ---" << std::endl;
    TestConcurrentWrite();
    TestConcurrentReadWrite();
    
    std::cout << "\n--- Callback Tests ---" << std::endl;
    TestChunkRequestCallback();
    
    std::cout << "\n--- Clear Tests ---" << std::endl;
    TestClear();
    
    std::cout << "\n--- Wait Tests ---" << std::endl;
    TestWaitForChunk();
    TestWaitForChunkTimeout();

    std::cout << "\n--- Chunk Fail Count / Permanent Error Tests (Issue B-IO) ---" << std::endl;
    TestChunkFailCountIncrementsOnError();
    TestChunkFailCountResetOnSuccess();
    TestChunkFailCountPreservedAcrossEmptyReset();
    TestReadReturnsErrorImmediatelyAfterMaxFailures();
    TestReadWakesImmediatelyOnErrorTransition();
    TestReadSucceedsAfterErrorRecovery();

    // 結果サマリー
    std::cout << "\n=== Test Summary ===" << std::endl;
    std::cout << "Passed: " << s_testsPassed << std::endl;
    std::cout << "Failed: " << s_testsFailed << std::endl;
    
    Logger::Shutdown();
    
    return s_testsFailed > 0 ? 1 : 0;
}
