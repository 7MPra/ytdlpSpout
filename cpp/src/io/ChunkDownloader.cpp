// =============================================================================
// ChunkDownloader.cpp - チャンク並列ダウンローダー 実装
// =============================================================================

#include "io/ChunkDownloader.h"
#include "io/SparseFileCache.h"
#include "io/HttpClient.h"
#include "utils/Logger.h"

using Logger = ytdlpspout::Logger;

#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <set>
#include <atomic>
#include <chrono>
#include <deque>

namespace ytdlpspout {
namespace io {

// =============================================================================
// 内部実装クラス
// =============================================================================

/// @brief ダウンロードリクエスト（内部用）
struct ChunkRequest {
    int64_t offset = 0;           // バイトオフセット
    ChunkPriority priority = ChunkPriority::Medium;
    int64_t requestTime = 0;      // リクエスト時刻（タイブレーカー用）
    int attempt = 0;              // これまでの試行回数（0=未試行、失敗のたび加算）

    /// @brief 優先度キュー用の比較演算子
    bool operator<(const ChunkRequest& other) const {
        // priority_queueはmax-heapなので、小さい値を優先するには逆にする
        if (priority != other.priority) {
            return static_cast<int>(priority) > static_cast<int>(other.priority);
        }
        // 同じ優先度なら早いリクエストを優先
        return requestTime > other.requestTime;
    }
};

/// @brief HLSセグメントリクエスト（内部用）
struct SegmentRequest {
    std::string url;
    int64_t segmentIndex = 0;
    ChunkPriority priority = ChunkPriority::Medium;
    std::chrono::steady_clock::time_point requestTime;
    int64_t byteRangeStart = -1;
    int64_t byteRangeLength = 0;
    int attempt = 0;              // これまでの試行回数（0=未試行、失敗のたび加算）

    /// @brief 優先度キュー用の比較演算子
    bool operator<(const SegmentRequest& other) const {
        // priority_queueはmax-heapなので、小さい値を優先するには逆にする
        if (priority != other.priority) {
            return static_cast<int>(priority) > static_cast<int>(other.priority);
        }
        // 同じ優先度なら早いリクエストを優先
        return requestTime > other.requestTime;
    }
};

/// @brief 帯域幅サンプル
struct BandwidthSample {
    int64_t bytes = 0;
    double durationMs = 0.0;
};

struct ChunkDownloader::Impl {
    // -------------------------------------------------------------------------
    // 設定
    // -------------------------------------------------------------------------
    SparseFileCache* cache = nullptr;
    int numWorkers = 4;
    std::string url;
    std::map<std::string, std::string> headers;
    
    // -------------------------------------------------------------------------
    // スレッド管理
    // -------------------------------------------------------------------------
    std::vector<std::thread> workers;
    std::atomic<bool> running{false};
    std::atomic<bool> stopRequested{false};
    
    // -------------------------------------------------------------------------
    // リクエストキュー
    // -------------------------------------------------------------------------
    std::priority_queue<ChunkRequest> requestQueue;
    std::set<int64_t> pendingOffsets;       // キューに入っているオフセット
    std::set<int64_t> activeOffsets;        // 現在ダウンロード中のオフセット
    std::set<int64_t> cancelledOffsets;     // キャンセルされたオフセット
    mutable std::mutex queueMutex;
    std::condition_variable queueCv;
    
    // -------------------------------------------------------------------------
    // セグメントリクエストキュー（HLS用）
    // -------------------------------------------------------------------------
    // queueMutexで保護する（requestQueueと別のmutexにすると、pushとnotifyの間に
    // 待機述語評価が挟まるタイミングで通知ロストが発生しうるため単一mutex化した）
    std::priority_queue<SegmentRequest> segmentQueue;
    std::set<int64_t> pendingSegments;      // キューに入っているセグメントインデックス
    std::set<int64_t> activeSegments;       // 現在ダウンロード中のセグメント
    SegmentDownloadCallback segmentCallback;

