// =============================================================================
// HttpClient.cpp - HTTP通信クライアント (libcurlラッパー) 実装
// =============================================================================

#include "io/HttpClient.h"
#include "utils/Logger.h"

#include <curl/curl.h>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <mutex>

namespace ytdlpspout {
namespace io {

// =============================================================================
// グローバル初期化
// =============================================================================

bool CurlGlobalInit::s_initialized = false;

void CurlGlobalInit::Initialize() {
    static std::once_flag s_initFlag;
    std::call_once(s_initFlag, []() {
        CURLcode result = curl_global_init(CURL_GLOBAL_DEFAULT);
        if (result == CURLE_OK) {
            s_initialized = true;
            LOG_INFO("libcurl initialized: {}", curl_version());
        } else {
            LOG_ERROR("Failed to initialize libcurl: {}", curl_easy_strerror(result));
        }
    });
}

void CurlGlobalInit::Shutdown() {
    if (s_initialized) {
        curl_global_cleanup();
        s_initialized = false;
        LOG_INFO("libcurl shutdown");
    }
}

bool CurlGlobalInit::IsInitialized() {
    return s_initialized;
}

// =============================================================================
// CURLコールバック用データ構造体（クラス外でアクセス可能）
// =============================================================================

struct CurlCallbackData {
    // レスポンスバッファ
    std::vector<uint8_t> responseData;
    std::map<std::string, std::string> responseHeaders;
    
    // ストリーミングコールバック
    DataCallback streamCallback;
    ProgressCallback progressCallback;
    int64_t downloadedBytes = 0;
    int64_t totalBytes = 0;
    bool cancelled = false;
    
    void Reset() {
        responseData.clear();
        responseHeaders.clear();
        streamCallback = nullptr;
        progressCallback = nullptr;
        downloadedBytes = 0;
        totalBytes = 0;
        cancelled = false;
    }
};

// =============================================================================
// CURL コールバック関数
// =============================================================================

static size_t WriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t realSize = size * nmemb;
    auto* data = static_cast<CurlCallbackData*>(userp);
    
    if (data->cancelled) {
        return 0; // ダウンロード中止
    }
    
    if (data->streamCallback) {
        // ストリーミングモード
        data->streamCallback(static_cast<const uint8_t*>(contents), realSize);
    } else {
        // バッファモード
        const uint8_t* bytes = static_cast<const uint8_t*>(contents);
        data->responseData.insert(data->responseData.end(), bytes, bytes + realSize);
    }
    
    return realSize;
}

static size_t HeaderCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t realSize = size * nmemb;
    auto* data = static_cast<CurlCallbackData*>(userp);
    
    std::string headerLine(static_cast<char*>(contents), realSize);
    
    // 改行を削除
    while (!headerLine.empty() && (headerLine.back() == '\r' || headerLine.back() == '\n')) {
        headerLine.pop_back();
    }
    
    // "Key: Value" 形式をパース
    size_t colonPos = headerLine.find(':');
    if (colonPos != std::string::npos) {
        std::string key = headerLine.substr(0, colonPos);
        std::string value = headerLine.substr(colonPos + 1);
        
        // 前後の空白を削除
        while (!value.empty() && value.front() == ' ') {
            value.erase(0, 1);
        }
        while (!value.empty() && value.back() == ' ') {
            value.pop_back();
        }
        
        data->responseHeaders[key] = value;
    }
    
    return realSize;
}

static int ProgressCallbackFunc(void* userp, curl_off_t dltotal, curl_off_t dlnow,
                                 curl_off_t /*ultotal*/, curl_off_t /*ulnow*/) {
    auto* data = static_cast<CurlCallbackData*>(userp);
    
    data->downloadedBytes = static_cast<int64_t>(dlnow);
    data->totalBytes = static_cast<int64_t>(dltotal);
    
    if (data->progressCallback) {
        if (!data->progressCallback(data->downloadedBytes, data->totalBytes)) {
            data->cancelled = true;
            return 1; // 中止
        }
    }
    
    return 0; // 継続
}

// =============================================================================
// HttpClient 実装構造体
// =============================================================================

