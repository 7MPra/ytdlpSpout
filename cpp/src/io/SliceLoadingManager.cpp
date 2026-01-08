// =============================================================================
// SliceLoadingManager.cpp - スライス読み込みマネージャー 実装
// =============================================================================

#include "SliceLoadingManager.h"
#include "CustomIOContext.h"
#include "SparseFileCache.h"
#include "ChunkDownloader.h"
#include "PrefetchScheduler.h"
#include "../ytdlp/YtDlpResolver.h"
#include "../utils/Logger.h"

#include <filesystem>
#include <fstream>
#include <regex>

namespace ytdlpspout {
namespace io {

// =============================================================================
// 内部実装クラス
// =============================================================================

struct SliceLoadingManager::Impl {
    // コンポーネント
    std::unique_ptr<CustomIOContext> ioContext;
    
    // 設定
    SliceLoadingConfig config;
    
    // ソース情報
    std::string source;
    std::string resolvedUrl;
    SliceSourceType sourceType = SliceSourceType::Unknown;
    int64_t fileSize = 0;
    
    // 動画情報
    double duration = 0.0;
    int bitrate = 0;
    
    // 状態
    bool isOpen = false;
    
    // ソースタイプを検出
    static SliceSourceType DetectSourceType(const std::string& pathOrUrl) {
        // 空文字列
        if (pathOrUrl.empty()) {
            return SliceSourceType::Unknown;
        }
        
        // http:// または https:// で始まる
        if (pathOrUrl.find("http://") == 0 || pathOrUrl.find("https://") == 0) {
            // YouTube等のyt-dlp対応URLかどうか判定
            if (ytdlp::YtDlpResolver::IsSupportedUrl(pathOrUrl)) {
                return SliceSourceType::YtDlpUrl;
            }
            return SliceSourceType::HttpUrl;
        }
        
        // ローカルファイルパス
        // Windowsパス（C:\...）またはUNIXパス（/...）
        if ((pathOrUrl.length() >= 2 && pathOrUrl[1] == ':') ||
            pathOrUrl[0] == '/' ||
            pathOrUrl[0] == '\\') {
            return SliceSourceType::LocalFile;
        }
        
        // 相対パスもローカルファイルとして扱う
        return SliceSourceType::LocalFile;
    }
    
    // ローカルファイルを開く
    bool OpenLocalFile() {
        // ファイル存在確認
        if (!std::filesystem::exists(source)) {
            LOG_ERROR("File not found: {}", source);
            return false;
        }
        
        // ファイルサイズを取得
        try {
            fileSize = static_cast<int64_t>(std::filesystem::file_size(source));
        } catch (const std::exception& e) {
            LOG_ERROR("Failed to get file size: {}", e.what());
            return false;
        }
        
        resolvedUrl = source;
        
        // CustomIOContextを初期化
        CustomIOContextConfig ioConfig;
        ioConfig.chunkSize = config.chunkSize;
        ioConfig.maxCacheMemory = config.maxCacheMemory;
        ioConfig.maxConcurrentDownloads = config.maxConcurrentDownloads;
        ioConfig.prefetchChunksAhead = config.prefetchChunksAhead;
        ioConfig.criticalChunksAhead = config.criticalChunksAhead;
        ioConfig.enableContinuousDownload = config.enableContinuousDownload;
        
        ioContext = std::make_unique<CustomIOContext>();
        if (!ioContext->Initialize(source, ioConfig)) {
            LOG_ERROR("Failed to initialize CustomIOContext for: {}", source);
            ioContext.reset();
            return false;
        }
        
        LOG_INFO("Opened local file: {} ({} bytes)", source, fileSize);
        return true;
    }
    
    // HTTP URLを開く
    bool OpenHttpUrl() {
        // CustomIOContextを初期化
        CustomIOContextConfig ioConfig;
        ioConfig.chunkSize = config.chunkSize;
        ioConfig.maxCacheMemory = config.maxCacheMemory;
        ioConfig.maxConcurrentDownloads = config.maxConcurrentDownloads;
        ioConfig.prefetchChunksAhead = config.prefetchChunksAhead;
        ioConfig.criticalChunksAhead = config.criticalChunksAhead;
        ioConfig.enableContinuousDownload = config.enableContinuousDownload;
        
        resolvedUrl = source;
        
        ioContext = std::make_unique<CustomIOContext>();
        if (!ioContext->Initialize(source, ioConfig)) {
            LOG_ERROR("Failed to initialize CustomIOContext for HTTP: {}", source);
            ioContext.reset();
            return false;
        }
        
        fileSize = ioContext->GetSize();
        
        LOG_INFO("Opened HTTP URL: {} ({} bytes)", source, fileSize);
        return true;
    }
    