    // -------------------------------------------------------------------------
    // 帯域幅推定
    // -------------------------------------------------------------------------
    std::deque<BandwidthSample> bandwidthSamples;
    static constexpr size_t MAX_BANDWIDTH_SAMPLES = 10;
    mutable std::mutex bandwidthMutex;
    std::atomic<double> currentBandwidth{0.0};

    // -------------------------------------------------------------------------
    // リトライ制御
    // -------------------------------------------------------------------------
    // ChunkDownloader内部での最大試行回数（HttpClient内のリクエスト単位リトライとは別。
    // それでも失敗し続けるリクエストを永久に放置しないための上位の回復手段）
    static constexpr int MAX_DOWNLOAD_ATTEMPTS = 3;
    
    // -------------------------------------------------------------------------
    // 統計カウンター
    // -------------------------------------------------------------------------
    int64_t requestCounter = 0;  // タイブレーカー用のカウンター
    
    // -------------------------------------------------------------------------
    // ヘルパー関数
    // -------------------------------------------------------------------------
    
    /// @brief 現在時刻をミリ秒で取得
    static int64_t GetCurrentTimeMs() {
        auto now = std::chrono::steady_clock::now();
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()).count();
    }
    
    /// @brief ワーカースレッド関数
    void WorkerThread(int workerId) {
        Logger::Debug("ChunkDownloader worker {} started", workerId);
        
        // 各ワーカーに専用のHttpClientを作成
        // headersはSetHeaders/SetHttpHeadersの書き込みと競合しうるため、
        // queueMutex下でローカルにコピーしてから使う（C-3）
        std::map<std::string, std::string> initialHeaders;
        {
            std::lock_guard<std::mutex> lock(queueMutex);
            initialHeaders = headers;
        }
        HttpClientConfig clientConfig;
        clientConfig.headers = initialHeaders;
        clientConfig.connectTimeoutMs = 10000;
        clientConfig.readTimeoutMs = 30000;
        HttpClient httpClient(clientConfig);
        
        while (!stopRequested.load()) {
            ChunkRequest chunkRequest;
            SegmentRequest segmentRequest;
            bool hasChunkRequest = false;
            bool hasSegmentRequest = false;
            
            // リクエストを取得（チャンクまたはセグメント）
            // requestQueueとsegmentQueueを単一のqueueMutexで保護する。
            // 別mutexにすると「述語評価」と「push+notify」がずれるタイミングで
            // 通知ロストが発生しうるため（C-2）、両キューとも同じmutex/CVを使う。
            {
                std::unique_lock<std::mutex> lock(queueMutex);
                queueCv.wait(lock, [this] {
                    return stopRequested.load() ||
                           !requestQueue.empty() ||
                           !segmentQueue.empty();
                });

                if (stopRequested.load()) {
                    break;
                }

                // セグメントとチャンクの優先度を比較
                bool segmentHigherPriority = false;
                if (!segmentQueue.empty() && !requestQueue.empty()) {
                    // 両方にリクエストがある場合、優先度を比較
                    segmentHigherPriority =
                        static_cast<int>(segmentQueue.top().priority) <
                        static_cast<int>(requestQueue.top().priority);
                } else if (!segmentQueue.empty()) {
                    segmentHigherPriority = true;
                }

                // セグメントリクエストを優先的に処理
                if (segmentHigherPriority) {
                    segmentRequest = segmentQueue.top();
                    segmentQueue.pop();
                    pendingSegments.erase(segmentRequest.segmentIndex);
                    activeSegments.insert(segmentRequest.segmentIndex);
                    hasSegmentRequest = true;
                }

                // チャンクリクエストを処理
                if (!hasSegmentRequest && !requestQueue.empty()) {
                    chunkRequest = requestQueue.top();
                    requestQueue.pop();
                    pendingOffsets.erase(chunkRequest.offset);

                    // キャンセルされていたらスキップ
                    if (cancelledOffsets.count(chunkRequest.offset) > 0) {
                        cancelledOffsets.erase(chunkRequest.offset);
                        continue;
                    }

                    activeOffsets.insert(chunkRequest.offset);
                    hasChunkRequest = true;
                }
            }

            // セグメントダウンロード実行
            if (hasSegmentRequest) {
                std::vector<uint8_t> segmentData;
                bool success = DownloadSegment(httpClient, segmentRequest, segmentData);

                bool requeued = false;
                {
                    std::lock_guard<std::mutex> lock(queueMutex);
                    activeSegments.erase(segmentRequest.segmentIndex);

                    // 失敗時はattempt上限に達するまで優先度を維持したまま再キューする（C-1）。
                    // HttpClient側で既にバックオフ＋リトライ済みのため、即時再キューでよい。
                    if (!success && segmentRequest.attempt + 1 < MAX_DOWNLOAD_ATTEMPTS) {
                        SegmentRequest retryRequest = segmentRequest;
                        retryRequest.attempt++;
                        retryRequest.requestTime = std::chrono::steady_clock::now();
                        segmentQueue.push(retryRequest);
                        pendingSegments.insert(retryRequest.segmentIndex);
                        requeued = true;
                    }
                }

                if (requeued) {
                    Logger::Warn("ChunkDownloader: Segment {} download failed, requeued (attempt {}/{})",
                                 segmentRequest.segmentIndex, segmentRequest.attempt + 2, MAX_DOWNLOAD_ATTEMPTS);
                    queueCv.notify_one();
                } else {
                    // 成功、またはattempt上限に達した最終失敗のみコールバックに通知する（現行互換）
                    if (!success) {
                        Logger::Warn("ChunkDownloader: Segment {} download failed permanently after {} attempts",
                                     segmentRequest.segmentIndex, MAX_DOWNLOAD_ATTEMPTS);
                    }
                    SegmentDownloadCallback cb;
                    {
                        std::lock_guard<std::mutex> lock(queueMutex);
                        cb = segmentCallback;
                    }
                    if (cb) {
                        cb(segmentRequest.segmentIndex, std::move(segmentData), success);
                    }
                }
                continue;
            }

            // チャンクダウンロード実行
            if (hasChunkRequest) {
                bool success = DownloadChunk(httpClient, chunkRequest.offset);

                bool requeued = false;
                {
                    std::lock_guard<std::mutex> lock(queueMutex);
                    activeOffsets.erase(chunkRequest.offset);

                    // 失敗時はattempt上限に達するまで優先度を維持したまま再キューする（C-1）
                    if (!success && chunkRequest.attempt + 1 < MAX_DOWNLOAD_ATTEMPTS) {
                        ChunkRequest retryRequest = chunkRequest;
                        retryRequest.attempt++;
                        requestQueue.push(retryRequest);
                        pendingOffsets.insert(retryRequest.offset);
                        requeued = true;
                    }
                }

                if (requeued) {
                    // ChunkState::Errorのままだと恒久的失敗に見えてしまうため、
                    // 再試行待ちであることが分かるようPendingに戻す
                    if (cache) {
                        int64_t chunkIndex = cache->GetChunkIndex(chunkRequest.offset);
                        cache->SetChunkState(chunkIndex, ChunkState::Pending);
                    }
                    Logger::Warn("ChunkDownloader: Chunk at offset {} failed, requeued (attempt {}/{})",
                                chunkRequest.offset, chunkRequest.attempt + 2, MAX_DOWNLOAD_ATTEMPTS);
                    queueCv.notify_one();
                } else if (!success) {
                    Logger::Warn("ChunkDownloader: Failed to download chunk at offset {} permanently after {} attempts",
                                        chunkRequest.offset, MAX_DOWNLOAD_ATTEMPTS);
                }
            }
        }

        Logger::Debug("ChunkDownloader worker {} stopped", workerId);
    }
    
