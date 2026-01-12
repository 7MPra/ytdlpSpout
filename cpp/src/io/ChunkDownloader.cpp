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
    // 帯域幅推定
    // -------------------------------------------------------------------------
    std::deque<BandwidthSample> bandwidthSamples;
    static constexpr size_t MAX_BANDWIDTH_SAMPLES = 10;
    mutable std::mutex bandwidthMutex;
    std::atomic<double> currentBandwidth{0.0};
    
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
        HttpClientConfig clientConfig;
        clientConfig.headers = headers;
        clientConfig.connectTimeoutMs = 10000;
        clientConfig.readTimeoutMs = 30000;
        HttpClient httpClient(clientConfig);
        
        while (!stopRequested.load()) {
            ChunkRequest request;
            
            // リクエストを取得
            {
                std::unique_lock<std::mutex> lock(queueMutex);
                queueCv.wait(lock, [this] {
                    return stopRequested.load() || !requestQueue.empty();
                });
                
                if (stopRequested.load()) {
                    break;
                }
                
                if (requestQueue.empty()) {
                    continue;
                }
                
                request = requestQueue.top();
                requestQueue.pop();
                pendingOffsets.erase(request.offset);
                
                // キャンセルされていたらスキップ
                if (cancelledOffsets.count(request.offset) > 0) {
                    cancelledOffsets.erase(request.offset);
                    continue;
                }
                
                activeOffsets.insert(request.offset);
            }
            
            // ダウンロード実行
            bool success = DownloadChunk(httpClient, request.offset);
            
            // アクティブリストから削除
            {
                std::lock_guard<std::mutex> lock(queueMutex);
                activeOffsets.erase(request.offset);
            }
            
            if (!success) {
                Logger::Warn("ChunkDownloader: Failed to download chunk at offset {}",
                                    request.offset);
            }
        }
        
        Logger::Debug("ChunkDownloader worker {} stopped", workerId);
    }
    
    /// @brief チャンクをダウンロード
    /// @param client HttpClient
    /// @param offset バイトオフセット
    /// @return 成功した場合true
    bool DownloadChunk(HttpClient& client, int64_t offset) {
        if (!cache || url.empty()) {
            return false;
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
        HttpResponse response = client.GetRange(url, byteOffset, endByte);
        
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
    
    // キューをクリア
    {
        std::lock_guard<std::mutex> lock(m_impl->queueMutex);
        while (!m_impl->requestQueue.empty()) {
            m_impl->requestQueue.pop();
        }
        m_impl->pendingOffsets.clear();
        m_impl->activeOffsets.clear();
        m_impl->cancelledOffsets.clear();
    }
    
    m_impl->running.store(false);
    Logger::Info("ChunkDownloader: All workers stopped");
}

void ChunkDownloader::RequestChunk(int64_t offset, ChunkPriority priority) {
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

} // namespace io
} // namespace ytdlpspout

