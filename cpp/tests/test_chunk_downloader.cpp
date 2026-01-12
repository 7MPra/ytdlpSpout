// =============================================================================
// test_chunk_downloader.cpp - ChunkDownloaderのユニットテスト
// =============================================================================

#include <iostream>
#include <cassert>
#include <thread>
#include <chrono>
#include <atomic>
#include <vector>
#include <mutex>
#include <condition_variable>
#include "io/ChunkDownloader.h"
#include "io/SparseFileCache.h"
#include "utils/Logger.h"

using namespace ytdlpspout;
using namespace ytdlpspout::io;

using Logger = ytdlpspout::Logger;

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
// ChunkDownloader基本テスト
// =============================================================================

void TestChunkDownloaderConstruction() {
    TEST_CASE("ChunkDownloader construction and destruction")
    {
        SparseFileCacheConfig cacheConfig;
        cacheConfig.chunkSize = 1024;
        cacheConfig.maxMemoryBytes = 1024 * 1024;
        SparseFileCache cache(cacheConfig);
        
        ChunkDownloader downloader(&cache, 2);
        // 構築が成功すればOK
        ASSERT_FALSE(downloader.IsRunning());
    }
    TEST_END()
}

void TestChunkDownloaderStartStop() {
    TEST_CASE("ChunkDownloader start and stop")
    {
        SparseFileCacheConfig cacheConfig;
        cacheConfig.chunkSize = 1024;
        cacheConfig.maxMemoryBytes = 1024 * 1024;
        SparseFileCache cache(cacheConfig);
        cache.Initialize(10240);  // 10KB
        
        ChunkDownloader downloader(&cache, 2);
        downloader.SetUrl("http://example.com/test.bin");
        
        ASSERT_FALSE(downloader.IsRunning());
        
        downloader.Start();
        ASSERT_TRUE(downloader.IsRunning());
        
        downloader.Stop();
        ASSERT_FALSE(downloader.IsRunning());
    }
    TEST_END()
}

void TestChunkDownloaderMultipleStartStop() {
    TEST_CASE("ChunkDownloader multiple start/stop cycles")
    {
        SparseFileCacheConfig cacheConfig;
        cacheConfig.chunkSize = 1024;
        cacheConfig.maxMemoryBytes = 1024 * 1024;
        SparseFileCache cache(cacheConfig);
        cache.Initialize(10240);
        
        ChunkDownloader downloader(&cache, 2);
        downloader.SetUrl("http://example.com/test.bin");
        
        // 複数回のstart/stopサイクル
        for (int i = 0; i < 3; ++i) {
            downloader.Start();
            ASSERT_TRUE(downloader.IsRunning());
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            downloader.Stop();
            ASSERT_FALSE(downloader.IsRunning());
        }
    }
    TEST_END()
}

void TestChunkDownloaderSetHeaders() {
    TEST_CASE("ChunkDownloader set headers")
    {
        SparseFileCacheConfig cacheConfig;
        cacheConfig.chunkSize = 1024;
        cacheConfig.maxMemoryBytes = 1024 * 1024;
        SparseFileCache cache(cacheConfig);
        
        ChunkDownloader downloader(&cache, 2);
        
        std::map<std::string, std::string> headers;
        headers["User-Agent"] = "TestAgent/1.0";
        headers["Cookie"] = "session=abc123";
        
        // ヘッダー設定が例外を投げないことを確認
        downloader.SetHeaders(headers);
    }
    TEST_END()
}

void TestChunkPriorityEnum() {
    TEST_CASE("ChunkPriority enum values")
    {
        // 優先度の順序を確認（Criticalが最高）
        ASSERT_TRUE(static_cast<int>(ChunkPriority::Critical) < static_cast<int>(ChunkPriority::High));
        ASSERT_TRUE(static_cast<int>(ChunkPriority::High) < static_cast<int>(ChunkPriority::Medium));
        ASSERT_TRUE(static_cast<int>(ChunkPriority::Medium) < static_cast<int>(ChunkPriority::Low));
    }
    TEST_END()
}

void TestChunkDownloaderRequestChunk() {
    TEST_CASE("ChunkDownloader request chunk")
    {
        SparseFileCacheConfig cacheConfig;
        cacheConfig.chunkSize = 1024;
        cacheConfig.maxMemoryBytes = 1024 * 1024;
        SparseFileCache cache(cacheConfig);
        cache.Initialize(10240);  // 10KB
        
        ChunkDownloader downloader(&cache, 2);
        downloader.SetUrl("http://example.com/test.bin");
        
        // 開始前にリクエストを追加できる
        downloader.RequestChunk(0, ChunkPriority::High);
        downloader.RequestChunk(1024, ChunkPriority::Medium);
        downloader.RequestChunk(2048, ChunkPriority::Low);
        
        // 例外が発生しなければOK
    }
    TEST_END()
}

