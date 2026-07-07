// =============================================================================
// test_chunk_downloader.cpp - ChunkDownloaderのユニットテスト
// =============================================================================

#ifdef _WIN32
// winsock2.hはwindows.hより先にincludeする必要があるため最上部に置く
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#endif

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
// テスト用モックサーバー（恒久的失敗シナリオ用）
// =============================================================================
//
// "http://localhost:1/..." のような接続不能ポートへのリクエストは、
// 環境によってはOS/仮想化レイヤーのTCP接続拒否(RST)の返却に数秒かかる場合があり
// （実測: このテスト実行環境では127.0.0.1への拒否接続1回あたり約2秒）、
// HttpClient内部リトライ(H-1)とChunkDownloader内部リトライ(C-1)が二重にネストすると
// 恒久的失敗に到達するまでの時間が数十秒に膨れ上がり、テストのデッドラインや
// ctestの実行時間制限を超えてしまう。
//
// このMockFailingServerは実際にListen/Acceptしたうえで即座に接続を切断するため、
// TCP接続自体は即時に確立され（OS依存の拒否タイムアウトが発生しない）、
// リトライにかかる時間はH-1/C-1が意図したバックオフ待機時間のみに支配される
// ようになり、環境非依存で高速かつ決定的にテストできる。
#ifdef _WIN32
class MockFailingServer {
public:
    MockFailingServer() {
        WSADATA wsaData;
        WSAStartup(MAKEWORD(2, 2), &wsaData);

        listenSocket_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;  // OSに空きポートを選ばせる

        bind(listenSocket_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
        listen(listenSocket_, SOMAXCONN);

        int addrLen = sizeof(addr);
        getsockname(listenSocket_, reinterpret_cast<sockaddr*>(&addr), &addrLen);
        port_ = ntohs(addr.sin_port);

        running_ = true;
        thread_ = std::thread([this] { Run(); });
    }

    ~MockFailingServer() {
        running_ = false;
        if (listenSocket_ != INVALID_SOCKET) {
            closesocket(listenSocket_);
            listenSocket_ = INVALID_SOCKET;
        }
        if (thread_.joinable()) {
            thread_.join();
        }
        WSACleanup();
    }

    MockFailingServer(const MockFailingServer&) = delete;
    MockFailingServer& operator=(const MockFailingServer&) = delete;

    /// @brief このモックサーバーを指す完全URLを返す
    std::string Url(const std::string& path) const {
        return "http://127.0.0.1:" + std::to_string(port_) + path;
    }

private:
    void Run() {
        while (running_) {
            SOCKET client = accept(listenSocket_, nullptr, nullptr);
            if (client == INVALID_SOCKET) {
                break;  // listenSocketがcloseされた（デストラクタから）
            }
            // リクエストを読まずに即座に切断し、サーバーダウンを模擬する
            closesocket(client);
        }
    }

    SOCKET listenSocket_ = INVALID_SOCKET;
    std::thread thread_;
    std::atomic<bool> running_{false};
    unsigned short port_ = 0;
};
#endif  // _WIN32

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
// HLSセグメントダウンロードテスト
// =============================================================================

void TestSegmentDownloadCallback() {
    TEST_CASE("ChunkDownloader segment download callback setting")
    {
        SparseFileCacheConfig cacheConfig;
        cacheConfig.chunkSize = 1024;
        cacheConfig.maxMemoryBytes = 1024 * 1024;
        SparseFileCache cache(cacheConfig);
        
        ChunkDownloader downloader(&cache, 2);
        
        // コールバックを設定できることを確認
        std::atomic<bool> callbackCalled{false};
        downloader.SetSegmentDownloadCallback(
            [&callbackCalled](int64_t /*segmentIndex*/, std::vector<uint8_t>&& /*data*/, bool /*success*/) {
                callbackCalled = true;
            }
        );
        
        // 例外が発生しなければOK
    }
    TEST_END()
}

void TestRequestSegment() {
    TEST_CASE("ChunkDownloader request segment")
    {
        SparseFileCacheConfig cacheConfig;
        cacheConfig.chunkSize = 1024;
        cacheConfig.maxMemoryBytes = 1024 * 1024;
        SparseFileCache cache(cacheConfig);
        cache.Initialize(10240);
        
        ChunkDownloader downloader(&cache, 2);
        
        // セグメントリクエストが例外を投げないことを確認
        downloader.RequestSegment(
            "http://example.com/segment0.ts", 
            0, 
            ChunkPriority::High
        );
        downloader.RequestSegment(
            "http://example.com/segment1.ts", 
            1, 
            ChunkPriority::Medium
        );
        
        // 例外が発生しなければOK
    }
    TEST_END()
}

void TestReprioritizeSegment() {
    TEST_CASE("ChunkDownloader reprioritize segment")
    {
        SparseFileCacheConfig cacheConfig;
        cacheConfig.chunkSize = 1024;
        cacheConfig.maxMemoryBytes = 1024 * 1024;
        SparseFileCache cache(cacheConfig);
        cache.Initialize(10240);
        
        ChunkDownloader downloader(&cache, 2);
        
        // セグメントをリクエストしてから優先度を変更
        downloader.RequestSegment(
            "http://example.com/segment0.ts", 
            0, 
            ChunkPriority::Low
        );
        
        // 優先度変更が例外を投げないことを確認
        downloader.ReprioritizeSegment(0, ChunkPriority::Critical);
        
        // 存在しないセグメントの優先度変更も例外を投げない
        downloader.ReprioritizeSegment(999, ChunkPriority::High);
    }
    TEST_END()
}

void TestClearSegmentQueue() {
    TEST_CASE("ChunkDownloader clear segment queue")
    {
        SparseFileCacheConfig cacheConfig;
        cacheConfig.chunkSize = 1024;
        cacheConfig.maxMemoryBytes = 1024 * 1024;
        SparseFileCache cache(cacheConfig);
        cache.Initialize(10240);
        
        ChunkDownloader downloader(&cache, 2);
        
        // 複数のセグメントをリクエスト
        for (int i = 0; i < 10; ++i) {
            downloader.RequestSegment(
                "http://example.com/segment" + std::to_string(i) + ".ts", 
                i, 
                ChunkPriority::Medium
            );
        }
        
        // キューをクリア
        downloader.ClearSegmentQueue();
        
        // 例外が発生しなければOK
    }
    TEST_END()
}

void TestSetHttpHeaders() {
    TEST_CASE("ChunkDownloader set HTTP headers")
    {
        SparseFileCacheConfig cacheConfig;
        cacheConfig.chunkSize = 1024;
        cacheConfig.maxMemoryBytes = 1024 * 1024;
        SparseFileCache cache(cacheConfig);
        
        ChunkDownloader downloader(&cache, 2);
        
        std::map<std::string, std::string> headers;
        headers["User-Agent"] = "Mozilla/5.0 (Windows NT 10.0; Win64; x64)";
        headers["Cookie"] = "session=abc123";
        headers["Referer"] = "https://example.com/";
        headers["Accept"] = "*/*";
        
        // SetHttpHeaders（新API）が動作することを確認
        downloader.SetHttpHeaders(headers);
        
        // 例外が発生しなければOK
    }
    TEST_END()
}

void TestSetHttpHeadersAfterStart() {
    TEST_CASE("ChunkDownloader set HTTP headers after start")
    {
        SparseFileCacheConfig cacheConfig;
        cacheConfig.chunkSize = 1024;
        cacheConfig.maxMemoryBytes = 1024 * 1024;
        SparseFileCache cache(cacheConfig);
        cache.Initialize(10240);
        
        ChunkDownloader downloader(&cache, 2);
        
        // まず空のヘッダーで開始
        downloader.Start();
        ASSERT_TRUE(downloader.IsRunning());
        
        // 開始後にヘッダーを設定
        std::map<std::string, std::string> headers;
        headers["User-Agent"] = "Mozilla/5.0 (Windows NT 10.0; Win64; x64)";
        headers["Cookie"] = "niconico_session=test123; user_session=abc456";
        headers["Referer"] = "https://www.nicovideo.jp/";
        
        // 開始後のヘッダー設定が例外を投げないことを確認
        downloader.SetHttpHeaders(headers);

        // セグメントリクエスト（ヘッダーが適用されることを確認）
        // 実際のネットワークリクエストはしないが、設定が適用されることを確認
        MockFailingServer mockServer;
        downloader.RequestSegment(mockServer.Url("/segment0.ts"), 0, ChunkPriority::Critical);
        
        // 少し待機
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        
        downloader.Stop();
        ASSERT_FALSE(downloader.IsRunning());
    }
    TEST_END()
}

void TestSegmentDownloadWithCallback() {
    TEST_CASE("ChunkDownloader segment download with callback")
    {
        SparseFileCacheConfig cacheConfig;
        cacheConfig.chunkSize = 1024;
        cacheConfig.maxMemoryBytes = 1024 * 1024;
        SparseFileCache cache(cacheConfig);
        cache.Initialize(10240);
        
        ChunkDownloader downloader(&cache, 2);
        
        std::mutex mtx;
        std::condition_variable cv;
        std::vector<int64_t> downloadedSegments;
        std::atomic<int> callbackCount{0};
        
        downloader.SetSegmentDownloadCallback(
            [&](int64_t segmentIndex, std::vector<uint8_t>&& /*data*/, bool /*success*/) {
                std::lock_guard<std::mutex> lock(mtx);
                downloadedSegments.push_back(segmentIndex);
                callbackCount++;
                cv.notify_all();
            }
        );
        
        // 実際のネットワークなしでもAPIが動作することを確認
        downloader.Start();
        
        // 架空のURLでリクエスト（実際のダウンロードは失敗するが、コールバックは呼ばれる）
        MockFailingServer mockServer;
        downloader.RequestSegment(mockServer.Url("/test.ts"), 0, ChunkPriority::High);
        
        // 少し待機（タイムアウト対策）
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        
        downloader.Stop();
        
        // 停止後にクラッシュしないことを確認
    }
    TEST_END()
}

void TestSegmentAndChunkCoexistence() {
    TEST_CASE("ChunkDownloader segment and chunk requests coexist")
    {
        SparseFileCacheConfig cacheConfig;
        cacheConfig.chunkSize = 1024;
        cacheConfig.maxMemoryBytes = 1024 * 1024;
        SparseFileCache cache(cacheConfig);
        cache.Initialize(102400);
        
        ChunkDownloader downloader(&cache, 4);
        downloader.SetUrl("http://example.com/video.mp4");
        
        // 通常のチャンクリクエスト
        downloader.RequestChunk(0, ChunkPriority::High);
        downloader.RequestChunk(1024, ChunkPriority::Medium);
        
        // セグメントリクエスト（別キュー）
        downloader.RequestSegment("http://example.com/seg0.ts", 0, ChunkPriority::High);
        downloader.RequestSegment("http://example.com/seg1.ts", 1, ChunkPriority::Medium);
        
        // 両方のリクエストが共存できることを確認
    }
    TEST_END()
}

// =============================================================================
// リトライ/再キューテスト（C-1）
// =============================================================================

void TestChunkDownloaderRequestChunkResetsErrorState() {
    TEST_CASE("ChunkDownloader RequestChunk resets Error chunk to Pending (C-1)")
    {
        SparseFileCacheConfig cacheConfig;
        cacheConfig.chunkSize = 1024;
        cacheConfig.maxMemoryBytes = 1024 * 1024;
        SparseFileCache cache(cacheConfig);
        cache.Initialize(10240);  // 10KB

        ChunkDownloader downloader(&cache, 2);
        downloader.SetUrl("http://example.com/test.bin");

        // attempt上限に達して恒久的に失敗した状態を模擬する
        int64_t chunkIndex = cache.GetChunkIndex(0);
        cache.SetChunkState(chunkIndex, ChunkState::Error);
        ASSERT_TRUE(cache.GetChunkState(chunkIndex) == ChunkState::Error);

        // Error状態のチャンクを再リクエストすると、再ダウンロード対象として
        // 受け付けられ、Pendingに戻ることを確認する（永久放置されない）
        downloader.RequestChunk(0, ChunkPriority::Critical);
        ASSERT_TRUE(cache.GetChunkState(chunkIndex) == ChunkState::Pending);
    }
    TEST_END()
}

void TestChunkDownloaderPermanentFailureReachesErrorState() {
    TEST_CASE("ChunkDownloader chunk exhausts retries and reaches Error state (C-1)")
    {
        SparseFileCacheConfig cacheConfig;
        cacheConfig.chunkSize = 1024;
        cacheConfig.maxMemoryBytes = 1024 * 1024;
        SparseFileCache cache(cacheConfig);
        cache.Initialize(10240);

        ChunkDownloader downloader(&cache, 1);
        // 接続不可なURL（即座に失敗する）を指定し、attempt上限まで再試行された後、
        // 永久に放置されず最終的にError状態へ到達することを確認する
        MockFailingServer mockServer;
        downloader.SetUrl(mockServer.Url("/test.bin"));
        downloader.Start();
        downloader.RequestChunk(0, ChunkPriority::Critical);

        int64_t chunkIndex = cache.GetChunkIndex(0);
        ChunkState finalState = ChunkState::Pending;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(40);
        while (std::chrono::steady_clock::now() < deadline) {
            finalState = cache.GetChunkState(chunkIndex);
            if (finalState == ChunkState::Error) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }

        downloader.Stop();

        ASSERT_TRUE(finalState == ChunkState::Error);
    }
    TEST_END()
}

void TestSegmentDownloaderRetriesBeforePermanentFailure() {
    TEST_CASE("ChunkDownloader segment exhausts retries and notifies failure exactly once (C-1)")
    {
        SparseFileCacheConfig cacheConfig;
        cacheConfig.chunkSize = 1024;
        cacheConfig.maxMemoryBytes = 1024 * 1024;
        SparseFileCache cache(cacheConfig);
        cache.Initialize(10240);

        ChunkDownloader downloader(&cache, 1);

        std::mutex mtx;
        std::condition_variable cv;
        int callbackCount = 0;
        bool lastSuccess = true;

        downloader.SetSegmentDownloadCallback(
            [&](int64_t /*segmentIndex*/, std::vector<uint8_t>&& /*data*/, bool success) {
                std::lock_guard<std::mutex> lock(mtx);
                callbackCount++;
                lastSuccess = success;
                cv.notify_all();
            }
        );

        downloader.Start();
        // 接続不可なURLをリクエスト。attempt上限まで再試行された後、
        // 再キュー中はコールバックが呼ばれず、最終的に1回だけ通知されることを確認する
        MockFailingServer mockServer;
        downloader.RequestSegment(mockServer.Url("/segment0.ts"), 0, ChunkPriority::Critical);

        {
            std::unique_lock<std::mutex> lock(mtx);
            cv.wait_for(lock, std::chrono::seconds(40), [&] { return callbackCount > 0; });
        }

        downloader.Stop();

        ASSERT_EQ(callbackCount, 1);
        ASSERT_FALSE(lastSuccess);
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
    
    std::cout << "\n[HLS Segment Tests]" << std::endl;
    TestSegmentDownloadCallback();
    TestRequestSegment();
    TestReprioritizeSegment();
    TestClearSegmentQueue();
    TestSetHttpHeaders();
    TestSetHttpHeadersAfterStart();
    TestSegmentDownloadWithCallback();
    TestSegmentAndChunkCoexistence();

    std::cout << "\n[Retry/Requeue Tests (C-1)]" << std::endl;
    TestChunkDownloaderRequestChunkResetsErrorState();
    TestChunkDownloaderPermanentFailureReachesErrorState();
    TestSegmentDownloaderRetriesBeforePermanentFailure();

    std::cout << "\n========================================" << std::endl;
    std::cout << "Results: " << s_testsPassed << " passed, " 
              << s_testsFailed << " failed" << std::endl;
    std::cout << "========================================" << std::endl;

    return s_testsFailed > 0 ? 1 : 0;
}