struct HttpClient::Impl {
    CURL* curl = nullptr;
    HttpClientConfig config;
    struct curl_slist* headerList = nullptr;
    
    // コールバック用データ
    CurlCallbackData callbackData;
    
    Impl() = default;
    ~Impl() {
        Cleanup();
    }
    
    void Cleanup() {
        if (headerList) {
            curl_slist_free_all(headerList);
            headerList = nullptr;
        }
        if (curl) {
            curl_easy_cleanup(curl);
            curl = nullptr;
        }
    }
    
    bool Init() {
        if (!CurlGlobalInit::IsInitialized()) {
            CurlGlobalInit::Initialize();
        }
        
        curl = curl_easy_init();
        if (!curl) {
            LOG_ERROR("Failed to create CURL handle");
            return false;
        }
        return true;
    }
    
    void Reset() {
        if (curl) {
            curl_easy_reset(curl);
        }
        if (headerList) {
            curl_slist_free_all(headerList);
            headerList = nullptr;
        }
        callbackData.Reset();
    }
    
    void ApplyConfig() {
        if (!curl) return;
        
        // User-Agent
        curl_easy_setopt(curl, CURLOPT_USERAGENT, config.userAgent.c_str());
        
        // タイムアウト
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, config.connectTimeoutMs);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, config.readTimeoutMs);
        
        // リダイレクト
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, config.followRedirects ? 1L : 0L);
        curl_easy_setopt(curl, CURLOPT_MAXREDIRS, static_cast<long>(config.maxRedirects));
        
        // SSL設定（本番環境では証明書検証を有効に）
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
        
        // カスタムヘッダー
        for (const auto& [key, value] : config.headers) {
            std::string header = key + ": " + value;
            headerList = curl_slist_append(headerList, header.c_str());
        }
        if (headerList) {
            curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headerList);
        }
    }
};

// =============================================================================
// HttpClient コンストラクタ・デストラクタ
// =============================================================================

HttpClient::HttpClient() : m_impl(std::make_unique<Impl>()) {
    if (!m_impl->Init()) {
        LOG_ERROR("HttpClient initialization failed");
    }
}

HttpClient::HttpClient(const HttpClientConfig& config) : m_impl(std::make_unique<Impl>()) {
    m_impl->config = config;
    if (!m_impl->Init()) {
        LOG_ERROR("HttpClient initialization failed");
    }
}

HttpClient::~HttpClient() = default;

HttpClient::HttpClient(HttpClient&&) noexcept = default;
HttpClient& HttpClient::operator=(HttpClient&&) noexcept = default;

// =============================================================================
// 設定メソッド
// =============================================================================

void HttpClient::Configure(const HttpClientConfig& config) {
    m_impl->config = config;
}

const HttpClientConfig& HttpClient::GetConfig() const {
    return m_impl->config;
}

// =============================================================================
// HTTPリクエスト実装
// =============================================================================