    // yt-dlp URLを開く
    bool OpenYtDlpUrl() {
        // yt-dlpでストリームURLを解決
        ytdlp::YtDlpResolver resolver;
        
        if (!config.ytdlpPath.empty()) {
            resolver.SetYtDlpPath(config.ytdlpPath);
        }
        
        auto streamUrl = resolver.GetStreamUrl(source, config.preferredHeight);
        if (!streamUrl) {
            LOG_ERROR("Failed to resolve yt-dlp URL: {}", source);
            return false;
        }
        
        resolvedUrl = *streamUrl;
        LOG_INFO("Resolved yt-dlp URL: {} -> {}", source, resolvedUrl);
        
        // メタデータも取得してビットレート/duration取得
        auto metadata = resolver.ResolveUrl(source);
        if (metadata) {
            duration = metadata->duration;
            // ベストフォーマットを選択してビットレートを取得
            auto format = resolver.SelectBestFormat(*metadata, config.preferredHeight);
            if (format) {
                bitrate = format->tbr * 1000;  // kbps -> bps
            }
        }
        
        // 解決したURLでCustomIOContextを初期化
        CustomIOContextConfig ioConfig;
        ioConfig.chunkSize = config.chunkSize;
        ioConfig.maxCacheMemory = config.maxCacheMemory;
        ioConfig.maxConcurrentDownloads = config.maxConcurrentDownloads;
        ioConfig.prefetchChunksAhead = config.prefetchChunksAhead;
        ioConfig.criticalChunksAhead = config.criticalChunksAhead;
        ioConfig.enableContinuousDownload = config.enableContinuousDownload;
        
        ioContext = std::make_unique<CustomIOContext>();
        if (!ioContext->Initialize(resolvedUrl, ioConfig)) {
            LOG_ERROR("Failed to initialize CustomIOContext for resolved URL: {}", resolvedUrl);
            ioContext.reset();
            return false;
        }
        
        fileSize = ioContext->GetSize();
        
        LOG_INFO("Opened yt-dlp source: {} ({} bytes)", source, fileSize);
        return true;
    }
    
