// =============================================================================
// CustomIOContext.cpp - FFmpeg AVIOContext カスタム実装
// =============================================================================

#include "io/CustomIOContext.h"
#include "io/HttpClient.h"
#include "io/SparseFileCache.h"
#include "io/ChunkDownloader.h"
#include "io/PrefetchScheduler.h"
#include "utils/Logger.h"

#include <algorithm>
#include <cstring>
#include <mutex>

// FFmpeg
extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/error.h>
}

namespace ytdlpspout {
namespace io {

// =============================================================================
// 定数
// =============================================================================
static constexpr size_t DEFAULT_AVIO_BUFFER_SIZE = 32 * 1024;  // 32KB

// =============================================================================
// 実装クラス
// =============================================================================
struct CustomIOContext::Impl {
    // 設定
    CustomIOContextConfig config;
    
    // URL情報
    std::string url;
    IOSourceType sourceType = IOSourceType::Unknown;
    
    // コンテンツ情報
    int64_t contentLength = -1;
    bool seekable = false;
    
    // 現在位置
    int64_t position = 0;
    
    // AVIOContext
    AVIOContext* avioContext = nullptr;
    uint8_t* avioBuffer = nullptr;
    
    // I/Oコンポーネント
    std::unique_ptr<HttpClient> httpClient;
    std::unique_ptr<SparseFileCache> cache;
    std::unique_ptr<ChunkDownloader> downloader;
    std::unique_ptr<PrefetchScheduler> prefetcher;
    
    // 状態
    bool initialized = false;
    std::mutex mutex;
    
    ~Impl() {
        // AVIOContextのクリーンアップ
        if (avioContext) {
            // バッファはavio_context_freeが解放するので、別途解放しない
            avio_context_free(&avioContext);
            avioContext = nullptr;
            avioBuffer = nullptr;
        }
    }
};

// =============================================================================
// コンストラクタ・デストラクタ
// =============================================================================

CustomIOContext::CustomIOContext()
    : m_impl(std::make_unique<Impl>()) {
}

CustomIOContext::~CustomIOContext() {
    Close();
}

// =============================================================================
// 初期化と終了
// =============================================================================

bool CustomIOContext::Initialize(const std::string& pathOrUrl, 
                                  const CustomIOContextConfig& config) {
    // 既に初期化されている場合はまず閉じる（ロック外で安全に実行）
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        if (m_impl->initialized) {
            LOG_WARN("CustomIOContext already initialized, closing first");
        }
    }
    Close();  // ロック外で呼び出し（Close内部で独自にロックを取得）
    
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    
    if (pathOrUrl.empty()) {
        LOG_ERROR("Empty path/URL provided");
        return false;
    }
    
    m_impl->config = config;
    m_impl->url = pathOrUrl;
    m_impl->sourceType = DetectSourceType(pathOrUrl);
    
    LOG_INFO("Initializing CustomIOContext for: {}", pathOrUrl);
    LOG_DEBUG("Source type: {}", 
              m_impl->sourceType == IOSourceType::HttpUrl ? "HTTP URL" :
              m_impl->sourceType == IOSourceType::LocalFile ? "Local file" : "Unknown");
    