HttpResponse HttpClient::Head(const std::string& url) {
    HttpResponse response;
    
    if (!m_impl->curl) {
        response.errorMessage = "CURL not initialized";
        return response;
    }
    
    LOG_TRACE("HEAD request: {}", url);
    
    m_impl->Reset();
    m_impl->ApplyConfig();
    
    curl_easy_setopt(m_impl->curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(m_impl->curl, CURLOPT_NOBODY, 1L); // HEADリクエスト
    curl_easy_setopt(m_impl->curl, CURLOPT_HEADERFUNCTION, HeaderCallback);
    curl_easy_setopt(m_impl->curl, CURLOPT_HEADERDATA, &m_impl->callbackData);
    
    CURLcode result = curl_easy_perform(m_impl->curl);
    
    if (result == CURLE_OK) {
        long httpCode = 0;
        curl_easy_getinfo(m_impl->curl, CURLINFO_RESPONSE_CODE, &httpCode);
        response.statusCode = static_cast<int>(httpCode);
        response.headers = m_impl->callbackData.responseHeaders;
        response.success = (httpCode >= 200 && httpCode < 300);
        
        // Content-Length取得
        auto it = m_impl->callbackData.responseHeaders.find("Content-Length");
        if (it == m_impl->callbackData.responseHeaders.end()) {
            it = m_impl->callbackData.responseHeaders.find("content-length");
        }
        if (it != m_impl->callbackData.responseHeaders.end()) {
            response.contentLength = std::stoll(it->second);
        }
        
        // Accept-Ranges確認
        auto rangesIt = m_impl->callbackData.responseHeaders.find("Accept-Ranges");
        if (rangesIt == m_impl->callbackData.responseHeaders.end()) {
            rangesIt = m_impl->callbackData.responseHeaders.find("accept-ranges");
        }
        if (rangesIt != m_impl->callbackData.responseHeaders.end()) {
            response.acceptsRanges = (rangesIt->second == "bytes");
        }
        
        LOG_TRACE("HEAD response: {} (Content-Length: {})", response.statusCode, response.contentLength);
    } else {
        response.errorMessage = curl_easy_strerror(result);
        LOG_ERROR("HEAD request failed: {}", response.errorMessage);
    }
    
    return response;
}

HttpResponse HttpClient::Get(const std::string& url) {
    HttpResponse response;
    
    if (!m_impl->curl) {
        response.errorMessage = "CURL not initialized";
        return response;
    }
    
    LOG_TRACE("GET request: {}", url);
    
    m_impl->Reset();
    m_impl->ApplyConfig();
    
    curl_easy_setopt(m_impl->curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(m_impl->curl, CURLOPT_HTTPGET, 1L);
    curl_easy_setopt(m_impl->curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(m_impl->curl, CURLOPT_WRITEDATA, &m_impl->callbackData);
    curl_easy_setopt(m_impl->curl, CURLOPT_HEADERFUNCTION, HeaderCallback);
    curl_easy_setopt(m_impl->curl, CURLOPT_HEADERDATA, &m_impl->callbackData);
    
    CURLcode result = curl_easy_perform(m_impl->curl);
    
    if (result == CURLE_OK) {
        long httpCode = 0;
        curl_easy_getinfo(m_impl->curl, CURLINFO_RESPONSE_CODE, &httpCode);
        response.statusCode = static_cast<int>(httpCode);
        response.headers = m_impl->callbackData.responseHeaders;
        response.data = std::move(m_impl->callbackData.responseData);
        response.success = (httpCode >= 200 && httpCode < 300);
        
        LOG_TRACE("GET response: {} ({} bytes)", response.statusCode, response.data.size());
    } else {
        response.errorMessage = curl_easy_strerror(result);
        LOG_ERROR("GET request failed: {}", response.errorMessage);
    }
    
    return response;
}

HttpResponse HttpClient::GetRange(const std::string& url, int64_t startByte, int64_t endByte) {
    HttpResponse response;
    
    if (!m_impl->curl) {
        response.errorMessage = "CURL not initialized";
        return response;
    }
    
    LOG_TRACE("GET Range request: {} (bytes={}-{})", url, startByte, endByte);
    
    m_impl->Reset();
    m_impl->ApplyConfig();
    
    // Rangeヘッダー設定
    std::ostringstream rangeHeader;
    rangeHeader << startByte << "-" << endByte;
    curl_easy_setopt(m_impl->curl, CURLOPT_RANGE, rangeHeader.str().c_str());
    
    curl_easy_setopt(m_impl->curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(m_impl->curl, CURLOPT_HTTPGET, 1L);
    curl_easy_setopt(m_impl->curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(m_impl->curl, CURLOPT_WRITEDATA, &m_impl->callbackData);
    curl_easy_setopt(m_impl->curl, CURLOPT_HEADERFUNCTION, HeaderCallback);
    curl_easy_setopt(m_impl->curl, CURLOPT_HEADERDATA, &m_impl->callbackData);
    
    CURLcode result = curl_easy_perform(m_impl->curl);
    
    if (result == CURLE_OK) {
        long httpCode = 0;
        curl_easy_getinfo(m_impl->curl, CURLINFO_RESPONSE_CODE, &httpCode);
        response.statusCode = static_cast<int>(httpCode);
        response.headers = m_impl->callbackData.responseHeaders;
        response.data = std::move(m_impl->callbackData.responseData);
        response.success = (httpCode >= 200 && httpCode < 300);
        
        LOG_TRACE("GET Range response: {} ({} bytes)", response.statusCode, response.data.size());
    } else {
        response.errorMessage = curl_easy_strerror(result);
        LOG_ERROR("GET Range request failed: {}", response.errorMessage);
    }
    
    return response;
}

HttpResponse HttpClient::GetRangeStreaming(
    const std::string& url,
    int64_t startByte,
    int64_t endByte,
    DataCallback dataCallback,
    ProgressCallback progressCallback)
{
    HttpResponse response;
    
    if (!m_impl->curl) {
        response.errorMessage = "CURL not initialized";
        return response;
    }
    
    LOG_TRACE("GET Range Streaming request: {} (bytes={}-{})", url, startByte, endByte);
    
    m_impl->Reset();
    m_impl->ApplyConfig();
    m_impl->callbackData.streamCallback = std::move(dataCallback);
    m_impl->callbackData.progressCallback = std::move(progressCallback);
    
    // Rangeヘッダー設定
    std::ostringstream rangeHeader;
    rangeHeader << startByte << "-" << endByte;
    curl_easy_setopt(m_impl->curl, CURLOPT_RANGE, rangeHeader.str().c_str());
    
    curl_easy_setopt(m_impl->curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(m_impl->curl, CURLOPT_HTTPGET, 1L);
    curl_easy_setopt(m_impl->curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(m_impl->curl, CURLOPT_WRITEDATA, &m_impl->callbackData);
    curl_easy_setopt(m_impl->curl, CURLOPT_HEADERFUNCTION, HeaderCallback);
    curl_easy_setopt(m_impl->curl, CURLOPT_HEADERDATA, &m_impl->callbackData);
    
    // 進捗コールバック
    if (m_impl->callbackData.progressCallback) {
        curl_easy_setopt(m_impl->curl, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(m_impl->curl, CURLOPT_XFERINFOFUNCTION, ProgressCallbackFunc);
        curl_easy_setopt(m_impl->curl, CURLOPT_XFERINFODATA, &m_impl->callbackData);
    }
    
    CURLcode result = curl_easy_perform(m_impl->curl);
    
    if (result == CURLE_OK || (result == CURLE_ABORTED_BY_CALLBACK && m_impl->callbackData.cancelled)) {
        long httpCode = 0;
        curl_easy_getinfo(m_impl->curl, CURLINFO_RESPONSE_CODE, &httpCode);
        response.statusCode = static_cast<int>(httpCode);
        response.headers = m_impl->callbackData.responseHeaders;
        response.success = (httpCode >= 200 && httpCode < 300);
        
        if (m_impl->callbackData.cancelled) {
            response.errorMessage = "Cancelled by user";
            response.success = false;
        }
        
        LOG_TRACE("GET Range Streaming response: {} (downloaded: {} bytes)", 
                  response.statusCode, m_impl->callbackData.downloadedBytes);
    } else {
        response.errorMessage = curl_easy_strerror(result);
        LOG_ERROR("GET Range Streaming request failed: {}", response.errorMessage);
    }
    
    return response;
}

// =============================================================================
// ユーティリティメソッド
// =============================================================================

bool HttpClient::IsHttpUrl(const std::string& url) {
    if (url.size() < 7) return false;
    
    std::string prefix = url.substr(0, 8);
    std::transform(prefix.begin(), prefix.end(), prefix.begin(), ::tolower);
    
    return prefix.substr(0, 7) == "http://" || prefix == "https://";
}

int64_t HttpClient::GetContentLength(const std::string& url) {
    auto response = Head(url);
    return response.success ? response.contentLength : -1;
}

bool HttpClient::SupportsRangeRequests(const std::string& url) {
    auto response = Head(url);
    return response.success && response.acceptsRanges;
}

} // namespace io
} // namespace ytdlpspout