    // 秒数からバイトオフセットを計算
    int64_t SecondsToBytes(double seconds) const {
        if (bitrate <= 0 || duration <= 0) {
            // ビットレート不明の場合、ファイルサイズと時間から推定
            if (duration > 0 && fileSize > 0) {
                return static_cast<int64_t>((seconds / duration) * fileSize);
            }
            return 0;
        }
        
        // bitrate (bps) * seconds / 8 (bits to bytes)
        return static_cast<int64_t>((static_cast<double>(bitrate) * seconds) / 8.0);
    }
};

// =============================================================================
// コンストラクタ・デストラクタ
// =============================================================================

SliceLoadingManager::SliceLoadingManager()
    : m_impl(std::make_unique<Impl>()) {
}

SliceLoadingManager::~SliceLoadingManager() {
    Close();
}

// =============================================================================
// 初期化
// =============================================================================

bool SliceLoadingManager::Open(const std::string& source, const SliceLoadingConfig& config) {
    // 既に開いている場合は閉じる
    if (m_impl->isOpen) {
        Close();
    }
    
    m_impl->source = source;
    m_impl->config = config;
    m_impl->sourceType = Impl::DetectSourceType(source);
    
    LOG_DEBUG("Opening source: {} (type: {})", source, static_cast<int>(m_impl->sourceType));
    
    bool result = false;
    
    switch (m_impl->sourceType) {
        case SliceSourceType::LocalFile:
            result = m_impl->OpenLocalFile();
            break;
            
        case SliceSourceType::HttpUrl:
            result = m_impl->OpenHttpUrl();
            break;
            
        case SliceSourceType::YtDlpUrl:
            result = m_impl->OpenYtDlpUrl();
            break;
            
        default:
            LOG_ERROR("Unknown source type: {}", source);
            return false;
    }
    
    m_impl->isOpen = result;
    return result;
}

void SliceLoadingManager::Close() {
    if (m_impl->ioContext) {
        m_impl->ioContext->Close();
        m_impl->ioContext.reset();
    }
    
    m_impl->source.clear();
    m_impl->resolvedUrl.clear();
    m_impl->sourceType = SliceSourceType::Unknown;
    m_impl->fileSize = 0;
    m_impl->duration = 0.0;
    m_impl->bitrate = 0;
    m_impl->isOpen = false;
    
    LOG_DEBUG("SliceLoadingManager closed");
}

bool SliceLoadingManager::IsOpen() const {
    return m_impl->isOpen;
}

// =============================================================================
// FFmpeg連携
// =============================================================================

AVIOContext* SliceLoadingManager::GetAVIOContext() {
    if (!m_impl->ioContext) {
        return nullptr;
    }
    return m_impl->ioContext->GetAVIOContext();
}

CustomIOContext* SliceLoadingManager::GetCustomIOContext() {
    return m_impl->ioContext.get();
}

int64_t SliceLoadingManager::GetFileSize() const {
    return m_impl->fileSize;
}

SliceSourceType SliceLoadingManager::GetSourceType() const {
    return m_impl->sourceType;
}

const std::string& SliceLoadingManager::GetResolvedUrl() const {
    return m_impl->resolvedUrl;
}

// =============================================================================
// 再生位置連携
// =============================================================================

void SliceLoadingManager::UpdatePlaybackPosition(double seconds) {
    if (!m_impl->ioContext) {
        return;
    }
    
    int64_t byteOffset = m_impl->SecondsToBytes(seconds);
    m_impl->ioContext->UpdatePlaybackPosition(byteOffset);
}

void SliceLoadingManager::UpdatePlaybackPositionBytes(int64_t byteOffset) {
    if (!m_impl->ioContext) {
        return;
    }
    m_impl->ioContext->UpdatePlaybackPosition(byteOffset);
}

void SliceLoadingManager::NotifySeek(double seconds) {
    if (!m_impl->ioContext) {
        return;
    }
    
    int64_t byteOffset = m_impl->SecondsToBytes(seconds);
    m_impl->ioContext->NotifySeek(byteOffset);
}

void SliceLoadingManager::NotifySeekBytes(int64_t byteOffset) {
    if (!m_impl->ioContext) {
        return;
    }
    m_impl->ioContext->NotifySeek(byteOffset);
}

// =============================================================================
// 統計
// =============================================================================

double SliceLoadingManager::GetDownloadProgress() const {
    if (!m_impl->ioContext) {
        return 0.0;
    }
    
    size_t total = GetTotalChunkCount();
    if (total == 0) {
        return 0.0;
    }
    
    size_t cached = GetCachedChunkCount();
    return static_cast<double>(cached) / static_cast<double>(total);
}

double SliceLoadingManager::GetBandwidth() const {
    // ローカルファイルの場合は0を返す（無限帯域）
    if (m_impl->sourceType == SliceSourceType::LocalFile) {
        return 0.0;
    }
    
    // TODO: 実際の帯域幅測定を実装
    // ChunkDownloaderから統計情報を取得
    return 0.0;
}

bool SliceLoadingManager::IsFullyCached() const {
    if (!m_impl->ioContext) {
        return false;
    }
    
    // ローカルファイルは常にキャッシュ済み
    if (m_impl->sourceType == SliceSourceType::LocalFile) {
        return true;
    }
    
    return GetCachedChunkCount() == GetTotalChunkCount();
}

size_t SliceLoadingManager::GetCachedChunkCount() const {
    if (!m_impl->ioContext) {
        return 0;
    }
    
    // ローカルファイルは全チャンクがキャッシュ済み
    if (m_impl->sourceType == SliceSourceType::LocalFile) {
        return GetTotalChunkCount();
    }
    
    // CustomIOContext経由でSparseFileCacheから実際のキャッシュ済みチャンク数を取得
    return m_impl->ioContext->GetCachedChunkCount();
}

size_t SliceLoadingManager::GetTotalChunkCount() const {
    if (!m_impl->ioContext) {
        // ioContextがない場合はファイルサイズから計算
        if (m_impl->fileSize <= 0 || m_impl->config.chunkSize <= 0) {
            return 0;
        }
        return static_cast<size_t>((m_impl->fileSize + m_impl->config.chunkSize - 1) / m_impl->config.chunkSize);
    }
    
    // ローカルファイルの場合はファイルサイズから計算
    if (m_impl->sourceType == SliceSourceType::LocalFile) {
        if (m_impl->fileSize <= 0 || m_impl->config.chunkSize <= 0) {
            return 0;
        }
        return static_cast<size_t>((m_impl->fileSize + m_impl->config.chunkSize - 1) / m_impl->config.chunkSize);
    }
    
    // CustomIOContext経由でSparseFileCacheから総チャンク数を取得
    return m_impl->ioContext->GetTotalChunkCount();
}

// =============================================================================
// 動画情報
// =============================================================================

double SliceLoadingManager::GetDuration() const {
    return m_impl->duration;
}

int SliceLoadingManager::GetBitrate() const {
    return m_impl->bitrate;
}

} // namespace io
} // namespace ytdlpspout
