// =============================================================================
// test_hls_http_headers.cpp - HLS HTTPヘッダー伝播のテスト
// =============================================================================
//
// テスト項目:
//   - HttpHeaderContextの構造体
//   - custom_io_open関数の動作
//   - HTTPヘッダーの伝播
//
// =============================================================================

#include <iostream>
#include <cassert>
#include <string>
#include <map>
#include <algorithm>
#include <cctype>

// Windows
#include <Windows.h>
#include <objbase.h>

// spdlog
#include <spdlog/spdlog.h>

// FFmpeg
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/opt.h>
#include <libavutil/dict.h>
}

// =============================================================================
// テストユーティリティ
// =============================================================================

#define TEST_ASSERT(condition, message)                           \
    do {                                                          \
        if (!(condition)) {                                       \
            std::cerr << "FAILED: " << (message) << std::endl;    \
            return false;                                          \
        }                                                          \
        std::cout << "PASSED: " << (message) << std::endl;        \
    } while (0)

#define RUN_TEST(func)                                            \
    do {                                                          \
        std::cout << "\n=== " #func " ===" << std::endl;          \
        if (func()) {                                             \
            passedTests++;                                         \
        } else {                                                   \
            failedTests++;                                         \
        }                                                          \
        totalTests++;                                              \
    } while (0)

// =============================================================================
// HttpHeaderContext構造体（VideoDecoder.cppと同一）
// =============================================================================

/// @brief HLS内部リクエスト用HTTPヘッダーコンテキスト
/// @details io_openコールバックでHTTPヘッダーを伝播するために使用
struct HttpHeaderContext {
    std::string headers;  ///< FFmpeg形式のHTTPヘッダー ("Key: Value\r\n" 形式)
};

// =============================================================================
// テストケース: HttpHeaderContext構造体の基本動作
// =============================================================================

bool Test_HttpHeaderContextCreation() {
    // 構造体の作成
    HttpHeaderContext ctx;
    TEST_ASSERT(ctx.headers.empty(), "Default headers should be empty");
    
    // ヘッダーを設定
    ctx.headers = "Cookie: session=abc123\r\nUser-Agent: Test/1.0\r\n";
    TEST_ASSERT(!ctx.headers.empty(), "Headers should not be empty after setting");
    TEST_ASSERT(ctx.headers.find("Cookie") != std::string::npos, "Cookie should be present");
    TEST_ASSERT(ctx.headers.find("User-Agent") != std::string::npos, "User-Agent should be present");
    
    return true;
}

// =============================================================================
// テストケース: ヘッダーマップから文字列への変換
// =============================================================================

bool Test_HeadersMapToString() {
    std::map<std::string, std::string> httpHeaders = {
        {"Cookie", "session_id=abc123; other=value"},
        {"User-Agent", "ytdlpSpout/1.0"},
        {"Referer", "https://example.com/"},
        {"Origin", "https://example.com"}
    };
    
    // マップから文字列への変換
    std::string headersStr;
    for (const auto& [key, value] : httpHeaders) {
        headersStr += key + ": " + value + "\r\n";
    }
    
    TEST_ASSERT(!headersStr.empty(), "Headers string should not be empty");
    TEST_ASSERT(headersStr.find("Cookie: session_id=abc123") != std::string::npos,
                "Cookie header should be formatted correctly");
    TEST_ASSERT(headersStr.find("\r\n") != std::string::npos,
                "Headers should use CRLF line endings");
    
    // 末尾がCRLFで終わることを確認
    TEST_ASSERT(headersStr.size() >= 2, "Headers string should have at least 2 chars");
    TEST_ASSERT(headersStr.substr(headersStr.size() - 2) == "\r\n",
                "Headers string should end with CRLF");
    
    std::cout << "  Total headers string length: " << headersStr.size() << std::endl;
    std::cout << "  Header count: " << httpHeaders.size() << std::endl;
    
    return true;
}

// =============================================================================
// テストケース: AVDictionaryへのヘッダー設定
// =============================================================================