void TestChunkDownloaderCancelChunk() {
    TEST_CASE("ChunkDownloader cancel chunk")
    {
        SparseFileCacheConfig cacheConfig;
        cacheConfig.chunkSize = 1024;
        cacheConfig.maxMemoryBytes = 1024 * 1024;
        SparseFileCache cache(cacheConfig);
        cache.Initialize(10240);
        
        ChunkDownloader downloader(&cache, 2);
        downloader.SetUrl("http://example.com/test.bin");
        
        // リクエストを追加してからキャンセル
        downloader.RequestChunk(0, ChunkPriority::High);
        downloader.RequestChunk(1024, ChunkPriority::Medium);
        
        downloader.CancelChunk(0);
        downloader.CancelChunk(1024);
        
        // 例外が発生しなければOK
    }
    TEST_END()
}

void TestChunkDownloaderBandwidth() {
    TEST_CASE("ChunkDownloader bandwidth estimation")
    {
        SparseFileCacheConfig cacheConfig;
        cacheConfig.chunkSize = 1024;
        cacheConfig.maxMemoryBytes = 1024 * 1024;
        SparseFileCache cache(cacheConfig);
        cache.Initialize(10240);
        
        ChunkDownloader downloader(&cache, 2);
        
        // 初期状態では帯域幅は0
        double bandwidth = downloader.GetBandwidth();
        ASSERT_GE(bandwidth, 0.0);
    }
    TEST_END()
}

void TestChunkDownloaderDestructorStops() {
    TEST_CASE("ChunkDownloader destructor stops workers")
    {
        SparseFileCacheConfig cacheConfig;
        cacheConfig.chunkSize = 1024;
        cacheConfig.maxMemoryBytes = 1024 * 1024;
        SparseFileCache cache(cacheConfig);
        cache.Initialize(10240);
        
        {
            ChunkDownloader downloader(&cache, 4);
            downloader.SetUrl("http://example.com/test.bin");
            downloader.Start();
            ASSERT_TRUE(downloader.IsRunning());
            // デストラクタでStopが呼ばれることを確認
        }
        // スコープを抜けた後、クラッシュしなければOK
    }
    TEST_END()
}

void TestChunkDownloaderNullCache() {
    TEST_CASE("ChunkDownloader with null cache")
    {
        bool exceptionThrown = false;
        try {
            ChunkDownloader downloader(nullptr, 2);
        } catch (const std::exception&) {
            exceptionThrown = true;
        }
        ASSERT_TRUE(exceptionThrown);
    }
    TEST_END()
}

void TestChunkDownloaderZeroWorkers() {
    TEST_CASE("ChunkDownloader with zero workers defaults to 1")
    {
        SparseFileCacheConfig cacheConfig;
        cacheConfig.chunkSize = 1024;
        cacheConfig.maxMemoryBytes = 1024 * 1024;
        SparseFileCache cache(cacheConfig);
        
        // 0ワーカーでも動作する（内部で最低1に補正される）
        ChunkDownloader downloader(&cache, 0);
        ASSERT_FALSE(downloader.IsRunning());
    }
    TEST_END()
}

void TestChunkDownloaderPriorityOrder() {
    TEST_CASE("ChunkDownloader priority queue ordering")
    {
        SparseFileCacheConfig cacheConfig;
        cacheConfig.chunkSize = 1024;
        cacheConfig.maxMemoryBytes = 1024 * 1024;
        SparseFileCache cache(cacheConfig);
        cache.Initialize(102400);  // 100KB
        
        ChunkDownloader downloader(&cache, 1);  // 1ワーカーで順序を確認
        
        // 異なる優先度でリクエストを追加
        downloader.RequestChunk(0, ChunkPriority::Low);
        downloader.RequestChunk(1024, ChunkPriority::Medium);
        downloader.RequestChunk(2048, ChunkPriority::High);
        downloader.RequestChunk(3072, ChunkPriority::Critical);
        
        // 例外が発生しなければOK（実際の順序は実ネットワーク必要）
    }
    TEST_END()
}

// =============================================================================
// メイン
// =============================================================================

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "ChunkDownloader Unit Tests" << std::endl;
    std::cout << "========================================" << std::endl;

    Logger::Initialize(false, "", LogLevel::Warn);

    std::cout << "\n[Basic Tests]" << std::endl;
    TestChunkDownloaderConstruction();
    TestChunkDownloaderStartStop();
    TestChunkDownloaderMultipleStartStop();
    TestChunkDownloaderSetHeaders();
    TestChunkPriorityEnum();
    
    std::cout << "\n[Request Tests]" << std::endl;
    TestChunkDownloaderRequestChunk();
    TestChunkDownloaderCancelChunk();
    TestChunkDownloaderBandwidth();
    
    std::cout << "\n[Edge Case Tests]" << std::endl;
    TestChunkDownloaderDestructorStops();
    TestChunkDownloaderNullCache();
    TestChunkDownloaderZeroWorkers();
    TestChunkDownloaderPriorityOrder();

    std::cout << "\n========================================" << std::endl;
    std::cout << "Results: " << s_testsPassed << " passed, " 
              << s_testsFailed << " failed" << std::endl;
    std::cout << "========================================" << std::endl;

    return s_testsFailed > 0 ? 1 : 0;
}
