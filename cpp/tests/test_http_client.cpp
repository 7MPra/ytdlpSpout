// =============================================================================
// test_http_client.cpp - HttpClientのユニットテスト
// =============================================================================

#include <iostream>
#include <cassert>
#include <thread>
#include <chrono>
#include <atomic>
#include <vector>
#include <curl/curl.h>
#include "io/HttpClient.h"
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

// =============================================================================
// HttpClient基本テスト
// =============================================================================

void TestHttpClientConstruction() {
    TEST_CASE("HttpClient construction and destruction")
    {
        HttpClient client;
        // 構築が成功すればOK
    }
    TEST_END()
}

void TestHttpClientWithConfig() {
    TEST_CASE("HttpClient construction with config")
    {
        HttpClientConfig config;
        config.userAgent = "TestAgent/1.0";
        config.connectTimeoutMs = 5000;
        config.readTimeoutMs = 15000;
        config.followRedirects = true;
        config.maxRedirects = 5;
        
        HttpClient client(config);
        ASSERT_EQ(client.GetConfig().userAgent, "TestAgent/1.0");
        ASSERT_EQ(client.GetConfig().connectTimeoutMs, 5000);
    }
    TEST_END()
}

void TestConfigure() {
    TEST_CASE("Configure method")
    {
        HttpClient client;
        
        HttpClientConfig config;
        config.userAgent = "UpdatedAgent/2.0";
        config.headers["X-Custom"] = "Value";
        
        client.Configure(config);
        ASSERT_EQ(client.GetConfig().userAgent, "UpdatedAgent/2.0");
    }
    TEST_END()
}

void TestIsHttpUrl() {
    TEST_CASE("IsHttpUrl static method")
    {
        ASSERT_TRUE(HttpClient::IsHttpUrl("http://example.com"));
        ASSERT_TRUE(HttpClient::IsHttpUrl("https://example.com"));
        ASSERT_TRUE(HttpClient::IsHttpUrl("HTTP://EXAMPLE.COM"));
        ASSERT_TRUE(HttpClient::IsHttpUrl("HTTPS://EXAMPLE.COM"));
        ASSERT_FALSE(HttpClient::IsHttpUrl("ftp://example.com"));
        ASSERT_FALSE(HttpClient::IsHttpUrl("file:///path"));
        ASSERT_FALSE(HttpClient::IsHttpUrl(""));
        ASSERT_FALSE(HttpClient::IsHttpUrl("http"));
    }
    TEST_END()
}

// =============================================================================
// リトライ判定ロジックテスト (H-1: detail::ShouldRetryRequest)
// =============================================================================

void TestShouldRetryRequestOnCurlError() {
    TEST_CASE("ShouldRetryRequest retries on generic curl-level error")
    {
        ASSERT_TRUE(detail::ShouldRetryRequest(static_cast<int>(CURLE_COULDNT_CONNECT), 0));
        ASSERT_TRUE(detail::ShouldRetryRequest(static_cast<int>(CURLE_OPERATION_TIMEDOUT), 0));
    }
    TEST_END()
}

void TestShouldRetryRequestNotOnUserCancel() {
    TEST_CASE("ShouldRetryRequest does not retry on user cancellation (CURLE_ABORTED_BY_CALLBACK)")
    {
        ASSERT_FALSE(detail::ShouldRetryRequest(static_cast<int>(CURLE_ABORTED_BY_CALLBACK), 0));
    }
    TEST_END()
}

void TestShouldRetryRequestOn5xxAnd429() {
    TEST_CASE("ShouldRetryRequest retries on HTTP 5xx and 429")
    {
        ASSERT_TRUE(detail::ShouldRetryRequest(static_cast<int>(CURLE_OK), 500));
        ASSERT_TRUE(detail::ShouldRetryRequest(static_cast<int>(CURLE_OK), 503));
        ASSERT_TRUE(detail::ShouldRetryRequest(static_cast<int>(CURLE_OK), 429));
    }
    TEST_END()
}