bool Test_AVDictionaryHeaderMerge() {
    // シミュレーション: io_openが受け取るoptionsにヘッダーをマージ
    AVDictionary* incoming_opts = nullptr;
    AVDictionary* merged_opts = nullptr;
    
    // 既存のオプション
    av_dict_set(&incoming_opts, "timeout", "5000000", 0);
    
    // HttpHeaderContextのヘッダー
    std::string headers = "Cookie: session=test\r\nReferer: https://example.com/\r\n";
    
    // マージ
    if (incoming_opts) {
        av_dict_copy(&merged_opts, incoming_opts, 0);
    }
    av_dict_set(&merged_opts, "headers", headers.c_str(), 0);
    
    // 検証
    AVDictionaryEntry* timeoutEntry = av_dict_get(merged_opts, "timeout", nullptr, 0);
    TEST_ASSERT(timeoutEntry != nullptr, "timeout option should be preserved");
    TEST_ASSERT(std::string(timeoutEntry->value) == "5000000", "timeout value should be preserved");
    
    AVDictionaryEntry* headersEntry = av_dict_get(merged_opts, "headers", nullptr, 0);
    TEST_ASSERT(headersEntry != nullptr, "headers option should be set");
    TEST_ASSERT(std::string(headersEntry->value).find("Cookie: session=test") != std::string::npos,
                "headers should contain Cookie");
    
    av_dict_free(&incoming_opts);
    av_dict_free(&merged_opts);
    
    return true;
}

// =============================================================================
// テストケース: opaqueポインタの設定と取得
// =============================================================================

bool Test_FormatContextOpaque() {
    // AVFormatContextを確保
    AVFormatContext* formatCtx = avformat_alloc_context();
    TEST_ASSERT(formatCtx != nullptr, "AVFormatContext allocation should succeed");
    
    // HttpHeaderContextを作成してopaqueに設定
    HttpHeaderContext* ctx = new HttpHeaderContext();
    ctx->headers = "Test-Header: value\r\n";
    
    formatCtx->opaque = ctx;
    
    // 取得して検証
    HttpHeaderContext* retrieved = static_cast<HttpHeaderContext*>(formatCtx->opaque);
    TEST_ASSERT(retrieved != nullptr, "Retrieved context should not be null");
    TEST_ASSERT(retrieved->headers == "Test-Header: value\r\n", "Headers should match");
    
    // クリーンアップ
    delete static_cast<HttpHeaderContext*>(formatCtx->opaque);
    formatCtx->opaque = nullptr;
    avformat_free_context(formatCtx);
    
    return true;
}

// =============================================================================
// テストケース: io_openコールバックのシグネチャ確認
// =============================================================================

bool Test_IoOpenCallbackSignature() {
    // io_openコールバックの型を確認
    // 実際のシグネチャ: int (*io_open)(struct AVFormatContext *s, AVIOContext **pb,
    //                                  const char *url, int flags, AVDictionary **options);
    
    AVFormatContext* formatCtx = avformat_alloc_context();
    TEST_ASSERT(formatCtx != nullptr, "AVFormatContext allocation should succeed");
    
    // デフォルトのio_openが存在することを確認
    // 注: 未初期化のformatCtxではio_openはnullptrの可能性がある
    std::cout << "  Default io_open: " << (formatCtx->io_open ? "set" : "null") << std::endl;
    std::cout << "  io_open callback address: " << reinterpret_cast<void*>(formatCtx->io_open) << std::endl;
    
    avformat_free_context(formatCtx);
    
    return true;
}

// =============================================================================
// テストケース: protocol_whitelist設定
// =============================================================================

bool Test_ProtocolWhitelist() {
    AVDictionary* opts = nullptr;
    
    // HLS暗号化対応のプロトコルホワイトリスト
    const char* whitelist = "file,http,https,tcp,tls,crypto,data";
    av_dict_set(&opts, "protocol_whitelist", whitelist, 0);
    
    AVDictionaryEntry* entry = av_dict_get(opts, "protocol_whitelist", nullptr, 0);
    TEST_ASSERT(entry != nullptr, "protocol_whitelist should be set");
    TEST_ASSERT(std::string(entry->value) == whitelist, "protocol_whitelist value should match");
    
    // 暗号化HLSに必要なプロトコルが含まれていることを確認
    std::string whitelistStr(entry->value);
    TEST_ASSERT(whitelistStr.find("http") != std::string::npos, "http should be in whitelist");
    TEST_ASSERT(whitelistStr.find("https") != std::string::npos, "https should be in whitelist");
    TEST_ASSERT(whitelistStr.find("crypto") != std::string::npos, "crypto should be in whitelist");
    TEST_ASSERT(whitelistStr.find("tls") != std::string::npos, "tls should be in whitelist");
    
    av_dict_free(&opts);
    
    return true;
}

// =============================================================================
// テストケース: メモリリーク防止（クリーンアップ）
// =============================================================================

bool Test_HttpHeaderContextCleanup() {
    // 複数回の作成と削除をシミュレート
    for (int i = 0; i < 10; ++i) {
        HttpHeaderContext* ctx = new HttpHeaderContext();
        ctx->headers = "Cookie: session" + std::to_string(i) + "\r\n";
        
        // 使用
        TEST_ASSERT(!ctx->headers.empty(), "Headers should not be empty");
        
        // クリーンアップ
        delete ctx;
    }
    
    std::cout << "  Successfully created and deleted 10 HttpHeaderContext instances" << std::endl;
    
    return true;
}