    /// @brief チャンクをダウンロード
    /// @param client HttpClient
    /// @param offset バイトオフセット
    /// @return 成功した場合true
    bool DownloadChunk(HttpClient& client, int64_t offset) {
        if (!cache) {
            return false;
        }

        // url/headersはSetUrl/SetHeadersの書き込みと競合しうるため、
        // queueMutex下でローカルにコピーしてから使う（C-3）。
        // ネットワークI/O中はmutexを保持しない。
        std::string urlCopy;
        std::map<std::string, std::string> headersCopy;
        {
            std::lock_guard<std::mutex> lock(queueMutex);
            urlCopy = url;
            headersCopy = headers;
        }

        if (urlCopy.empty()) {
            return false;
        }

        // HTTPヘッダーを設定（SetHeaders()で更新された最新の値を使用）
        if (!headersCopy.empty()) {
            HttpClientConfig config;
            config.headers = headersCopy;
            config.connectTimeoutMs = 10000;
            config.readTimeoutMs = 30000;
            client.Configure(config);
        }

        // チャンクインデックスとサイズを計算
        int64_t chunkIndex = cache->GetChunkIndex(offset);
        size_t chunkSize = cache->GetActualChunkSize(chunkIndex);
        int64_t byteOffset = cache->GetByteOffset(chunkIndex);
        int64_t endByte = byteOffset + static_cast<int64_t>(chunkSize) - 1;
        
        // 既にキャッシュ済みの場合はスキップ
        if (cache->GetChunkState(chunkIndex) == ChunkState::Cached) {
            Logger::Debug("ChunkDownloader: Chunk {} already cached", chunkIndex);
            return true;
        }
        
        // ダウンロード中状態に設定
        cache->SetChunkState(chunkIndex, ChunkState::Downloading);
        
        Logger::Debug("ChunkDownloader: Downloading chunk {} (bytes {}-{})",
                            chunkIndex, byteOffset, endByte);
        
        // ダウンロード時間計測開始
        auto startTime = std::chrono::steady_clock::now();
        
        // Range Requestでダウンロード
        HttpResponse response = client.GetRange(urlCopy, byteOffset, endByte);
        
        // ダウンロード時間計測終了
        auto endTime = std::chrono::steady_clock::now();
        double durationMs = std::chrono::duration<double, std::milli>(
            endTime - startTime).count();
        
        if (!response.success || response.data.empty()) {
            Logger::Warn("ChunkDownloader: HTTP request failed for chunk {} (status {})",
                               chunkIndex, response.statusCode);
            cache->SetChunkState(chunkIndex, ChunkState::Error);
            return false;
        }
        
        // 帯域幅サンプルを記録
        {
            std::lock_guard<std::mutex> lock(bandwidthMutex);
            bandwidthSamples.push_back({
                static_cast<int64_t>(response.data.size()),
                durationMs
            });
            if (bandwidthSamples.size() > MAX_BANDWIDTH_SAMPLES) {
                bandwidthSamples.pop_front();
            }
            
            // 帯域幅を更新
            UpdateBandwidth();
        }
        
        // キャッシュに書き込み
        if (!cache->WriteChunk(chunkIndex, response.data.data(), response.data.size())) {
            Logger::Warn("ChunkDownloader: Failed to write chunk {} to cache",
                               chunkIndex);
            cache->SetChunkState(chunkIndex, ChunkState::Error);
            return false;
        }
        
        Logger::Debug("ChunkDownloader: Chunk {} downloaded ({} bytes)",
                            chunkIndex, response.data.size());
        
        return true;
    }
    