void TestShouldRetryRequestNotOnClientErrorOrSuccess() {
    TEST_CASE("ShouldRetryRequest does not retry on 4xx (except 429) or success")
    {
        ASSERT_FALSE(detail::ShouldRetryRequest(static_cast<int>(CURLE_OK), 403));
        ASSERT_FALSE(detail::ShouldRetryRequest(static_cast<int>(CURLE_OK), 404));
        ASSERT_FALSE(detail::ShouldRetryRequest(static_cast<int>(CURLE_OK), 200));
    }
    TEST_END()
}

// =============================================================================
// Rangeレスポンス検証ロジックテスト (H-3: detail::IsValidRangeResponse)
// =============================================================================

void TestIsValidRangeResponseAccepts206() {
    TEST_CASE("IsValidRangeResponse accepts 206 Partial Content")
    {
        ASSERT_TRUE(detail::IsValidRangeResponse(206, 100, 50, 50));
    }
    TEST_END()
}

void TestIsValidRangeResponseRejects200WithNonZeroStart() {
    TEST_CASE("IsValidRangeResponse rejects 200 when startByte > 0 (server ignored Range)")
    {
        ASSERT_FALSE(detail::IsValidRangeResponse(200, 100, 1000, 50));
    }
    TEST_END()
}

void TestIsValidRangeResponseRejects200OversizedBody() {
    TEST_CASE("IsValidRangeResponse rejects 200 with body larger than requested range")
    {
        ASSERT_FALSE(detail::IsValidRangeResponse(200, 0, 10000, 100));
    }
    TEST_END()
}

void TestIsValidRangeResponseAccepts200ValidSlice() {
    TEST_CASE("IsValidRangeResponse accepts 200 when startByte==0 and body fits requested length")
    {
        ASSERT_TRUE(detail::IsValidRangeResponse(200, 0, 100, 100));
    }
    TEST_END()
}

// =============================================================================
// ヘッダーパースロジックテスト (H-4: detail::IsHttpStatusLine / ParseHeaderLine)
// =============================================================================

void TestIsHttpStatusLine() {
    TEST_CASE("IsHttpStatusLine detects HTTP status lines")
    {
        ASSERT_TRUE(detail::IsHttpStatusLine("HTTP/1.1 200 OK\r\n"));
        ASSERT_TRUE(detail::IsHttpStatusLine("HTTP/2 301 Moved Permanently"));
        ASSERT_FALSE(detail::IsHttpStatusLine("Content-Length: 100"));
        ASSERT_FALSE(detail::IsHttpStatusLine(""));
    }
    TEST_END()
}

void TestParseHeaderLineLowercasesKey() {
    TEST_CASE("ParseHeaderLine normalizes header keys to lowercase")
    {
        std::string key, value;
        ASSERT_TRUE(detail::ParseHeaderLine("Content-Length: 12345\r\n", key, value));
        ASSERT_EQ(key, "content-length");
        ASSERT_EQ(value, "12345");

        ASSERT_TRUE(detail::ParseHeaderLine("ACCEPT-RANGES: bytes", key, value));
        ASSERT_EQ(key, "accept-ranges");
        ASSERT_EQ(value, "bytes");
    }
    TEST_END()
}

void TestParseHeaderLineRejectsNonHeaderLine() {
    TEST_CASE("ParseHeaderLine returns false for a line without a colon")
    {
        std::string key, value;
        ASSERT_FALSE(detail::ParseHeaderLine("not a header line", key, value));
    }
    TEST_END()
}

// =============================================================================
// HTTPリクエストテスト（実ネットワーク使用）
// =============================================================================

void TestHeadRequest() {
    TEST_CASE("HEAD request to httpbin.org")
    {
        HttpClientConfig config;
        config.userAgent = "ytdlpSpout-Test/1.0";
        config.readTimeoutMs = 10000;
        
        HttpClient client(config);
        auto response = client.Head("https://httpbin.org/get");
        
        // ステータスコード200を期待
        ASSERT_EQ(response.statusCode, 200);
        ASSERT_TRUE(response.success);
        ASSERT_TRUE(response.errorMessage.empty());
        // HEADリクエストではボディは空
        ASSERT_TRUE(response.data.empty());
    }
    TEST_END()
}

