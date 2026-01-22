// =============================================================================
// HlsSliceLoadingManager.cpp - HLSスライス読み込み統合マネージャー 実装
// =============================================================================

#include "hls/HlsSliceLoadingManager.h"
#include "hls/M3U8Parser.h"
#include "hls/AesCbcDecryptor.h"
#include "hls/HlsSegmentCache.h"
#include "hls/HlsCustomAVIOContext.h"
#include "io/ChunkDownloader.h"
#include "io/HttpClient.h"
#include "io/SparseFileCache.h"
#include "utils/Logger.h"

#include <algorithm>
#include <cctype>
#include <mutex>
#include <atomic>
#include <thread>
#include <chrono>

namespace ytdlpspout {
namespace hls {

// =============================================================================
// 内部実装クラス
// =============================================================================

struct HlsSliceLoadingManager::Impl {
    // =========================================================================
    // コンポーネント
    // =========================================================================
    
    /// @brief セグメントキャッシュ
    std::unique_ptr<HlsSegmentCache> cache;
    
    /// @brief カスタムAVIOContext
    std::unique_ptr<HlsCustomAVIOContext> avioContext;
    
    /// @brief チャンクダウンローダー（セグメント用）
    std::unique_ptr<io::ChunkDownloader> downloader;
    
    /// @brief ダミーのSparseFileCache（ChunkDownloaderに必要）
    std::unique_ptr<io::SparseFileCache> sparseCache;
    
    /// @brief HTTPクライアント（m3u8ダウンロード用）
    std::unique_ptr<io::HttpClient> httpClient;
    
    // =========================================================================
    // 状態
    // =========================================================================
    
    /// @brief 設定
    HlsSliceConfig config;
    
    /// @brief プレイリスト
    M3U8Playlist playlist;
    
    /// @brief HLS URL
    std::string hlsUrl;
    
    /// @brief 暗号化キーデータ
    std::vector<uint8_t> encryptionKeyData;
    
    /// @brief 開いているか
    std::atomic<bool> isOpen{false};
    
    /// @brief ミューテックス
    mutable std::mutex mutex;
    
    /// @brief 帯域幅推定（bytes/sec）
    std::atomic<double> bandwidth{0.0};
    
    // =========================================================================
    // ヘルパーメソッド
    // =========================================================================
    