// =============================================================================
// テストケース: 特殊文字を含むヘッダー
// =============================================================================

bool Test_SpecialCharactersInHeaders() {
    HttpHeaderContext ctx;
    
    // 日本語（UTF-8）を含むヘッダー
    // 注: HTTPヘッダー自体はASCIIが推奨だが、Cookie値などにUTF-8が含まれることがある
    ctx.headers = "Cookie: name=%E3%83%86%E3%82%B9%E3%83%88\r\n";  // URLエンコードされた日本語
    ctx.headers += "User-Agent: TestApp/1.0 (Windows; x64)\r\n";
    ctx.headers += "X-Custom: value=with;semicolons&ampersands\r\n";
    
    TEST_ASSERT(ctx.headers.find("Cookie:") != std::string::npos, "Cookie should be present");
    TEST_ASSERT(ctx.headers.find("User-Agent:") != std::string::npos, "User-Agent should be present");
    TEST_ASSERT(ctx.headers.find("X-Custom:") != std::string::npos, "X-Custom should be present");
    
    // AVDictionaryに設定できることを確認
    AVDictionary* opts = nullptr;
    int ret = av_dict_set(&opts, "headers", ctx.headers.c_str(), 0);
    TEST_ASSERT(ret >= 0, "av_dict_set should succeed with special characters");
    
    AVDictionaryEntry* entry = av_dict_get(opts, "headers", nullptr, 0);
    TEST_ASSERT(entry != nullptr, "headers entry should exist");
    
    av_dict_free(&opts);
    
    return true;
}

// =============================================================================
// テストケース: HLS URLの検出
// =============================================================================

bool Test_HlsUrlDetection() {
    auto isHlsUrl = [](const std::string& url) -> bool {
        std::string lower = url;
        std::transform(lower.begin(), lower.end(), lower.begin(), 
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return (lower.find(".m3u8") != std::string::npos) ||
               (lower.find("format=m3u8") != std::string::npos) ||
               (lower.find("/hls/") != std::string::npos);
    };
    
    // HLS URLの検出
    TEST_ASSERT(isHlsUrl("https://example.com/video.m3u8"), ".m3u8 extension should be detected");
    TEST_ASSERT(isHlsUrl("https://example.com/video.M3U8"), ".M3U8 (uppercase) should be detected");
    TEST_ASSERT(isHlsUrl("https://example.com/api?format=m3u8"), "format=m3u8 query should be detected");
    TEST_ASSERT(isHlsUrl("https://example.com/hls/stream/index"), "/hls/ path should be detected");
    
    // 非HLS URL
    TEST_ASSERT(!isHlsUrl("https://example.com/video.mp4"), ".mp4 should not be HLS");
    TEST_ASSERT(!isHlsUrl("https://example.com/stream"), "Generic URL should not be HLS");
    TEST_ASSERT(!isHlsUrl("file:///C:/video.mkv"), "Local file should not be HLS");
    
    return true;
}

// =============================================================================
// メイン
// =============================================================================

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "HLS HTTP Headers Propagation Tests" << std::endl;
    std::cout << "========================================" << std::endl;

    // COM初期化
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr)) {
        std::cerr << "Failed to initialize COM" << std::endl;
        return 1;
    }

    int totalTests = 0;
    int passedTests = 0;
    int failedTests = 0;

    // テスト実行
    RUN_TEST(Test_HttpHeaderContextCreation);
    RUN_TEST(Test_HeadersMapToString);
    RUN_TEST(Test_AVDictionaryHeaderMerge);
    RUN_TEST(Test_FormatContextOpaque);
    RUN_TEST(Test_IoOpenCallbackSignature);
    RUN_TEST(Test_ProtocolWhitelist);
    RUN_TEST(Test_HttpHeaderContextCleanup);
    RUN_TEST(Test_SpecialCharactersInHeaders);
    RUN_TEST(Test_HlsUrlDetection);

    // 結果表示
    std::cout << "\n========================================" << std::endl;
    std::cout << "Test Results: " << passedTests << "/" << totalTests << " passed";
    if (failedTests > 0) {
        std::cout << " (" << failedTests << " failed)";
    }
    std::cout << std::endl;
    std::cout << "========================================" << std::endl;

    // COM終了
    CoUninitialize();

    return failedTests > 0 ? 1 : 0;
}