    // ソースタイプに応じた初期化
    if (m_impl->sourceType == IOSourceType::HttpUrl) {
        return InitializeHttp();
    } else if (m_impl->sourceType == IOSourceType::LocalFile) {
        return InitializeLocalFile();
    } else {
        LOG_ERROR("Unknown source type for: {}", pathOrUrl);
        return false;
    }
}

bool CustomIOContext::InitializeHttp() {
    // HttpClientを作成
    m_impl->httpClient = std::make_unique<HttpClient>();
    
    HttpClientConfig httpConfig;
    httpConfig.connectTimeoutMs = 10000;
    httpConfig.readTimeoutMs = m_impl->config.readTimeoutMs;
    m_impl->httpClient->Configure(httpConfig);
    
    // HEADリクエストでContent-Lengthを取得
    LOG_DEBUG("Fetching content info via HEAD request...");
    HttpResponse headResponse = m_impl->httpClient->Head(m_impl->url);
    
    if (!headResponse.success) {
        LOG_ERROR("HEAD request failed: {}", headResponse.errorMessage);
        return false;
    }
    
    m_impl->contentLength = headResponse.contentLength;
    m_impl->seekable = headResponse.acceptsRanges;
    
    LOG_INFO("Content-Length: {}, Seekable: {}", 
             m_impl->contentLength, m_impl->seekable);
    
    if (m_impl->contentLength <= 0) {
        LOG_ERROR("Invalid content length: {}", m_impl->contentLength);
        return false;
    }
    
    // SparseFileCacheを初期化
    SparseFileCacheConfig cacheConfig;
    cacheConfig.chunkSize = m_impl->config.chunkSize;
    cacheConfig.maxMemoryBytes = m_impl->config.maxCacheMemory;
    
    m_impl->cache = std::make_unique<SparseFileCache>(cacheConfig);
    m_impl->cache->Initialize(m_impl->contentLength);
    
    // ChunkDownloaderを初期化
    m_impl->downloader = std::make_unique<ChunkDownloader>(
        m_impl->cache.get(), 
        m_impl->config.maxConcurrentDownloads
    );
    m_impl->downloader->SetUrl(m_impl->url);
    m_impl->downloader->Start();
    
    // PrefetchSchedulerを初期化
    m_impl->prefetcher = std::make_unique<PrefetchScheduler>();
    PrefetchConfig prefetchConfig;
    prefetchConfig.prefetchChunksAhead = m_impl->config.prefetchChunksAhead;
    prefetchConfig.criticalChunksAhead = m_impl->config.criticalChunksAhead;
    prefetchConfig.enableContinuousDownload = m_impl->config.enableContinuousDownload;
    prefetchConfig.updateIntervalMs = 50;  // より頻繁な更新
    m_impl->prefetcher->Initialize(m_impl->cache.get(), m_impl->downloader.get(), prefetchConfig);
    m_impl->prefetcher->Start();
    
    // AVIOContextを作成
    if (!CreateAVIOContext()) {
        LOG_ERROR("Failed to create AVIOContext");
        return false;
    }
    
    m_impl->initialized = true;
    LOG_INFO("CustomIOContext initialized successfully");
    return true;
}

bool CustomIOContext::InitializeLocalFile() {
    // ローカルファイルの場合、FFmpegの標準機能を使用
    // AVIOContextは作成せず、FFmpegに直接ファイルパスを渡す
    // ここでは簡易的に「初期化成功」とするが、実際の読み取りはFFmpegに任せる
    
    LOG_INFO("Local file mode - using FFmpeg's built-in file I/O");
    
    // ファイル存在確認（オプション）
    // 実際の読み取りはFFmpegに任せるので、ここでは初期化成功としておく
    m_impl->initialized = true;
    m_impl->seekable = true;  // ローカルファイルは通常シーク可能
    
    return true;
}

bool CustomIOContext::CreateAVIOContext() {
    // バッファを割り当て
    size_t bufferSize = m_impl->config.bufferSize;
    m_impl->avioBuffer = static_cast<uint8_t*>(av_malloc(bufferSize));
    if (!m_impl->avioBuffer) {
        LOG_ERROR("Failed to allocate AVIO buffer ({} bytes)", bufferSize);
        return false;
    }
    
    // AVIOContextを作成
    // write_flag=0 (読み取り専用)
    // seek関数を渡すことでシーク可能になる
    m_impl->avioContext = avio_alloc_context(
        m_impl->avioBuffer,
        static_cast<int>(bufferSize),
        0,  // write_flag
        this,  // opaque (コールバックに渡される)
        ReadPacket,  // read_packet
        nullptr,  // write_packet
        m_impl->seekable ? Seek : nullptr  // seek
    );
    
    if (!m_impl->avioContext) {
        LOG_ERROR("Failed to allocate AVIOContext");
        av_free(m_impl->avioBuffer);
        m_impl->avioBuffer = nullptr;
        return false;
    }
    
    // シーク可能フラグを設定
    m_impl->avioContext->seekable = m_impl->seekable ? AVIO_SEEKABLE_NORMAL : 0;
    
    LOG_DEBUG("AVIOContext created (buffer size: {}, seekable: {})", 
              bufferSize, m_impl->seekable);
    
    return true;
}

void CustomIOContext::Close() {
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    
    if (!m_impl->initialized) {
        return;
    }
    
    LOG_INFO("Closing CustomIOContext...");
    
    // プリフェッチャーを停止
    if (m_impl->prefetcher) {
        m_impl->prefetcher->Stop();
        m_impl->prefetcher->Shutdown();
        m_impl->prefetcher.reset();
    }
    
    // ダウンローダーを停止
    if (m_impl->downloader) {
        m_impl->downloader->Stop();
        m_impl->downloader.reset();
    }
    
    // キャッシュをクリア
    if (m_impl->cache) {
        m_impl->cache->Clear();
        m_impl->cache.reset();
    }
    
    // HTTPクライアントを解放
    m_impl->httpClient.reset();
    
    // AVIOContextを解放
    if (m_impl->avioContext) {
        // avio_context_freeはバッファも解放する
        avio_context_free(&m_impl->avioContext);
        m_impl->avioContext = nullptr;
        m_impl->avioBuffer = nullptr;
    }
    
    // 状態をリセット
    m_impl->initialized = false;
    m_impl->position = 0;
    m_impl->contentLength = -1;
    m_impl->seekable = false;
    m_impl->url.clear();
    m_impl->sourceType = IOSourceType::Unknown;
    
    LOG_INFO("CustomIOContext closed");
}

bool CustomIOContext::IsInitialized() const {
    return m_impl->initialized;
}

// =============================================================================
// FFmpeg統合
// =============================================================================

AVIOContext* CustomIOContext::GetAVIOContext() const {
    return m_impl->avioContext;
}

bool CustomIOContext::AttachToFormatContext(AVFormatContext* formatCtx) {
    if (!m_impl->initialized) {
        LOG_ERROR("Cannot attach: CustomIOContext not initialized");
        return false;
    }
    
    if (!formatCtx) {
        LOG_ERROR("Cannot attach: AVFormatContext is null");
        return false;
    }
    
    if (!m_impl->avioContext) {
        // ローカルファイルモードの場合はAVIOContextがない
        LOG_DEBUG("No custom AVIOContext (local file mode)");
        return true;
    }
    
    formatCtx->pb = m_impl->avioContext;
    formatCtx->flags |= AVFMT_FLAG_CUSTOM_IO;
    
    LOG_DEBUG("Attached AVIOContext to AVFormatContext");
    return true;
}

// =============================================================================
// 情報取得
// =============================================================================

IOSourceType CustomIOContext::GetSourceType() const {
    return m_impl->sourceType;
}

int64_t CustomIOContext::GetSize() const {
    return m_impl->contentLength;
}

int64_t CustomIOContext::GetPosition() const {
    return m_impl->position;
}

bool CustomIOContext::IsSeekable() const {
    return m_impl->seekable;
}

const std::string& CustomIOContext::GetUrl() const {
    return m_impl->url;
}

// =============================================================================
// 再生制御通知
// =============================================================================

void CustomIOContext::UpdatePlaybackPosition(int64_t byteOffset) {
    if (m_impl->prefetcher) {
        m_impl->prefetcher->UpdatePlaybackPosition(byteOffset);
    }
}

void CustomIOContext::NotifySeek(int64_t byteOffset) {
    if (m_impl->prefetcher) {
        m_impl->prefetcher->NotifySeek(byteOffset);
    }
}

// =============================================================================
// キャッシュ統計
// =============================================================================

size_t CustomIOContext::GetCachedChunkCount() const {
    if (!m_impl->cache) {
        return 0;
    }
    return static_cast<size_t>(m_impl->cache->GetCachedChunkCount());
}

size_t CustomIOContext::GetDownloadedChunkCount() const {
    if (!m_impl->cache) {
        return 0;
    }
    return static_cast<size_t>(m_impl->cache->GetDownloadedChunkCount());
}

size_t CustomIOContext::GetTotalChunkCount() const {
    if (!m_impl->cache) {
        return 0;
    }
    return static_cast<size_t>(m_impl->cache->GetChunkCount());
}

double CustomIOContext::GetDownloadProgress() const {
    size_t total = GetTotalChunkCount();
    if (total == 0) {
        // ローカルファイルモードまたは未初期化の場合
        if (m_impl->sourceType == IOSourceType::LocalFile) {
            return 1.0;  // ローカルファイルは100%キャッシュ済み
        }
        return 0.0;
    }
    // LRU削除されても減らないダウンロード完了数を使用
    return static_cast<double>(GetDownloadedChunkCount()) / static_cast<double>(total);
}

// =============================================================================
// ユーティリティ
// =============================================================================

IOSourceType CustomIOContext::DetectSourceType(const std::string& pathOrUrl) {
    if (pathOrUrl.empty()) {
        return IOSourceType::Unknown;
    }
    
    // HTTP/HTTPS URLのチェック
    if (pathOrUrl.size() >= 7) {
        std::string prefix = pathOrUrl.substr(0, 8);
        // 小文字に変換して比較
        std::transform(prefix.begin(), prefix.end(), prefix.begin(), ::tolower);
        
        if (prefix.substr(0, 7) == "http://") {
            return IOSourceType::HttpUrl;
        }
        if (prefix == "https://") {
            return IOSourceType::HttpUrl;
        }
    }
    
    // それ以外はローカルファイルとして扱う
    return IOSourceType::LocalFile;
}

// =============================================================================
// FFmpegコールバック
// =============================================================================

int CustomIOContext::ReadPacket(void* opaque, uint8_t* buf, int buf_size) {
    CustomIOContext* self = static_cast<CustomIOContext*>(opaque);
    if (!self || !self->m_impl->initialized) {
        return AVERROR(EIO);
    }
    
    int64_t position = self->m_impl->position;
    int64_t contentLength = self->m_impl->contentLength;
    
    // EOF判定
    if (position >= contentLength) {
        LOG_DEBUG("ReadPacket: EOF at position {}", position);
        return AVERROR_EOF;
    }
    
    // 読み取りサイズを調整（ファイル末尾を超えないように）
    int64_t remaining = contentLength - position;
    int readSize = static_cast<int>(std::min(static_cast<int64_t>(buf_size), remaining));
    
    // キャッシュから読み取り
    SparseFileCache* cache = self->m_impl->cache.get();
    if (!cache) {
        LOG_ERROR("ReadPacket: Cache not available");
        return AVERROR(EIO);
    }
    
    // 読み取り（ブロッキング）
    int64_t bytesRead = cache->Read(position, buf, readSize, 
                                     self->m_impl->config.readTimeoutMs);
    
    if (bytesRead < 0) {
        LOG_ERROR("ReadPacket: Read error at position {}", position);
        return AVERROR(EIO);
    }
    
    if (bytesRead == 0) {
        // タイムアウトまたはEOF
        if (position >= contentLength) {
            return AVERROR_EOF;
        }
        LOG_WARN("ReadPacket: Read timeout at position {}", position);
        return AVERROR(EAGAIN);
    }
    
    // 位置を更新
    self->m_impl->position = position + bytesRead;
    
    // プリフェッチ位置を更新
    if (self->m_impl->prefetcher) {
        self->m_impl->prefetcher->UpdatePlaybackPosition(self->m_impl->position);
    }
    
    LOG_TRACE("ReadPacket: Read {} bytes at position {}", bytesRead, position);
    
    return static_cast<int>(bytesRead);
}

int64_t CustomIOContext::Seek(void* opaque, int64_t offset, int whence) {
    CustomIOContext* self = static_cast<CustomIOContext*>(opaque);
    if (!self || !self->m_impl->initialized) {
        return AVERROR(EIO);
    }
    
    int64_t contentLength = self->m_impl->contentLength;
    int64_t newPosition;
    
    // AVSEEK_SIZE: ファイルサイズを返す
    if (whence == AVSEEK_SIZE) {
        LOG_DEBUG("Seek: AVSEEK_SIZE -> {}", contentLength);
        return contentLength;
    }
    
    // SEEK_SET/SEEK_CUR/SEEK_END
    switch (whence) {
        case SEEK_SET:
            newPosition = offset;
            break;
        case SEEK_CUR:
            newPosition = self->m_impl->position + offset;
            break;
        case SEEK_END:
            newPosition = contentLength + offset;
            break;
        default:
            LOG_ERROR("Seek: Invalid whence: {}", whence);
            return AVERROR(EINVAL);
    }
    
    // 範囲チェック
    if (newPosition < 0) {
        LOG_ERROR("Seek: Invalid position: {} (before start)", newPosition);
        return AVERROR(EINVAL);
    }
    
    if (newPosition > contentLength) {
        LOG_WARN("Seek: Position {} beyond content length {}", 
                 newPosition, contentLength);
        // FFmpegはファイル終端を超えてシークすることがある
        // その場合は許容する（次のReadでEOFを返す）
    }
    
    LOG_DEBUG("Seek: {} -> {} (whence={})", self->m_impl->position, newPosition, whence);
    
    self->m_impl->position = newPosition;
    
    // シーク通知（プリフェッチ再スケジュール）
    if (self->m_impl->prefetcher) {
        self->m_impl->prefetcher->NotifySeek(newPosition);
    }
    
    return newPosition;
}

} // namespace io
} // namespace ytdlpspout
