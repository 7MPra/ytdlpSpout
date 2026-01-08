// =============================================================================
// test_custom_io_context.cpp - CustomIOContextのユニットテスト
// =============================================================================

#include <iostream>
#include <cassert>
#include <thread>
#include <chrono>
#include <atomic>
#include <vector>
#include <cstring>
#include "io/CustomIOContext.h"
#include "io/SparseFileCache.h"
#include "io/ChunkDownloader.h"
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

#define ASSERT_NULL(ptr) \
    if ((ptr) != nullptr) throw std::runtime_error("Assertion failed: " #ptr " != nullptr")

#define ASSERT_NOT_NULL(ptr) \
    if ((ptr) == nullptr) throw std::runtime_error("Assertion failed: " #ptr " == nullptr")

// =============================================================================
// 基本コンストラクタテスト
// =============================================================================

void TestConstruction() {
    TEST_CASE("CustomIOContext construction and destruction")
    {
        CustomIOContext context;
        ASSERT_FALSE(context.IsInitialized());
        ASSERT_NULL(context.GetAVIOContext());
        ASSERT_EQ(context.GetSize(), -1);
        ASSERT_EQ(context.GetPosition(), 0);
    }
    TEST_END()
}

void TestDefaultConfig() {
    TEST_CASE("CustomIOContext default configuration")
    {
        CustomIOContext context;
        ASSERT_FALSE(context.IsInitialized());
        ASSERT_EQ(context.GetSourceType(), IOSourceType::Unknown);
    }
    TEST_END()
}

// =============================================================================
// ソースタイプ検出テスト
// =============================================================================

void TestDetectSourceTypeHttp() {
    TEST_CASE("DetectSourceType with HTTP URL")
    {
        ASSERT_EQ(CustomIOContext::DetectSourceType("http://example.com/video.mp4"),
                  IOSourceType::HttpUrl);
    }
    TEST_END()
}

void TestDetectSourceTypeHttps() {
    TEST_CASE("DetectSourceType with HTTPS URL")
    {
        ASSERT_EQ(CustomIOContext::DetectSourceType("https://example.com/video.mp4"),
                  IOSourceType::HttpUrl);
    }
    TEST_END()
}

void TestDetectSourceTypeLocalFile() {
    TEST_CASE("DetectSourceType with local file")
    {
        ASSERT_EQ(CustomIOContext::DetectSourceType("C:\\videos\\test.mp4"),
                  IOSourceType::LocalFile);
        ASSERT_EQ(CustomIOContext::DetectSourceType("/home/user/video.mp4"),
                  IOSourceType::LocalFile);
        ASSERT_EQ(CustomIOContext::DetectSourceType("relative/path.mp4"),
                  IOSourceType::LocalFile);
    }
    TEST_END()
}

void TestDetectSourceTypeEmpty() {
    TEST_CASE("DetectSourceType with empty string")
    {
        ASSERT_EQ(CustomIOContext::DetectSourceType(""), IOSourceType::Unknown);
    }
    TEST_END()
}

// =============================================================================
// 初期化・終了テスト（ローカルファイル - モック不要）
// =============================================================================

void TestInitializeWithInvalidUrl() {
    TEST_CASE("Initialize with invalid/unreachable URL returns false")
    {
        CustomIOContext context;
        // 存在しないURLで初期化は失敗するはず
        bool result = context.Initialize("http://localhost:99999/nonexistent.mp4");
        // 接続失敗で false が返る
        ASSERT_FALSE(result);
        ASSERT_FALSE(context.IsInitialized());
    }
    TEST_END()
}

void TestCloseWithoutInitialize() {
    TEST_CASE("Close without Initialize should not crash")
    {
        CustomIOContext context;
        context.Close(); // クラッシュしなければOK
        ASSERT_FALSE(context.IsInitialized());
    }
    TEST_END()
}

void TestDoubleClose() {
    TEST_CASE("Double Close should not crash")
    {
        CustomIOContext context;
        context.Close();
        context.Close();
        ASSERT_FALSE(context.IsInitialized());
    }
    TEST_END()
}

// =============================================================================
// AVIOContext取得テスト
// =============================================================================

void TestGetAVIOContextBeforeInit() {
    TEST_CASE("GetAVIOContext before Initialize returns nullptr")
    {
        CustomIOContext context;
        ASSERT_NULL(context.GetAVIOContext());
    }
    TEST_END()
}

// =============================================================================
// URL保存テスト
// =============================================================================

void TestGetUrlBeforeInit() {
    TEST_CASE("GetUrl before Initialize returns empty string")
    {
        CustomIOContext context;
        ASSERT_TRUE(context.GetUrl().empty());
    }
    TEST_END()
}

// =============================================================================
// シーク可能フラグテスト
// =============================================================================

void TestIsSeekableBeforeInit() {
    TEST_CASE("IsSeekable before Initialize returns false")
    {
        CustomIOContext context;
        ASSERT_FALSE(context.IsSeekable());
    }
    TEST_END()
}

// =============================================================================
// 再生位置更新テスト
// =============================================================================

void TestUpdatePlaybackPositionBeforeInit() {
    TEST_CASE("UpdatePlaybackPosition before Initialize should not crash")
    {
        CustomIOContext context;
        context.UpdatePlaybackPosition(0);
        context.UpdatePlaybackPosition(1000);
        // クラッシュしなければOK
    }
    TEST_END()
}

void TestNotifySeekBeforeInit() {
    TEST_CASE("NotifySeek before Initialize should not crash")
    {
        CustomIOContext context;
        context.NotifySeek(0);
        context.NotifySeek(5000);
        // クラッシュしなければOK
    }
    TEST_END()
}

// =============================================================================
// 設定オプションテスト
// =============================================================================

void TestCustomConfig() {
    TEST_CASE("CustomIOContext with custom config")
    {
        CustomIOContext context;
        CustomIOContextConfig config;
        config.bufferSize = 64 * 1024;  // 64KB
        config.chunkSize = 2 * 1024 * 1024;  // 2MB
        config.maxCacheMemory = 128 * 1024 * 1024;  // 128MB
        config.maxConcurrentDownloads = 8;
        config.readTimeoutMs = 60000;
        
        // 不正なURLで初期化（設定自体は受け付けるはず）
        bool result = context.Initialize("http://invalid-url-12345.test/video.mp4", config);
        ASSERT_FALSE(result);  // 接続失敗
    }
    TEST_END()
}

// =============================================================================
// AVFormatContext統合テスト
// =============================================================================

void TestAttachToFormatContextBeforeInit() {
    TEST_CASE("AttachToFormatContext before Initialize returns false")
    {
        CustomIOContext context;
        // nullptrを渡してもfalseが返るはず
        ASSERT_FALSE(context.AttachToFormatContext(nullptr));
    }
    TEST_END()
}

// =============================================================================
// メイン関数
// =============================================================================

int main() {
    std::cout << "=====================================================" << std::endl;
    std::cout << "CustomIOContext Unit Tests" << std::endl;
    std::cout << "=====================================================" << std::endl;
    
    // ロガー初期化
    ytdlpspout::Logger::Initialize(false, "", ytdlpspout::LogLevel::Debug);
    
    // 基本テスト
    std::cout << "\n[Basic Construction Tests]" << std::endl;
    TestConstruction();
    TestDefaultConfig();
    
    // ソースタイプ検出テスト
    std::cout << "\n[Source Type Detection Tests]" << std::endl;
    TestDetectSourceTypeHttp();
    TestDetectSourceTypeHttps();
    TestDetectSourceTypeLocalFile();
    TestDetectSourceTypeEmpty();
    
    // 初期化・終了テスト
    std::cout << "\n[Initialization Tests]" << std::endl;
    TestInitializeWithInvalidUrl();
    TestCloseWithoutInitialize();
    TestDoubleClose();
    
    // AVIOContextテスト
    std::cout << "\n[AVIOContext Tests]" << std::endl;
    TestGetAVIOContextBeforeInit();
    
    // URL保存テスト
    std::cout << "\n[URL Storage Tests]" << std::endl;
    TestGetUrlBeforeInit();
    
    // シーク可能フラグテスト
    std::cout << "\n[Seekable Flag Tests]" << std::endl;
    TestIsSeekableBeforeInit();
    
    // 再生位置更新テスト
    std::cout << "\n[Playback Position Tests]" << std::endl;
    TestUpdatePlaybackPositionBeforeInit();
    TestNotifySeekBeforeInit();
    
    // 設定オプションテスト
    std::cout << "\n[Configuration Tests]" << std::endl;
    TestCustomConfig();
    
    // AVFormatContext統合テスト
    std::cout << "\n[AVFormatContext Integration Tests]" << std::endl;
    TestAttachToFormatContextBeforeInit();
    
    // 結果サマリ
    std::cout << "\n=====================================================" << std::endl;
    std::cout << "Test Results: " << s_testsPassed << " passed, " 
              << s_testsFailed << " failed" << std::endl;
    std::cout << "=====================================================" << std::endl;
    
    return s_testsFailed > 0 ? 1 : 0;
}
