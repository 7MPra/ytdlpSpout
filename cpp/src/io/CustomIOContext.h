// =============================================================================
// CustomIOContext.h - FFmpeg AVIOContext カスタム実装
// =============================================================================
//
// 機能:
//   - read_packet / seek コールバック
//   - SparseFileCacheとChunkDownloaderの統合
//   - シームレスなファイル/URL切り替え
//
// =============================================================================

#pragma once

#include <string>
#include <memory>
#include <cstdint>
#include <functional>

// FFmpeg前方宣言
struct AVIOContext;
struct AVFormatContext;

namespace ytdlpspout {
namespace io {

// 前方宣言
class HttpClient;
class SparseFileCache;
class ChunkDownloader;
class PrefetchScheduler;

/// @brief CustomIOContext設定
struct CustomIOContextConfig {
    size_t bufferSize = 64 * 1024;             // IOバッファサイズ（64KB）
    size_t chunkSize = 2 * 1024 * 1024;        // チャンクサイズ（2MB）
    size_t maxCacheMemory = 256 * 1024 * 1024; // 最大キャッシュメモリ（256MB）
    int maxConcurrentDownloads = 6;            // 最大並列ダウンロード数
    int readTimeoutMs = 30000;                 // 読み取りタイムアウト
    int prefetchChunksAhead = 24;              // 先読みチャンク数
    int criticalChunksAhead = 6;               // 最優先チャンク数（即座にダウンロード）
    bool enableContinuousDownload = true;      // ファイル全体を継続ダウンロードするか
};

/// @brief IOソースタイプ
enum class IOSourceType {
    Unknown,
    LocalFile,
    HttpUrl
};

/// @brief CustomIOContextクラス
/// @details HTTP URLからのストリーミング再生をサポートするカスタムAVIOContext
class CustomIOContext {
public:
    CustomIOContext();
    ~CustomIOContext();

    // コピー禁止
    CustomIOContext(const CustomIOContext&) = delete;
    CustomIOContext& operator=(const CustomIOContext&) = delete;

    // =========================================================================
    // 初期化
    // =========================================================================

    /// @brief 初期化
    /// @param pathOrUrl ファイルパスまたはURL
    /// @param config 設定
    /// @return 成功した場合true
    bool Initialize(const std::string& pathOrUrl, const CustomIOContextConfig& config = {});

    /// @brief クローズ
    void Close();

    /// @brief 初期化済みか
    /// @return 初期化済みの場合true
    bool IsInitialized() const;

    // =========================================================================
    // FFmpeg統合
    // =========================================================================

    /// @brief AVIOContextを取得
    /// @return AVIOContextポインタ（所有権は移動しない）
    AVIOContext* GetAVIOContext() const;

    /// @brief AVFormatContextにアタッチ
    /// @param formatCtx AVFormatContext
    /// @return 成功した場合true
    bool AttachToFormatContext(AVFormatContext* formatCtx);

    // =========================================================================
    // 情報取得
    // =========================================================================

    /// @brief ソースタイプを取得
    /// @return ソースタイプ
    IOSourceType GetSourceType() const;

    /// @brief ファイル/コンテンツサイズを取得
    /// @return サイズ（不明の場合-1）
    int64_t GetSize() const;

    /// @brief 現在の読み取り位置を取得
    /// @return 現在位置（バイトオフセット）
    int64_t GetPosition() const;

    /// @brief シーク可能か
    /// @return シーク可能な場合true
    bool IsSeekable() const;

    /// @brief URL/パスを取得
    /// @return URL/パス文字列
    const std::string& GetUrl() const;

    // =========================================================================
    // 再生制御通知
    // =========================================================================

    /// @brief 再生位置を更新（プリフェッチ用）
    /// @param byteOffset 現在のバイトオフセット
    void UpdatePlaybackPosition(int64_t byteOffset);

    /// @brief シーク通知
    /// @param byteOffset シーク先のバイトオフセット
    void NotifySeek(int64_t byteOffset);

    // =========================================================================
    // キャッシュ統計
    // =========================================================================

    /// @brief キャッシュ済みチャンク数を取得（現在メモリ上にあるチャンク数）
    /// @return キャッシュ済みチャンク数
    size_t GetCachedChunkCount() const;

    /// @brief ダウンロード完了済みチャンク数を取得（LRU削除されても減らない）
    /// @return ダウンロード完了済みチャンク数
    size_t GetDownloadedChunkCount() const;

    /// @brief 総チャンク数を取得
    /// @return 総チャンク数
    size_t GetTotalChunkCount() const;

    /// @brief ダウンロード進捗を取得（0.0〜1.0）
    /// @return ダウンロード進捗
    double GetDownloadProgress() const;

    // =========================================================================
    // ユーティリティ
    // =========================================================================

    /// @brief パス/URLからソースタイプを判定
    /// @param pathOrUrl パス/URL
    /// @return ソースタイプ
    static IOSourceType DetectSourceType(const std::string& pathOrUrl);

private:
    /// @brief AVIOContextコールバック - read_packet
    static int ReadPacket(void* opaque, uint8_t* buf, int bufSize);

    /// @brief AVIOContextコールバック - seek
    static int64_t Seek(void* opaque, int64_t offset, int whence);

    /// @brief HTTP初期化処理
    bool InitializeHttp();
    
    /// @brief ローカルファイル初期化処理
    bool InitializeLocalFile();
    
    /// @brief AVIOContext作成
    bool CreateAVIOContext();

    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace io
} // namespace ytdlpspout