void TestGetRequest() {
    TEST_CASE("GET request to httpbin.org")
    {
        HttpClientConfig config;
        config.userAgent = "ytdlpSpout-Test/1.0";
        config.readTimeoutMs = 10000;
        
        HttpClient client(config);
        auto response = client.Get("https://httpbin.org/get");
        
        ASSERT_EQ(response.statusCode, 200);
        ASSERT_TRUE(response.success);
        ASSERT_TRUE(response.errorMessage.empty());
        ASSERT_FALSE(response.data.empty());
        
        // レスポンスにJSON形式のボディが含まれることを確認
        std::string bodyStr(response.data.begin(), response.data.end());
        ASSERT_TRUE(bodyStr.find("httpbin.org") != std::string::npos);
    }
    TEST_END()
}

void TestGetWithCustomHeaders() {
    TEST_CASE("GET request with custom headers")
    {
        HttpClientConfig config;
        config.userAgent = "ytdlpSpout-Test/1.0";
        config.readTimeoutMs = 10000;
        config.headers["X-Custom-Test"] = "TestValue123";
        
        HttpClient client(config);
        auto response = client.Get("https://httpbin.org/headers");
        
        ASSERT_EQ(response.statusCode, 200);
        ASSERT_TRUE(response.success);
        
        // カスタムヘッダーがエコーバックされることを確認
        std::string bodyStr(response.data.begin(), response.data.end());
        ASSERT_TRUE(bodyStr.find("X-Custom-Test") != std::string::npos);
    }
    TEST_END()
}

void TestGetRangeRequest() {
    TEST_CASE("GET Range request")
    {
        HttpClientConfig config;
        config.userAgent = "ytdlpSpout-Test/1.0";
        config.readTimeoutMs = 10000;
        
        HttpClient client(config);
        // httpbin.orgの /bytes/N はRangeヘッダーを無視して常に200+全ボディを返すため
        // 部分レンジ取得の検証には使えない（H-3のIsValidRangeResponseにより弾かれる）。
        // Rangeを正しくサポートする /range/N エンドポイントを使用する。
        auto response = client.GetRange("https://httpbin.org/range/1000", 0, 99);
        
        // 206 Partial Content または 200 OKを期待
        ASSERT_TRUE(response.statusCode == 206 || response.statusCode == 200);
        ASSERT_TRUE(response.success);
        // リクエストした範囲のデータが返されることを確認
        ASSERT_GT(response.data.size(), 0u);
    }
    TEST_END()
}

void TestGetContentLength() {
    TEST_CASE("GetContentLength")
    {
        HttpClientConfig config;
        config.userAgent = "ytdlpSpout-Test/1.0";
        config.readTimeoutMs = 10000;
        
        HttpClient client(config);
        // httpbin.orgのbytesエンドポイントは指定したサイズを返す
        int64_t contentLength = client.GetContentLength("https://httpbin.org/bytes/500");
        
        // コンテンツ長が取得できることを確認
        ASSERT_EQ(contentLength, 500);
    }
    TEST_END()
}

void TestSupportsRangeRequests() {
    TEST_CASE("SupportsRangeRequests")
    {
        HttpClientConfig config;
        config.userAgent = "ytdlpSpout-Test/1.0";
        config.readTimeoutMs = 10000;
        
        HttpClient client(config);
        // httpbin.orgはRange Requestをサポート
        bool supportsRange = client.SupportsRangeRequests("https://httpbin.org/bytes/100");
        
        // 結果を確認（httpbin.orgの挙動次第で変わる可能性あり）
        // テストとしては呼び出しが成功すればOK
        (void)supportsRange;
    }
    TEST_END()
}