    /// @brief 帯域幅を更新（bandwidthMutexロック下で呼び出し）
    void UpdateBandwidth() {
        if (bandwidthSamples.empty()) {
            currentBandwidth.store(0.0);
            return;
        }
        
        double totalBytes = 0.0;
        double totalMs = 0.0;
        
        for (const auto& sample : bandwidthSamples) {
            totalBytes += static_cast<double>(sample.bytes);
            totalMs += sample.durationMs;
        }
        
        if (totalMs > 0.0) {
            // バイト/秒に変換
            currentBandwidth.store((totalBytes / totalMs) * 1000.0);
        }
    }
    
    /// @brief HLSセグメントをダウンロード
    /// @param client HttpClient
    /// @param request セグメントリクエスト
    /// @param outData ダウンロードしたデータ（成功時のみ有効な内容が入る）
    /// @return 成功した場合true
    bool DownloadSegment(HttpClient& client, const SegmentRequest& request, std::vector<uint8_t>& outData) {
        Logger::Debug("ChunkDownloader: Starting download of segment {} from {}...",
                            request.segmentIndex, request.url.substr(0, std::min(request.url.length(), size_t(80))));

        // headersはSetHeaders/SetHttpHeadersの書き込みと競合しうるため、
        // queueMutex下でローカルにコピーしてから使う（C-3）
        std::map<std::string, std::string> headersCopy;
        {
            std::lock_guard<std::mutex> lock(queueMutex);
            headersCopy = headers;
        }

        // HTTPヘッダーを設定（SetHttpHeaders()で更新された最新の値を使用）
        Logger::Trace("ChunkDownloader: Configuring HTTP headers for segment {} ({} headers available)",
                     request.segmentIndex, headersCopy.size());
        if (!headersCopy.empty()) {
            HttpClientConfig config;
            config.headers = headersCopy;
            config.connectTimeoutMs = 30000;
            config.readTimeoutMs = 60000;
            client.Configure(config);
            Logger::Debug("ChunkDownloader: Configured HTTP headers for segment {} ({} headers)",
                          request.segmentIndex, headersCopy.size());
        } else {
            Logger::Warn("ChunkDownloader: No HTTP headers available for segment {}", request.segmentIndex);
        }

        // ダウンロード時間計測開始
        auto startTime = std::chrono::steady_clock::now();
        Logger::Debug("ChunkDownloader: Sending HTTP request for segment {} (Range: {}-{})", 
                     request.segmentIndex, request.byteRangeStart, 
                     request.byteRangeLength > 0 ? request.byteRangeStart + request.byteRangeLength - 1 : -1);
        
        HttpResponse response;
        if (request.byteRangeLength > 0 && request.byteRangeStart >= 0) {
            // Range Request
            response = client.GetRange(request.url, request.byteRangeStart, 
                                     request.byteRangeStart + request.byteRangeLength - 1);
        } else {
            // セグメント全体をダウンロード
            response = client.Get(request.url);
        }
        
        // ダウンロード時間計測終了
        auto endTime = std::chrono::steady_clock::now();
        double durationMs = std::chrono::duration<double, std::milli>(
            endTime - startTime).count();
        
        Logger::Debug("ChunkDownloader: HTTP response for segment {}: status={}, success={}, size={}, duration={:.0f}ms",
                     request.segmentIndex, response.statusCode, response.success, response.data.size(), durationMs);
        
        bool success = response.success && !response.data.empty();
        
        if (!success) {
            Logger::Error("ChunkDownloader: FAILED to download segment {} - status={}, success={}, errorMessage={}",
                          request.segmentIndex, response.statusCode, response.success, response.errorMessage);
        }
        
        if (success) {
            // 帯域幅サンプルを記録
            {
                std::lock_guard<std::mutex> lock(bandwidthMutex);
                bandwidthSamples.push_back({
                    static_cast<int64_t>(response.data.size()),
                    durationMs
                });
                if (bandwidthSamples.size() > MAX_BANDWIDTH_SAMPLES) {
                    bandwidthSamples.pop_front();
                }
                UpdateBandwidth();
            }
            
            Logger::Debug("ChunkDownloader: Segment {} downloaded successfully ({} bytes in {:.0f}ms, {:.1f} KB/s)",
                                request.segmentIndex, response.data.size(), durationMs,
                                (response.data.size() / durationMs) * 1000.0 / 1024.0);
        } else {
            Logger::Error("ChunkDownloader: Failed to download segment {} (status {}): {}",
                               request.segmentIndex, response.statusCode, response.errorMessage);
        }

        // コールバックの呼び出しは呼び出し元（WorkerThread）でattempt上限判定後に行う（C-1）
        outData = std::move(response.data);
        return success;
    }
};

// =============================================================================
// コンストラクタ/デストラクタ
// =============================================================================

ChunkDownloader::ChunkDownloader(SparseFileCache* cache, int numWorkers)
    : m_impl(std::make_unique<Impl>())
{
    if (!cache) {
        throw std::invalid_argument("ChunkDownloader: cache cannot be null");
    }
    
    m_impl->cache = cache;
    m_impl->numWorkers = std::max(1, numWorkers);  // 最低1ワーカー
    
    Logger::Debug("ChunkDownloader created with {} workers", m_impl->numWorkers);
}

ChunkDownloader::~ChunkDownloader() {
    Stop();
    Logger::Debug("ChunkDownloader destroyed");
}

// =============================================================================
// 設定
// =============================================================================

void ChunkDownloader::SetUrl(const std::string& url) {
    std::lock_guard<std::mutex> lock(m_impl->queueMutex);
    m_impl->url = url;
    Logger::Debug("ChunkDownloader: URL set to {}", url);
}

void ChunkDownloader::SetHeaders(const std::map<std::string, std::string>& headers) {
    std::lock_guard<std::mutex> lock(m_impl->queueMutex);
    m_impl->headers = headers;
    Logger::Debug("ChunkDownloader: Headers updated ({} entries)", headers.size());
}

void ChunkDownloader::SetHttpHeaders(const std::map<std::string, std::string>& headers) {
    // SetHeadersと同じ実装（HLS用のエイリアス）
    SetHeaders(headers);
}

// =============================================================================
// ダウンロード制御
// =============================================================================

void ChunkDownloader::Start() {
    if (m_impl->running.load()) {
        return;  // 既に実行中
    }
    
    Logger::Info("ChunkDownloader: Starting {} worker threads", m_impl->numWorkers);
    
    m_impl->stopRequested.store(false);
    m_impl->running.store(true);
    
    // ワーカースレッドを起動
    for (int i = 0; i < m_impl->numWorkers; ++i) {
        m_impl->workers.emplace_back(&Impl::WorkerThread, m_impl.get(), i);
    }
}

void ChunkDownloader::Stop() {
    if (!m_impl->running.load()) {
        return;  // 既に停止
    }
    
    Logger::Info("ChunkDownloader: Stopping worker threads");
    
    m_impl->stopRequested.store(true);
    m_impl->queueCv.notify_all();
    
    // すべてのワーカースレッドが終了するのを待つ
    for (auto& worker : m_impl->workers) {
        if (worker.joinable()) {
            worker.join();
        }
    }
    m_impl->workers.clear();
    
    // キューをクリア（チャンク/セグメントとも同一queueMutexで保護されている）
    {
        std::lock_guard<std::mutex> lock(m_impl->queueMutex);
        while (!m_impl->requestQueue.empty()) {
            m_impl->requestQueue.pop();
        }
        m_impl->pendingOffsets.clear();
        m_impl->activeOffsets.clear();
        m_impl->cancelledOffsets.clear();

        while (!m_impl->segmentQueue.empty()) {
            m_impl->segmentQueue.pop();
        }
        m_impl->pendingSegments.clear();
        m_impl->activeSegments.clear();
    }

    m_impl->running.store(false);
    Logger::Info("ChunkDownloader: All workers stopped");
}

void ChunkDownloader::RequestChunk(int64_t offset, ChunkPriority priority) {
    {
        std::lock_guard<std::mutex> lock(m_impl->queueMutex);

        // 既にキューにあるか、ダウンロード中の場合はスキップ
        if (m_impl->pendingOffsets.count(offset) > 0 ||
            m_impl->activeOffsets.count(offset) > 0) {
            return;
        }

        // キャンセルリストから削除（再リクエスト）
        m_impl->cancelledOffsets.erase(offset);

        ChunkRequest request;
        request.offset = offset;
        request.priority = priority;
        request.requestTime = m_impl->requestCounter++;

        m_impl->requestQueue.push(request);
        m_impl->pendingOffsets.insert(offset);
    }

    // attempt上限に達してChunkState::Errorのまま放置されていたチャンクも、
    // 再リクエストされたらダウンロード待ちとして受け付ける（C-1）
    if (m_impl->cache) {
        int64_t chunkIndex = m_impl->cache->GetChunkIndex(offset);
        if (m_impl->cache->GetChunkState(chunkIndex) == ChunkState::Error) {
            m_impl->cache->SetChunkState(chunkIndex, ChunkState::Pending);
        }
    }

    Logger::Debug("ChunkDownloader: Requested chunk at offset {} (priority {})",
                        offset, static_cast<int>(priority));

    m_impl->queueCv.notify_one();
}

void ChunkDownloader::CancelChunk(int64_t offset) {
    std::lock_guard<std::mutex> lock(m_impl->queueMutex);
    
    // キューにある場合はキャンセルリストに追加
    if (m_impl->pendingOffsets.count(offset) > 0) {
        m_impl->cancelledOffsets.insert(offset);
        m_impl->pendingOffsets.erase(offset);
        Logger::Debug("ChunkDownloader: Cancelled chunk at offset {}", offset);
    }
}

// =============================================================================
// 統計
// =============================================================================

double ChunkDownloader::GetBandwidth() const {
    return m_impl->currentBandwidth.load();
}

bool ChunkDownloader::IsRunning() const {
    return m_impl->running.load();
}

// =============================================================================
// HLSセグメントダウンロード
// =============================================================================

void ChunkDownloader::SetSegmentDownloadCallback(SegmentDownloadCallback callback) {
    std::lock_guard<std::mutex> lock(m_impl->queueMutex);
    m_impl->segmentCallback = std::move(callback);
    Logger::Debug("ChunkDownloader: Segment download callback set");
}

void ChunkDownloader::RequestSegment(const std::string& url, int64_t segmentIndex, ChunkPriority priority,
                                     int64_t byteRangeStart, int64_t byteRangeLength) {
    if (url.empty()) {
        return;
    }

    std::lock_guard<std::mutex> lock(m_impl->queueMutex);

    // 既に完了済み、または処理中の場合はスキップするか検討
    // -> HLSの場合はリトライなどで再リクエストされることがあるので許可する
    
    SegmentRequest request;
    request.url = url;
    request.segmentIndex = segmentIndex;
    request.priority = priority;
    request.requestTime = std::chrono::steady_clock::now();
    request.byteRangeStart = byteRangeStart;
    request.byteRangeLength = byteRangeLength;
    
    m_impl->segmentQueue.push(request);
    m_impl->pendingSegments.insert(segmentIndex);
    
    Logger::Debug("ChunkDownloader: Requested segment {} (priority {}, range {}-{})",
                  segmentIndex, static_cast<int>(priority), byteRangeStart, byteRangeLength);
    
    m_impl->queueCv.notify_one();
}

void ChunkDownloader::ReprioritizeSegment(int64_t segmentIndex, ChunkPriority newPriority) {
    std::lock_guard<std::mutex> lock(m_impl->queueMutex);
    
    // キューに入っている場合のみ変更可能（現在ダウンロード中のものは変更できない）
    if (m_impl->pendingSegments.count(segmentIndex) == 0) {
        return;
    }
    
    // priority_queueから全要素を取り出して再構築
    std::vector<SegmentRequest> requests;
    while (!m_impl->segmentQueue.empty()) {
        SegmentRequest req = m_impl->segmentQueue.top();
        m_impl->segmentQueue.pop();
        
        if (req.segmentIndex == segmentIndex) {
            req.priority = newPriority;
        }
        requests.push_back(req);
    }
    
    // 再構築
    for (auto& req : requests) {
        m_impl->segmentQueue.push(req);
    }
    
    Logger::Debug("ChunkDownloader: Reprioritized segment {} to priority {}",
                        segmentIndex, static_cast<int>(newPriority));
}

void ChunkDownloader::ClearSegmentQueue() {
    std::lock_guard<std::mutex> lock(m_impl->queueMutex);
    
    while (!m_impl->segmentQueue.empty()) {
        m_impl->segmentQueue.pop();
    }
    m_impl->pendingSegments.clear();
    // activeSegmentsはクリアしない（現在ダウンロード中のものは継続）
    
    Logger::Debug("ChunkDownloader: Segment queue cleared");
}

} // namespace io
} // namespace ytdlpspout

