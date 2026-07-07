// =============================================================================
// PrefetchScheduler.cpp - プリフェッチスケジューラー実装
// =============================================================================

#include "io/PrefetchScheduler.h"
#include "io/SparseFileCache.h"
#include "io/ChunkDownloader.h"
#include "utils/Logger.h"

#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>

namespace ytdlpspout {
namespace io {

// =============================================================================
// 実装クラス
// =============================================================================
struct PrefetchScheduler::Impl {
    // 設定
    PrefetchConfig config;
    
    // 参照（所有しない）
    SparseFileCache* cache = nullptr;
    ChunkDownloader* downloader = nullptr;
    
    // 状態
    std::atomic<bool> running{false};
    std::atomic<bool> shutdownRequested{false};
    std::atomic<int64_t> currentPosition{0};
    
    // スレッド
    std::thread workerThread;
    std::mutex mutex;
    std::condition_variable cv;
    
    ~Impl() {
        shutdownRequested = true;
        running = false;
        cv.notify_all();
        if (workerThread.joinable()) {
            workerThread.join();
        }
    }
};

// =============================================================================
// コンストラクタ・デストラクタ
// =============================================================================

PrefetchScheduler::PrefetchScheduler()
    : m_impl(std::make_unique<Impl>()) {
}

PrefetchScheduler::~PrefetchScheduler() {
    Shutdown();
}

// =============================================================================
// 初期化
// =============================================================================

void PrefetchScheduler::Initialize(SparseFileCache* cache, 
                                    ChunkDownloader* downloader,
                                    const PrefetchConfig& config) {
    m_impl->cache = cache;
    m_impl->downloader = downloader;
    m_impl->config = config;
    
    LOG_DEBUG("PrefetchScheduler initialized (prefetchAhead={}, prefetchBehind={})",
              config.prefetchChunksAhead, config.prefetchChunksBehind);
}

void PrefetchScheduler::Shutdown() {
    Stop();
    m_impl->shutdownRequested = true;
    m_impl->cv.notify_all();
    
    if (m_impl->workerThread.joinable()) {
        m_impl->workerThread.join();
    }
    
    m_impl->cache = nullptr;
    m_impl->downloader = nullptr;
}

// =============================================================================
// 再生位置管理
// =============================================================================

void PrefetchScheduler::UpdatePlaybackPosition(int64_t byteOffset) {
    m_impl->currentPosition = byteOffset;
    
    // ワーカースレッドに通知
    m_impl->cv.notify_one();
}

void PrefetchScheduler::NotifySeek(int64_t byteOffset) {
    m_impl->currentPosition = byteOffset;
    
    // シーク時は即座にプリフェッチを再スケジュール
    TriggerPrefetch();
}

int64_t PrefetchScheduler::GetCurrentPosition() const {
    return m_impl->currentPosition;
}

// =============================================================================
// プリフェッチ制御
// =============================================================================

void PrefetchScheduler::Start() {
    if (m_impl->running) {
        return;
    }
    
    if (!m_impl->cache || !m_impl->downloader) {
        LOG_ERROR("PrefetchScheduler not properly initialized");
        return;
    }
    
    m_impl->running = true;
    m_impl->shutdownRequested = false;
    
    // ワーカースレッドを開始
    m_impl->workerThread = std::thread([this]() {
        LOG_DEBUG("PrefetchScheduler worker thread started");
        
        while (m_impl->running && !m_impl->shutdownRequested) {
            // プリフェッチを実行
            TriggerPrefetch();
            
            // 次の更新まで待機
            std::unique_lock<std::mutex> lock(m_impl->mutex);
            m_impl->cv.wait_for(lock, 
                std::chrono::milliseconds(m_impl->config.updateIntervalMs));
        }
        
        LOG_DEBUG("PrefetchScheduler worker thread stopped");
    });
    
    LOG_INFO("PrefetchScheduler started");
}

void PrefetchScheduler::Stop() {
    if (!m_impl->running) {
        return;
    }
    
    m_impl->running = false;
    m_impl->cv.notify_all();
    
    if (m_impl->workerThread.joinable()) {
        m_impl->workerThread.join();
    }
    
    LOG_INFO("PrefetchScheduler stopped");
}

bool PrefetchScheduler::IsRunning() const {
    return m_impl->running;
}

std::vector<int64_t> PrefetchScheduler::GetPrefetchChunks() const {
    std::vector<int64_t> chunks;
    
    if (!m_impl->cache) {
        return chunks;
    }
    
    int64_t currentChunk = m_impl->cache->GetChunkIndex(m_impl->currentPosition);
    int64_t totalChunks = m_impl->cache->GetChunkCount();
    
    // 現在位置から先のチャンクを追加（優先度順）
    for (int i = 0; i < m_impl->config.prefetchChunksAhead; ++i) {
        int64_t chunk = currentChunk + i;
        if (chunk >= 0 && chunk < totalChunks) {
            chunks.push_back(chunk);
        }
    }
    
    return chunks;
}

void PrefetchScheduler::TriggerPrefetch() {
    if (!m_impl->cache || !m_impl->downloader) {
        return;
    }
    
    int64_t currentChunk = m_impl->cache->GetChunkIndex(m_impl->currentPosition);
    int64_t totalChunks = m_impl->cache->GetChunkCount();
    
    // フェーズ1: 優先度の高いチャンク（先読み範囲内）をリクエスト
    std::vector<int64_t> chunks = GetPrefetchChunks();
    
    for (int64_t chunk : chunks) {
        ChunkState state = m_impl->cache->GetChunkState(chunk);

        // Empty（未ダウンロード）に加え、再試行上限に達していないErrorチャンクも
        // 再リクエスト対象にする（恒久ストール防止・Issue B-IO）
        bool shouldRequest = (state == ChunkState::Empty) ||
            (state == ChunkState::Error &&
             m_impl->cache->GetChunkFailCount(chunk) <= kMaxChunkFailCount);

        if (shouldRequest) {
            // 優先度を決定
            ChunkPriority priority;

            if (chunk <= currentChunk + m_impl->config.criticalChunksAhead) {
                priority = ChunkPriority::Critical;
            } else if (chunk <= currentChunk + 6) {
                priority = ChunkPriority::High;
            } else {
                priority = ChunkPriority::Medium;
            }

            if (state == ChunkState::Error) {
                // 状態遷移を明確にするため、再リクエスト前にEmptyへリセットする
                // （ChunkDownloader側はError状態でも受け付けるが、失敗カウントは保持される）
                m_impl->cache->SetChunkState(chunk, ChunkState::Empty);
            }

            // ダウンロードをリクエスト
            int64_t byteOffset = m_impl->cache->GetByteOffset(chunk);
            m_impl->downloader->RequestChunk(byteOffset, priority);
        }
    }
    
    // フェーズ2: 継続ダウンロード（先読み範囲外をバッチで低優先度ダウンロード）
    if (m_impl->config.enableContinuousDownload) {
        // バッチサイズ分だけ追加でキューに入れる（全チャンクを一度に入れない）
        int64_t startChunk = currentChunk + m_impl->config.prefetchChunksAhead;
        int64_t endChunk = std::min(
            startChunk + m_impl->config.continuousDownloadBatch,
            totalChunks
        );
        
        for (int64_t chunk = startChunk; chunk < endChunk; ++chunk) {
            ChunkState state = m_impl->cache->GetChunkState(chunk);

            bool shouldRequest = (state == ChunkState::Empty) ||
                (state == ChunkState::Error &&
                 m_impl->cache->GetChunkFailCount(chunk) <= kMaxChunkFailCount);

            if (shouldRequest) {
                if (state == ChunkState::Error) {
                    m_impl->cache->SetChunkState(chunk, ChunkState::Empty);
                }
                int64_t byteOffset = m_impl->cache->GetByteOffset(chunk);
                m_impl->downloader->RequestChunk(byteOffset, ChunkPriority::Low);
            }
        }
    }
}

// =============================================================================
// 設定
// =============================================================================

void PrefetchScheduler::Configure(const PrefetchConfig& config) {
    m_impl->config = config;
}

const PrefetchConfig& PrefetchScheduler::GetConfig() const {
    return m_impl->config;
}

} // namespace io
} // namespace ytdlpspout