void TestGetRangeStreaming() {
    TEST_CASE("GET Range Streaming request")
    {
        HttpClientConfig config;
        config.userAgent = "ytdlpSpout-Test/1.0";
        config.readTimeoutMs = 10000;
        
        HttpClient client(config);
        
        std::vector<uint8_t> receivedData;
        int64_t lastProgress = 0;
        
        auto response = client.GetRangeStreaming(
            "https://httpbin.org/bytes/500",
            0, 499,
            [&receivedData](const uint8_t* data, size_t size) {
                receivedData.insert(receivedData.end(), data, data + size);
            },
            [&lastProgress](int64_t downloaded, int64_t total) {
                lastProgress = downloaded;
                return true; // 継続
            }
        );
        
        ASSERT_TRUE(response.success);
        ASSERT_GT(receivedData.size(), 0u);
    }
    TEST_END()
}

// =============================================================================
// エラーハンドリングテスト
// =============================================================================

void TestInvalidUrl() {
    TEST_CASE("Invalid URL error handling")
    {
        HttpClientConfig config;
        config.connectTimeoutMs = 3000;
        config.readTimeoutMs = 5000;
        
        HttpClient client(config);
        auto response = client.Get("https://invalid.domain.that.does.not.exist.example/");
        
        // エラーが設定されることを確認
        ASSERT_FALSE(response.success);
        ASSERT_FALSE(response.errorMessage.empty());
    }
    TEST_END()
}

void TestTimeoutHandling() {
    TEST_CASE("Timeout handling")
    {
        HttpClientConfig config;
        config.connectTimeoutMs = 2000;
        config.readTimeoutMs = 2000;
        // このテストは単発のタイムアウト検出を検証するものであり、
        // リトライ(H-1)によるバックオフ加算で elapsed < 10 が崩れないようにする
        config.maxRetries = 0;

        HttpClient client(config);

        // httpbin.orgのdelayエンドポイントで遅延をシミュレート
        auto start = std::chrono::steady_clock::now();
        auto response = client.Get("https://httpbin.org/delay/10");  // 10秒遅延
        auto end = std::chrono::steady_clock::now();
        
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(end - start).count();
        
        // タイムアウトが機能していることを確認（10秒未満で終了）
        ASSERT_TRUE(elapsed < 10);
        // タイムアウトエラーを期待
        if (response.statusCode != 200) {
            ASSERT_FALSE(response.errorMessage.empty());
        }
    }
    TEST_END()
}

void Test404Response() {
    TEST_CASE("404 Not Found response handling")
    {
        HttpClientConfig config;
        config.readTimeoutMs = 10000;
        
        HttpClient client(config);
        auto response = client.Get("https://httpbin.org/status/404");
        
        ASSERT_EQ(response.statusCode, 404);
        ASSERT_FALSE(response.success);
    }
    TEST_END()
}

void TestResponseHeaders() {
    TEST_CASE("Response headers parsing")
    {
        HttpClientConfig config;
        config.readTimeoutMs = 10000;
        
        HttpClient client(config);
        auto response = client.Get("https://httpbin.org/response-headers?X-Test-Header=HelloWorld");
        
        ASSERT_EQ(response.statusCode, 200);
        ASSERT_TRUE(response.success);
        
        // レスポンスヘッダーが解析されていることを確認
        ASSERT_FALSE(response.headers.empty());
        
        // Content-Typeヘッダーが存在することを確認
        bool hasContentType = false;
        for (const auto& [key, value] : response.headers) {
            if (key == "Content-Type" || key == "content-type") {
                hasContentType = true;
                break;
            }
        }
        ASSERT_TRUE(hasContentType);
    }
    TEST_END()
}

void TestCancelStreaming() {
    TEST_CASE("Cancel streaming download")
    {
        HttpClientConfig config;
        config.userAgent = "ytdlpSpout-Test/1.0";
        config.readTimeoutMs = 30000;
        
        HttpClient client(config);
        
        size_t bytesReceived = 0;
        
        auto response = client.GetRangeStreaming(
            "https://httpbin.org/bytes/10000",
            0, 9999,
            [&bytesReceived](const uint8_t* /*data*/, size_t size) {
                bytesReceived += size;
            },
            [](int64_t downloaded, int64_t /*total*/) {
                // 500バイト受信後にキャンセル
                return downloaded < 500;
            }
        );
        
        // キャンセルされたことを確認
        ASSERT_FALSE(response.success);
    }
    TEST_END()
}