    /// @brief 文字列を小文字に変換
    static std::string ToLower(const std::string& str) {
        std::string result = str;
        std::transform(result.begin(), result.end(), result.begin(),
                      [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return result;
    }
    
    /// @brief m3u8をダウンロード
    bool DownloadM3U8(const std::string& url, std::string& content) {
        io::HttpClientConfig httpConfig;
        httpConfig.readTimeoutMs = config.readTimeoutMs;
        httpConfig.connectTimeoutMs = 10000;
        httpConfig.headers = config.httpHeaders;
        
        httpClient->Configure(httpConfig);
        
        auto response = httpClient->Get(url);
        if (!response.success || response.statusCode != 200) {
            LOG_ERROR("Failed to download m3u8: {} (status: {})", 
                      url, response.statusCode);
            return false;
        }
        
        content = std::string(response.data.begin(), response.data.end());
        return true;
    }
    
    /// @brief 暗号化キーをダウンロード
    bool DownloadEncryptionKey(const std::string& keyUrl) {
        io::HttpClientConfig httpConfig;
        httpConfig.readTimeoutMs = config.readTimeoutMs;
        httpConfig.headers = config.httpHeaders;
        
        httpClient->Configure(httpConfig);
        
        auto response = httpClient->Get(keyUrl);
        if (!response.success || response.statusCode != 200) {
            LOG_ERROR("Failed to download encryption key: {} (status: {})",
                      keyUrl, response.statusCode);
            return false;
        }
        
        if (response.data.size() != 16) {
            LOG_ERROR("Invalid encryption key size: {} (expected 16)", 
                      response.data.size());
            return false;
        }
        
        encryptionKeyData = std::move(response.data);
        LOG_INFO("Downloaded encryption key from: {}", keyUrl);
        return true;
    }
    
    /// @brief セグメントダウンロードコールバック
    void OnSegmentDownloaded(int64_t index, std::vector<uint8_t>&& data, bool success) {
        if (!success) {
            LOG_WARN("Segment {} download failed", index);
            return;
        }
        
        bool isEncrypted = playlist.encryptionKey.has_value() &&
                          playlist.encryptionKey->method == "AES-128";
        
        // 初期化セグメントは通常暗号化されていない（fMP4ヘッダーなど）
        // 明示的な暗号化指示がない限り、初期化セグメントは平文として扱う
        if (index == -1) {
            isEncrypted = false;
        }
        
        if (!data.empty()) {
            char buffer[4];
            std::string hex;
            for (size_t i = 0; i < std::min(data.size(), size_t(16)); ++i) {
                snprintf(buffer, sizeof(buffer), "%02X ", data[i]);
                hex += buffer;
            }
            if (index == -1) {
                LOG_DEBUG("Initialization Segment downloaded. Size: {} bytes, Encrypted: {}. Head: {}", 
                         data.size(), isEncrypted, hex);
            } else {
                LOG_DEBUG("Segment {} downloaded. Size: {} bytes, Encrypted: {}. Head: {}", 
                         index, data.size(), isEncrypted, hex);
            }
        }
        
        if (!cache->WriteSegment(index, std::move(data), isEncrypted)) {
            if (index == -1) {
                LOG_ERROR("Failed to write Initialization Segment to cache");
            } else {
                LOG_ERROR("Failed to write segment {} to cache", index);
            }
        } else {
            LOG_DEBUG("Segment {} cached successfully", index);
        }
    }
    
    /// @brief URLからベースURLを取得
    static std::string GetBaseUrl(const std::string& url) {
        size_t pos = url.rfind('/');
        if (pos != std::string::npos) {
            return url.substr(0, pos + 1);
        }
        return url;
    }
};

// =============================================================================
// コンストラクタ / デストラクタ
// =============================================================================

HlsSliceLoadingManager::HlsSliceLoadingManager()
    : m_impl(std::make_unique<Impl>()) {
}

HlsSliceLoadingManager::~HlsSliceLoadingManager() {
    Close();
}

// =============================================================================
// 初期化 / 終了
// =============================================================================

bool HlsSliceLoadingManager::Open(const std::string& hlsUrl, const HlsSliceConfig& config) {
    // 既存の状態を先にクローズ（ロック外で）
    Close();
    
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        
        // 空URLチェック
        if (hlsUrl.empty()) {
            LOG_ERROR("Empty HLS URL");
            return false;
        }
        
        // HLS URLチェック
        if (!IsHlsUrl(hlsUrl)) {
            LOG_ERROR("Not an HLS URL: {}", hlsUrl);
            return false;
        }
        
        m_impl->hlsUrl = hlsUrl;
        m_impl->config = config;
        
        LOG_INFO("Opening HLS URL: {}", hlsUrl);
        LOG_INFO("HTTP headers configured: {} entries", config.httpHeaders.size());
        for (const auto& [key, value] : config.httpHeaders) {
            LOG_DEBUG("  Header: {} = {}...", key, value.substr(0, std::min(value.length(), size_t(20))));
        }
        
        // 1. HTTPクライアントを初期化
        m_impl->httpClient = std::make_unique<io::HttpClient>();
        
        // 2. m3u8をダウンロード
        std::string m3u8Content;
        std::string currentUrl = hlsUrl;  // 現在処理中のURL（マスター→サブで変わる可能性あり）
        LOG_INFO("Downloading m3u8 from: {}", currentUrl);
        if (!m_impl->DownloadM3U8(currentUrl, m3u8Content)) {
            LOG_ERROR("Failed to download m3u8");
            return false;
        }
        LOG_INFO("M3U8 downloaded successfully, size: {} bytes", m3u8Content.size());
        
        // 2.5 マスタープレイリスト判定
        if (M3U8Parser::IsMasterPlaylist(m3u8Content)) {
            LOG_INFO("Detected master playlist, selecting best variant...");
            
            std::string baseUrl = Impl::GetBaseUrl(currentUrl);
            auto masterPlaylist = M3U8Parser::ParseMaster(m3u8Content, baseUrl);
            if (!masterPlaylist || masterPlaylist->variants.empty()) {
                LOG_ERROR("Failed to parse master playlist or no variants found");
                return false;
            }
            
            LOG_INFO("Master playlist contains {} variants", masterPlaylist->variants.size());
            
            // 最適なバリアントを選択（最高帯域幅）
            auto bestVariant = M3U8Parser::SelectBestVariant(*masterPlaylist);
            if (!bestVariant) {
                LOG_ERROR("Failed to select variant from master playlist");
                return false;
            }
            
            LOG_INFO("Selected variant: {}x{} @ {} bps, URL: {}",
                     bestVariant->width, bestVariant->height, bestVariant->bandwidth,
                     bestVariant->url.substr(0, std::min(bestVariant->url.length(), size_t(80))));
            
            // サブプレイリストを再取得
            currentUrl = bestVariant->url;
            m3u8Content.clear();
            if (!m_impl->DownloadM3U8(currentUrl, m3u8Content)) {
                LOG_ERROR("Failed to download sub-playlist from: {}", currentUrl);
                return false;
            }
            LOG_INFO("Sub-playlist downloaded successfully, size: {} bytes", m3u8Content.size());
        }
        
        // 3. メディアプレイリストをパース
        LOG_DEBUG("Parsing media playlist content...");
        std::string baseUrl = Impl::GetBaseUrl(currentUrl);
        auto parsedPlaylist = M3U8Parser::Parse(m3u8Content, baseUrl);
        if (!parsedPlaylist) {
            LOG_ERROR("Failed to parse media playlist");
            return false;
        }
        
        m_impl->playlist = *parsedPlaylist;
        
        // セグメントがない場合はエラー
        if (m_impl->playlist.segments.empty()) {
            LOG_ERROR("No segments in playlist");
            return false;
        }
        
        LOG_INFO("Parsed playlist: {} segments, total duration: {:.2f}s",
                 m_impl->playlist.segments.size(), m_impl->playlist.totalDuration);
    
        
        // 4. 暗号化キーがある場合はダウンロード
        if (m_impl->playlist.encryptionKey.has_value() &&
            m_impl->playlist.encryptionKey->method == "AES-128") {
            if (!m_impl->DownloadEncryptionKey(m_impl->playlist.encryptionKey->keyUrl)) {
                LOG_ERROR("Failed to download encryption key");
                return false;
            }
        }
        
        // 5. セグメントキャッシュを初期化
        HlsSegmentCacheConfig cacheConfig;
        cacheConfig.maxMemoryBytes = config.maxCacheMemory;
        cacheConfig.maxSegments = static_cast<size_t>(m_impl->playlist.segments.size());
        
        m_impl->cache = std::make_unique<HlsSegmentCache>(cacheConfig);
        m_impl->cache->Initialize(m_impl->playlist);
        
        // 暗号化キーを設定
        if (!m_impl->encryptionKeyData.empty()) {
           if (m_impl->playlist.encryptionKey.has_value() &&
            !m_impl->playlist.encryptionKey->keyUrl.empty()) {
            
            // 明示的なIVがあればログ出力
            if (!m_impl->playlist.encryptionKey->iv.empty()) {
                std::string ivHex;
                char buffer[4];
                for (uint8_t b : m_impl->playlist.encryptionKey->iv) {
                    snprintf(buffer, sizeof(buffer), "%02X ", b);
                    ivHex += buffer;
                }
                LOG_INFO("Explicit IV found in playlist: {}", ivHex);
                // SetEncryptionKeyにはIVも渡す
                m_impl->cache->SetEncryptionKey(m_impl->encryptionKeyData, m_impl->playlist.encryptionKey->iv);
            } else {
                // IVなし
                m_impl->cache->SetEncryptionKey(m_impl->encryptionKeyData, std::nullopt);
            }
            
            LOG_DEBUG("Encryption key set (with explicit IV: {})", !m_impl->playlist.encryptionKey->iv.empty());
        }
    }
        
        // 6. ChunkDownloaderを初期化（ダミーのSparseFileCacheを使用）
        m_impl->sparseCache = std::make_unique<io::SparseFileCache>();
        m_impl->sparseCache->Initialize(1024);  // 最小サイズで初期化
        
        m_impl->downloader = std::make_unique<io::ChunkDownloader>(
            m_impl->sparseCache.get(), config.maxConcurrentDownloads
        );
        m_impl->downloader->SetHttpHeaders(config.httpHeaders);
        

        
        // 初期化セグメントがある場合、最優先でダウンロード
        if (m_impl->playlist.map.has_value()) {
            LOG_INFO("Requesting Initialization Segment: {}", m_impl->playlist.map->url);
            
            // 優先度最高(0より小さい値、ここでは-1を特別扱いするか、単に最高優先度でキューに入れる)
            // ChunkDownloader実装ではpriorityが小さいほど優先度が高い仕様なら0より小さくすべき
            // ChunkDownloader::RequestSegmentはpriorityをintで受ける
            
            m_impl->downloader->RequestSegment(
                m_impl->playlist.map->url,
                -1,  // index -1
                io::ChunkPriority::Critical, // priority (Critical)
                m_impl->playlist.map->byteRangeStart,
                m_impl->playlist.map->byteRangeLength
            );
        }
        
        // セグメントダウンロードコールバックを設定
        m_impl->downloader->SetSegmentDownloadCallback(
            [this](int64_t index, std::vector<uint8_t>&& data, bool success) {
                m_impl->OnSegmentDownloaded(index, std::move(data), success);
            }
        );
        
        m_impl->downloader->Start();
        
        // 7. HlsCustomAVIOContextを初期化
        m_impl->avioContext = std::make_unique<HlsCustomAVIOContext>();
        if (!m_impl->avioContext->Initialize(m_impl->cache.get(), m_impl->playlist)) {
            LOG_ERROR("Failed to initialize HlsCustomAVIOContext");
            m_impl->downloader->Stop();
            return false;
        }
        
        m_impl->avioContext->SetReadTimeout(config.readTimeoutMs);
        
        // シークコールバックを設定（FFmpeg内部シーク用）
        m_impl->avioContext->SetOnSeekCallback([this](int64_t segmentIndex) {
            // このコールバックはAVIOContextのロック内から呼ばれるが、
            // Managerのロックは取得していない状態で呼ばれることを前提とする。
            // (Manager::Open -> ManagerLock -> AVIO::Open -> ... 完了後)
            // (FFmpeg -> AVIO::Seek -> Callback)
            
            // 無効なインデックスは無視
            if (segmentIndex < 0 || segmentIndex >= static_cast<int64_t>(m_impl->playlist.segments.size())) {
                return;
            }
            
            // 必要なセグメントを特定するためのロック
            bool needsRequest = false;
            std::string url;
            {
                std::lock_guard<std::mutex> lock(m_impl->mutex);
                if (!m_impl->isOpen || !m_impl->cache) return;
                
                if (!m_impl->cache->IsSegmentCached(segmentIndex)) {
                    const auto* info = m_impl->cache->GetSegmentInfo(segmentIndex);
                    if (info) {
                        url = info->url;
                        needsRequest = true;
                    }
                }
                
                // プリフェッチのために再生位置更新も行うが、これは非同期で行いたい
                // ここではとりあえず緊急のターゲットセグメントだけリクエストする
            }
            
            if (needsRequest && !url.empty()) {
                LOG_DEBUG("FFmpeg seek to segment {}, requesting download (Priority: Critical)", segmentIndex);
                m_impl->downloader->RequestSegment(url, segmentIndex, io::ChunkPriority::Critical);
                
                // 周辺セグメントのプリフェッチもトリガー（別スレッドでやると良いかもだが、ここでは簡易的に）
                // ただしUpdatePlaybackPositionはロックを取るので、ここから呼ぶとデッドロックのリスクがあるか確認が必要
                // UpdatePlaybackPositionは m_impl->mutex を取得する。
                // 現在のコンテキスト（Callback）は AVIOContext::Seek (AVIOLock保持) から来ている。
                // ManagerLockは保持していないはず。
                // -> Safe.
                
                // インデックスから時間への変換は概算で良い
                const auto& segments = m_impl->playlist.segments;
                double time = 0;
                for(int64_t i=0; i<segmentIndex && i < (int64_t)segments.size(); ++i) {
                    time += segments[i].duration;
                }
                UpdatePlaybackPosition(time);
            }
        });
        
        // 8. 先頭セグメントのダウンロードを開始し、完了を待機
        LOG_INFO("Requesting first segment with highest priority...");
        
        // isOpenをtrueに設定してからUpdatePlaybackPositionを呼ぶ必要がある
        m_impl->isOpen = true;
        
        // 最初のセグメントをCritical優先度でリクエスト
        if (!m_impl->playlist.segments.empty()) {
            const auto& firstSegment = m_impl->playlist.segments[0];
            LOG_INFO("First segment URL: {}...", firstSegment.url.substr(0, std::min(firstSegment.url.length(), size_t(80))));
            LOG_INFO("First segment duration: {:.2f}s", firstSegment.duration);
            m_impl->downloader->RequestSegment(firstSegment.url, 0, io::ChunkPriority::Critical);
            LOG_INFO("First segment request queued");
        }
        
        // 条件変数で待機（タイムアウトあり）
        // Note: WaitForSegment自体はスレッドセーフだが、ここでロックを持ったまま待機すると
        // 他のスレッド（があれば）をブロックしてしまう。
        // しかし現状ではこのロックはOpen専用に近いので許容範囲。
        // UpdatePlaybackPositionでの二重ロック回避のためにこのブロックを抜けることが重要。
        const int waitTimeoutMs = config.readTimeoutMs > 0 ? config.readTimeoutMs : 30000;
        auto waitStartTime = std::chrono::steady_clock::now();
        
        // fMP4の場合は初期化セグメント（index -1）も待機
        if (m_impl->playlist.map.has_value()) {
            LOG_INFO("Waiting for initialization segment (timeout: {}ms)...", waitTimeoutMs);
            if (!m_impl->cache->WaitForSegment(-1, waitTimeoutMs)) {
                auto waitEndTime = std::chrono::steady_clock::now();
                auto waitDuration = std::chrono::duration_cast<std::chrono::milliseconds>(waitEndTime - waitStartTime).count();
                LOG_ERROR("Initialization segment not available after {}ms wait", waitDuration);
                m_impl->isOpen = false;
                return false;
            } else {
                auto waitEndTime = std::chrono::steady_clock::now();
                auto waitDuration = std::chrono::duration_cast<std::chrono::milliseconds>(waitEndTime - waitStartTime).count();
                LOG_INFO("Initialization segment available after {}ms wait", waitDuration);
            }
        }
        
        LOG_INFO("Waiting for first segment to be available (timeout: {}ms)...", waitTimeoutMs);
        waitStartTime = std::chrono::steady_clock::now();  // 待機開始時刻をリセット
        
        if (!m_impl->cache->WaitForSegment(0, waitTimeoutMs)) {
            auto waitEndTime = std::chrono::steady_clock::now();
            auto waitDuration = std::chrono::duration_cast<std::chrono::milliseconds>(waitEndTime - waitStartTime).count();
            LOG_ERROR("First segment not available after {}ms wait (timeout was {}ms)", waitDuration, waitTimeoutMs);
            // タイムアウト時はエラーを返す（isOpen=falseにするなどクリーンアップ必要だが、デストラクタでCloseされる）
            m_impl->isOpen = false;
            return false;
        } else {
            auto waitEndTime = std::chrono::steady_clock::now();
            auto waitDuration = std::chrono::duration_cast<std::chrono::milliseconds>(waitEndTime - waitStartTime).count();
            LOG_INFO("First segment available after {}ms wait", waitDuration);
        }
        
        LOG_INFO("HLS stream opened successfully: {} segments", m_impl->playlist.segments.size());

    } // ★ここでロック解放 (UpdatePlaybackPositionでのデッドロック回避)
    
    // プリフェッチを開始（最初のセグメントがダウンロードされた後）
    // ロック外で呼び出す
    UpdatePlaybackPosition(0.0);
    
    return true;
}

void HlsSliceLoadingManager::Close() {
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    
    if (!m_impl->isOpen) {
        return;
    }
    
    LOG_DEBUG("Closing HlsSliceLoadingManager");
    
    // ダウンローダーを停止
    if (m_impl->downloader) {
        m_impl->downloader->Stop();
        m_impl->downloader.reset();
    }
    
    // AVIOContextをクローズ
    if (m_impl->avioContext) {
        m_impl->avioContext->Close();
        m_impl->avioContext.reset();
    }
    
    // キャッシュをクリア
    m_impl->cache.reset();
    m_impl->sparseCache.reset();
    
    // HTTPクライアントをクリア
    m_impl->httpClient.reset();
    
    // 状態をリセット
    m_impl->playlist = M3U8Playlist{};
    m_impl->hlsUrl.clear();
    m_impl->encryptionKeyData.clear();
    m_impl->bandwidth = 0.0;
    
    m_impl->isOpen = false;
    
    LOG_DEBUG("HlsSliceLoadingManager closed");
}

bool HlsSliceLoadingManager::IsOpen() const {
    return m_impl->isOpen;
}

// =============================================================================
// FFmpeg統合
// =============================================================================

AVIOContext* HlsSliceLoadingManager::GetAVIOContext() const {
    if (!m_impl->isOpen || !m_impl->avioContext) {
        return nullptr;
    }
    return m_impl->avioContext->GetAVIOContext();
}

// =============================================================================
// URL判定
// =============================================================================

bool HlsSliceLoadingManager::IsHlsUrl(const std::string& url) {
    if (url.empty()) {
        return false;
    }
    
    // 小文字に変換して比較
    std::string lower = Impl::ToLower(url);
    
    // .m3u8 または .m3u が含まれるか
    if (lower.find(".m3u8") != std::string::npos) {
        return true;
    }
    if (lower.find(".m3u") != std::string::npos) {
        return true;
    }
    
    return false;
}

// =============================================================================
// 再生情報
// =============================================================================

double HlsSliceLoadingManager::GetDuration() const {
    if (!m_impl->isOpen) {
        return 0.0;
    }
    return m_impl->playlist.totalDuration;
}

// =============================================================================
// 統計情報
// =============================================================================

double HlsSliceLoadingManager::GetDownloadProgress() const {
    if (!m_impl->isOpen || !m_impl->cache) {
        return 0.0;
    }
    
    size_t total = m_impl->cache->GetTotalSegmentCount();
    if (total == 0) {
        return 0.0;
    }
    
    size_t cached = m_impl->cache->GetCachedSegmentCount();
    return static_cast<double>(cached) / static_cast<double>(total);
}

double HlsSliceLoadingManager::GetBandwidth() const {
    if (!m_impl->isOpen || !m_impl->downloader) {
        return 0.0;
    }
    return m_impl->downloader->GetBandwidth();
}

bool HlsSliceLoadingManager::IsFullyCached() const {
    if (!m_impl->isOpen || !m_impl->cache) {
        return false;
    }
    
    return m_impl->cache->GetCachedSegmentCount() == 
           m_impl->cache->GetTotalSegmentCount();
}

size_t HlsSliceLoadingManager::GetCachedSegmentCount() const {
    if (!m_impl->isOpen || !m_impl->cache) {
        return 0;
    }
    return m_impl->cache->GetCachedSegmentCount();
}

size_t HlsSliceLoadingManager::GetTotalSegmentCount() const {
    if (!m_impl->isOpen || !m_impl->cache) {
        return 0;
    }
    return m_impl->cache->GetTotalSegmentCount();
}

// =============================================================================
// 再生位置連携
// =============================================================================

void HlsSliceLoadingManager::UpdatePlaybackPosition(double seconds) {
    if (!m_impl->isOpen || !m_impl->cache || !m_impl->downloader) {
        return;
    }
    
    // リクエスト対象のセグメント情報を収集（ロック内）
    std::vector<std::tuple<std::string, int64_t, io::ChunkPriority>> segmentsToRequest;
    int64_t currentSegment;
    int prefetchAhead;
    
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        
        currentSegment = m_impl->cache->GetSegmentIndexFromTime(seconds);
        int64_t totalSegments = static_cast<int64_t>(m_impl->playlist.segments.size());
        prefetchAhead = m_impl->config.prefetchSegmentsAhead;
        
        // 現在位置から prefetchSegmentsAhead 個先までリクエスト対象を収集
        for (int64_t i = currentSegment; 
             i < currentSegment + prefetchAhead && i < totalSegments; 
             ++i) {
            if (i < 0) continue;
            
            if (!m_impl->cache->IsSegmentCached(i)) {
                // 優先度を設定
                io::ChunkPriority priority;
                if (i == currentSegment) {
                    priority = io::ChunkPriority::Critical;
                } else if (i == currentSegment + 1) {
                    priority = io::ChunkPriority::High;
                } else {
                    priority = io::ChunkPriority::Medium;
                }
                
                const auto* segmentInfo = m_impl->cache->GetSegmentInfo(i);
                if (segmentInfo) {
                    segmentsToRequest.emplace_back(segmentInfo->url, i, priority);
                }
            }
        }
    }
    
