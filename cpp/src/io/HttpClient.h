// =============================================================================
// HttpClient.h - HTTP通信クライアント (libcurlラッパー)
// =============================================================================
//
// 機能:
//   - HTTP GET/HEADリクエスト
//   - Range Request対応
//   - 非同期ダウンロード
//   - ヘッダー設定（User-Agent、Cookie等）
//
// =============================================================================

#pragma once

#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <cstdint>
#include <optional>
#include <map>

namespace ytdlpspout {
namespace io {

/// @brief HTTPレスポンス情報
struct HttpResponse {
    int statusCode = 0;                            // HTTPステータスコード
    std::map<std::string, std::string> headers;    // レスポンスヘッダー
    std::vector<uint8_t> data;                     // レスポンスボディ
    std::string errorMessage;                      // エラーメッセージ
    bool success = false;                          // 成功フラグ
    int64_t contentLength = -1;                    // Content-Length
    bool acceptsRanges = false;                    // Range Request対応
};

/// @brief HTTP設定
struct HttpClientConfig {
    std::string userAgent = "ytdlpSpout/1.0";      // User-Agent
    int connectTimeoutMs = 10000;                  // 接続タイムアウト（ミリ秒）
    int readTimeoutMs = 30000;                     // 読み取りタイムアウト（ミリ秒）
    int maxRetries = 3;                            // 最大リトライ回数
    int retryDelayMs = 1000;                       // リトライ間隔（ミリ秒）
    bool followRedirects = true;                   // リダイレクトを追跡
    int maxRedirects = 10;                         // 最大リダイレクト回数
    std::map<std::string, std::string> headers;   // 追加ヘッダー
};

/// @brief ダウンロード進捗コールバック
/// @param downloaded ダウンロード済みバイト数
/// @param total 合計バイト数
/// @return 継続する場合true、中断する場合false
using ProgressCallback = std::function<bool(int64_t downloaded, int64_t total)>;

/// @brief データ受信コールバック
/// @param data 受信データ
/// @param size データサイズ
using DataCallback = std::function<void(const uint8_t* data, size_t size)>;

/// @brief HTTPクライアントクラス
class HttpClient {
public:
    HttpClient();
    explicit HttpClient(const HttpClientConfig& config);
    ~HttpClient();

    // コピー禁止
    HttpClient(const HttpClient&) = delete;
    HttpClient& operator=(const HttpClient&) = delete;

    // ムーブ許可
    HttpClient(HttpClient&&) noexcept;
    HttpClient& operator=(HttpClient&&) noexcept;

    // =========================================================================
    // 設定
    // =========================================================================

    /// @brief 設定を更新
    /// @param config 新しい設定
    void Configure(const HttpClientConfig& config);

    /// @brief 現在の設定を取得
    /// @return 現在の設定
    const HttpClientConfig& GetConfig() const;

    // =========================================================================
    // リクエスト
    // =========================================================================

    /// @brief HEADリクエストを実行
    /// @param url URL
    /// @return レスポンス
    HttpResponse Head(const std::string& url);

    /// @brief GETリクエストを実行
    /// @param url URL
    /// @return レスポンス
    HttpResponse Get(const std::string& url);

    /// @brief 範囲指定GETリクエストを実行
    /// @param url URL
    /// @param startByte 開始バイト位置
    /// @param endByte 終了バイト位置（含む）
    /// @return レスポンス
    HttpResponse GetRange(const std::string& url, int64_t startByte, int64_t endByte);

    /// @brief ストリーミングGETリクエストを実行
    /// @param url URL
    /// @param startByte 開始バイト位置
    /// @param endByte 終了バイト位置（含む）
    /// @param dataCallback データ受信時のコールバック
    /// @param progressCallback 進捗コールバック（オプション）
    /// @return レスポンス（dataは空）
    HttpResponse GetRangeStreaming(
        const std::string& url,
        int64_t startByte,
        int64_t endByte,
        DataCallback dataCallback,
        ProgressCallback progressCallback = nullptr
    );

    // =========================================================================
    // ユーティリティ
    // =========================================================================

    /// @brief URLがHTTP/HTTPSかどうかを判定
    /// @param url 判定するURL
    /// @return HTTP/HTTPSの場合true
    static bool IsHttpUrl(const std::string& url);

    /// @brief ファイルサイズを取得（HEADリクエスト）
    /// @param url URL
    /// @return ファイルサイズ（取得失敗時は-1）
    int64_t GetContentLength(const std::string& url);

    /// @brief Range Request対応を確認
    /// @param url URL
    /// @return 対応している場合true
    bool SupportsRangeRequests(const std::string& url);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

/// @brief libcurl グローバル初期化/終了
class CurlGlobalInit {
public:
    static void Initialize();
    static void Shutdown();
    static bool IsInitialized();

private:
    static bool s_initialized;
};

} // namespace io
} // namespace ytdlpspout