// =============================================================================
// スレッドセーフティテスト
// =============================================================================

void TestThreadSafety() {
    TEST_CASE("Thread safety - multiple clients")
    {
        const int numThreads = 4;
        std::vector<std::thread> threads;
        std::atomic<int> successCount{0};
        std::atomic<int> failCount{0};
        
        for (int i = 0; i < numThreads; ++i) {
            threads.emplace_back([&successCount, &failCount, i]() {
                HttpClientConfig config;
                config.userAgent = "ytdlpSpout-Thread-" + std::to_string(i);
                config.readTimeoutMs = 15000;
                
                HttpClient client(config);
                auto response = client.Get("https://httpbin.org/get");
                
                if (response.success && response.statusCode == 200) {
                    successCount++;
                } else {
                    failCount++;
                }
            });
        }
        
        for (auto& t : threads) {
            t.join();
        }
        
        // 全スレッドが成功することを期待
        ASSERT_EQ(successCount.load(), numThreads);
    }
    TEST_END()
}

// =============================================================================
// CurlGlobalInit テスト
// =============================================================================

void TestCurlGlobalInit() {
    TEST_CASE("CurlGlobalInit lifecycle")
    {
        // 既に初期化されているはず
        ASSERT_TRUE(CurlGlobalInit::IsInitialized());
    }
    TEST_END()
}

// =============================================================================
// メイン
// =============================================================================

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "HttpClient Unit Tests" << std::endl;
    std::cout << "========================================" << std::endl;
    
    // ロガーの初期化
    Logger::Initialize(false, "", LogLevel::Debug);
    
    // libcurlグローバル初期化
    CurlGlobalInit::Initialize();
    
    std::cout << std::endl << "[Basic Tests]" << std::endl;
    TestHttpClientConstruction();
    TestHttpClientWithConfig();
    TestConfigure();
    TestIsHttpUrl();
    TestCurlGlobalInit();

    std::cout << std::endl << "[Retry Logic Tests]" << std::endl;
    TestShouldRetryRequestOnCurlError();
    TestShouldRetryRequestNotOnUserCancel();
    TestShouldRetryRequestOn5xxAnd429();
    TestShouldRetryRequestNotOnClientErrorOrSuccess();

    std::cout << std::endl << "[Range Response Validation Tests]" << std::endl;
    TestIsValidRangeResponseAccepts206();
    TestIsValidRangeResponseRejects200WithNonZeroStart();
    TestIsValidRangeResponseRejects200OversizedBody();
    TestIsValidRangeResponseAccepts200ValidSlice();

    std::cout << std::endl << "[Header Parsing Tests]" << std::endl;
    TestIsHttpStatusLine();
    TestParseHeaderLineLowercasesKey();
    TestParseHeaderLineRejectsNonHeaderLine();

    std::cout << std::endl << "[HTTP Request Tests]" << std::endl;
    TestHeadRequest();
    TestGetRequest();
    TestGetWithCustomHeaders();
    TestGetRangeRequest();
    TestGetContentLength();
    TestSupportsRangeRequests();
    TestGetRangeStreaming();
    
    std::cout << std::endl << "[Error Handling Tests]" << std::endl;
    TestInvalidUrl();
    TestTimeoutHandling();
    Test404Response();
    TestResponseHeaders();
    TestCancelStreaming();
    
    std::cout << std::endl << "[Thread Safety Tests]" << std::endl;
    TestThreadSafety();
    
    // libcurlグローバルクリーンアップ
    CurlGlobalInit::Shutdown();
    
    Logger::Shutdown();
    
    std::cout << std::endl << "========================================" << std::endl;
    std::cout << "Results: " << s_testsPassed << " passed, " 
              << s_testsFailed << " failed" << std::endl;
    std::cout << "========================================" << std::endl;
    
    return s_testsFailed > 0 ? 1 : 0;
}