    // ロック外でリクエストを発行
    for (const auto& [url, index, priority] : segmentsToRequest) {
        m_impl->downloader->RequestSegment(url, index, priority);
    }
    
    // ロック外でキャッシュ最適化
    m_impl->cache->OptimizeForPlayback(currentSegment, prefetchAhead);
}

void HlsSliceLoadingManager::NotifySeek(double seconds) {
    if (!m_impl->isOpen || !m_impl->cache || !m_impl->downloader) {
        return;
    }
    
    LOG_DEBUG("Seek to {:.2f}s", seconds);
    
    // 既存キューをクリア（ロック外で）
    m_impl->downloader->ClearSegmentQueue();
    
    // リクエスト対象のセグメント情報を収集（ロック内）
    std::vector<std::tuple<std::string, int64_t, io::ChunkPriority>> segmentsToRequest;
    int64_t targetSegment;
    int prefetchAhead;
    HlsCustomAVIOContext* avioContext = nullptr;
    
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        
        targetSegment = m_impl->cache->GetSegmentIndexFromTime(seconds);
        int64_t totalSegments = static_cast<int64_t>(m_impl->playlist.segments.size());
        prefetchAhead = m_impl->config.prefetchSegmentsAhead;
        avioContext = m_impl->avioContext.get();
        
        // シーク先から優先ダウンロード対象を収集
        for (int64_t i = targetSegment;
             i < targetSegment + static_cast<int64_t>(prefetchAhead) && i < totalSegments;
             ++i) {
            if (i < 0) continue;
            
            if (!m_impl->cache->IsSegmentCached(i)) {
                io::ChunkPriority priority;
                if (i == targetSegment) {
                    priority = io::ChunkPriority::Critical;
                } else if (i == targetSegment + 1) {
                    priority = io::ChunkPriority::High;
                } else {
                    priority = io::ChunkPriority::Medium;
                }
                
                const auto* segmentInfo = m_impl->cache->GetSegmentInfo(i);
                if (segmentInfo) {
                    segmentsToRequest.emplace_back(segmentInfo->url, i, priority);
                }
            }
        }
    }
    
    // ロック外でリクエストを発行
    for (const auto& [url, index, priority] : segmentsToRequest) {
        m_impl->downloader->RequestSegment(url, index, priority);
    }
    
    // ロック外でキャッシュ最適化
    m_impl->cache->OptimizeForPlayback(targetSegment, prefetchAhead);
    
    // ロック外でAVIOにシークを通知
    if (avioContext) {
        avioContext->SeekToTime(seconds);
    }
}

}  // namespace hls
}  // namespace ytdlpspout
