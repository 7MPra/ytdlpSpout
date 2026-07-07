// =============================================================================
// SparseFileCache.cpp - スパースファイルキャッシュ 実装
// =============================================================================

#include "io/SparseFileCache.h"
#include "utils/Logger.h"

#include <unordered_map>
#include <unordered_set>
#include <list>
#include <algorithm>
#include <cstring>
#include <chrono>

namespace ytdlpspout {
namespace io {

// =============================================================================
// 内部実装クラス
// =============================================================================

struct SparseFileCache::Impl {
    // -------------------------------------------------------------------------
    // 設定
    // -------------------------------------------------------------------------
    SparseFileCacheConfig config;
    int64_t totalSize = 0;
    int64_t chunkCount = 0;
    bool initialized = false;
    
    // -------------------------------------------------------------------------
    // チャンクデータ構造
    // -------------------------------------------------------------------------
    struct ChunkData {
        std::vector<uint8_t> data;
        ChunkState state = ChunkState::Empty;
        int64_t lastAccessTime = 0;  // ミリ秒単位のタイムスタンプ
        int failCount = 0;          // Error状態への累積遷移回数（WriteChunk成功時にリセット）
    };
    
    // チャンクデータマップ（chunkIndex -> ChunkData）
    std::unordered_map<int64_t, ChunkData> chunks;
    
    // LRU順序リスト（最近アクセスされた順）
    std::list<int64_t> lruList;
    std::unordered_map<int64_t, std::list<int64_t>::iterator> lruMap;
    
    // -------------------------------------------------------------------------
    // 同期
    // -------------------------------------------------------------------------
    mutable std::mutex mutex;
    std::condition_variable chunkAvailableCv;
    
    // -------------------------------------------------------------------------
    // コールバック
    // -------------------------------------------------------------------------
    ChunkRequestCallback chunkRequestCallback;
    
    // -------------------------------------------------------------------------
    // 統計
    // -------------------------------------------------------------------------
    size_t memoryUsed = 0;
    int64_t cachedChunkCount = 0;
    int64_t downloadedChunkCount = 0;  // ダウンロード完了総数（LRU削除されても減らない）
    std::unordered_set<int64_t> downloadedChunks;  // ダウンロード済みチャンクの追跡
    
    // -------------------------------------------------------------------------
    // ヘルパー関数
    // -------------------------------------------------------------------------
    
    /// @brief 現在時刻をミリ秒で取得
    static int64_t GetCurrentTimeMs() {
        auto now = std::chrono::steady_clock::now();
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()).count();
    }
    
    /// @brief LRUリストでチャンクを最新に移動
    void TouchChunk(int64_t chunkIndex) {
        auto it = lruMap.find(chunkIndex);
        if (it != lruMap.end()) {
            lruList.erase(it->second);
        }
        lruList.push_front(chunkIndex);
        lruMap[chunkIndex] = lruList.begin();
    }
    
    /// @brief LRUリストからチャンクを削除
    void RemoveFromLRU(int64_t chunkIndex) {
        auto it = lruMap.find(chunkIndex);
        if (it != lruMap.end()) {
            lruList.erase(it->second);
            lruMap.erase(it);
        }
    }
    
    /// @brief 最も古いチャンクのインデックスを取得
    int64_t GetOldestChunk() const {
        if (lruList.empty()) {
            return -1;
        }
        return lruList.back();
    }
    
    /// @brief チャンクの実際のサイズを計算
    size_t CalculateActualChunkSize(int64_t chunkIndex) const {
        if (chunkIndex < 0 || chunkIndex >= chunkCount) {
            return 0;
        }
        
        int64_t startOffset = chunkIndex * static_cast<int64_t>(config.chunkSize);
        int64_t endOffset = startOffset + static_cast<int64_t>(config.chunkSize);
        
        if (endOffset > totalSize) {
            return static_cast<size_t>(totalSize - startOffset);
        }
        return config.chunkSize;
    }
};

// =============================================================================
// コンストラクタ・デストラクタ
// =============================================================================

SparseFileCache::SparseFileCache(const SparseFileCacheConfig& config)
    : m_impl(std::make_unique<Impl>())
{
    m_impl->config = config;
    
    // maxChunksが0の場合は自動計算
    if (m_impl->config.maxChunks == 0 && m_impl->config.chunkSize > 0) {
        m_impl->config.maxChunks = m_impl->config.maxMemoryBytes / m_impl->config.chunkSize;
    }
    
    LOG_DEBUG("SparseFileCache created: chunkSize={}, maxMemory={}MB, maxChunks={}",
              m_impl->config.chunkSize,
              m_impl->config.maxMemoryBytes / (1024 * 1024),
              m_impl->config.maxChunks);
}

SparseFileCache::~SparseFileCache() {
    Clear();
    LOG_DEBUG("SparseFileCache destroyed");
}

// =============================================================================
// 初期化
// =============================================================================

void SparseFileCache::Initialize(int64_t totalSize) {
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    
    if (m_impl->initialized) {
        LOG_WARN("SparseFileCache already initialized, clearing first");
        // ロック解除せずにクリア処理（内部用）
        m_impl->chunks.clear();
        m_impl->lruList.clear();
        m_impl->lruMap.clear();
        m_impl->memoryUsed = 0;
        m_impl->cachedChunkCount = 0;
    }
    
    m_impl->totalSize = totalSize;
    m_impl->chunkCount = (totalSize + static_cast<int64_t>(m_impl->config.chunkSize) - 1) 
                         / static_cast<int64_t>(m_impl->config.chunkSize);
    m_impl->initialized = true;
    
    LOG_INFO("SparseFileCache initialized: totalSize={}, chunkCount={}, chunkSize={}",
             totalSize, m_impl->chunkCount, m_impl->config.chunkSize);
}

void SparseFileCache::Clear() {
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    
    m_impl->chunks.clear();
    m_impl->lruList.clear();
    m_impl->lruMap.clear();
    m_impl->totalSize = 0;
    m_impl->chunkCount = 0;
    m_impl->memoryUsed = 0;
    m_impl->cachedChunkCount = 0;
    m_impl->initialized = false;
    
    LOG_DEBUG("SparseFileCache cleared");
}

bool SparseFileCache::IsInitialized() const {
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    return m_impl->initialized;
}

// =============================================================================
// チャンク状態操作
// =============================================================================

ChunkState SparseFileCache::GetChunkState(int64_t chunkIndex) const {
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    
    if (!m_impl->initialized || chunkIndex < 0 || chunkIndex >= m_impl->chunkCount) {
        return ChunkState::Empty;
    }
    
    auto it = m_impl->chunks.find(chunkIndex);
    if (it == m_impl->chunks.end()) {
        return ChunkState::Empty;
    }
    return it->second.state;
}

void SparseFileCache::SetChunkState(int64_t chunkIndex, ChunkState state) {
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    
    if (!m_impl->initialized || chunkIndex < 0 || chunkIndex >= m_impl->chunkCount) {
        return;
    }
    
    auto& chunk = m_impl->chunks[chunkIndex];
    ChunkState oldState = chunk.state;
    chunk.state = state;

    if (state == ChunkState::Error) {
        // 失敗回数を記録する。WriteChunk成功（Cached）まで保持される
        chunk.failCount++;
    }

    LOG_TRACE("Chunk {} state changed: {} -> {}",
              chunkIndex, static_cast<int>(oldState), static_cast<int>(state));

    if (state == ChunkState::Error) {
        // Error遷移を待機中のRead/WaitForChunkへ即座に伝え、
        // タイムアウトまで無駄に待たせないようにする
        m_impl->chunkAvailableCv.notify_all();
    }
}

int SparseFileCache::GetChunkFailCount(int64_t chunkIndex) const {
    std::lock_guard<std::mutex> lock(m_impl->mutex);

    if (!m_impl->initialized || chunkIndex < 0 || chunkIndex >= m_impl->chunkCount) {
        return 0;
    }

    auto it = m_impl->chunks.find(chunkIndex);
    if (it == m_impl->chunks.end()) {
        return 0;
    }
    return it->second.failCount;
}

// =============================================================================
// チャンク読み書き
// =============================================================================

bool SparseFileCache::WriteChunk(int64_t chunkIndex, const uint8_t* data, size_t size) {
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    
    if (!m_impl->initialized || chunkIndex < 0 || chunkIndex >= m_impl->chunkCount) {
        LOG_ERROR("WriteChunk failed: invalid chunk index {} (count={})", 
                  chunkIndex, m_impl->chunkCount);
        return false;
    }
    
    if (data == nullptr || size == 0) {
        LOG_ERROR("WriteChunk failed: invalid data or size");
        return false;
    }
    
    size_t expectedSize = m_impl->CalculateActualChunkSize(chunkIndex);
    if (size > expectedSize) {
        LOG_WARN("WriteChunk: size {} exceeds expected {} for chunk {}, truncating",
                 size, expectedSize, chunkIndex);
        size = expectedSize;
    }
    
    // メモリ制限チェック - 既存チャンクを削除してスペースを確保
    auto existingIt = m_impl->chunks.find(chunkIndex);
    bool isNewChunk = (existingIt == m_impl->chunks.end() || 
                       existingIt->second.state != ChunkState::Cached);
    
    if (isNewChunk) {
        // 新しいチャンクを追加する前にメモリ制限をチェック
        while (m_impl->memoryUsed + size > m_impl->config.maxMemoryBytes && 
               !m_impl->lruList.empty()) {
            int64_t oldestChunk = m_impl->GetOldestChunk();
            if (oldestChunk >= 0 && oldestChunk != chunkIndex) {
                auto oldIt = m_impl->chunks.find(oldestChunk);
                if (oldIt != m_impl->chunks.end() && oldIt->second.state == ChunkState::Cached) {
                    m_impl->memoryUsed -= oldIt->second.data.size();
                    m_impl->cachedChunkCount--;
                    oldIt->second.data.clear();
                    oldIt->second.state = ChunkState::Empty;
                    m_impl->RemoveFromLRU(oldestChunk);
                    m_impl->chunks.erase(oldIt);
                    LOG_TRACE("Evicted chunk {} for LRU", oldestChunk);
                } else {
                    m_impl->RemoveFromLRU(oldestChunk);
                }
            } else {
                break;  // 無限ループ防止
            }
        }
    } else {
        // 既存チャンクを更新する場合、古いメモリ使用量を差し引く
        if (existingIt->second.state == ChunkState::Cached) {
            m_impl->memoryUsed -= existingIt->second.data.size();
            m_impl->cachedChunkCount--;
        }
    }
    
    // データを書き込み
    auto& chunk = m_impl->chunks[chunkIndex];
    chunk.data.assign(data, data + size);
    chunk.state = ChunkState::Cached;
    chunk.lastAccessTime = Impl::GetCurrentTimeMs();
    chunk.failCount = 0;  // 成功したので失敗カウントをリセット

    m_impl->memoryUsed += size;
    m_impl->cachedChunkCount++;
    
    // ダウンロード完了チャンクを追跡（LRU削除されても減らない）
    if (m_impl->downloadedChunks.find(chunkIndex) == m_impl->downloadedChunks.end()) {
        m_impl->downloadedChunks.insert(chunkIndex);
        m_impl->downloadedChunkCount++;
    }
    
    m_impl->TouchChunk(chunkIndex);
    
    LOG_TRACE("WriteChunk: chunk={}, size={}, memoryUsed={}", 
              chunkIndex, size, m_impl->memoryUsed);
    
    // チャンク利用可能通知
    m_impl->chunkAvailableCv.notify_all();
    
    return true;
}

int64_t SparseFileCache::ReadChunk(int64_t chunkIndex, uint8_t* buffer, size_t bufferSize) {
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    
    if (!m_impl->initialized || chunkIndex < 0 || chunkIndex >= m_impl->chunkCount) {
        return -1;
    }
    
    if (buffer == nullptr || bufferSize == 0) {
        return -1;
    }
    
    auto it = m_impl->chunks.find(chunkIndex);
    if (it == m_impl->chunks.end() || it->second.state != ChunkState::Cached) {
        return -1;  // チャンクがキャッシュされていない
    }
    
    auto& chunk = it->second;
    size_t copySize = std::min(bufferSize, chunk.data.size());
    std::memcpy(buffer, chunk.data.data(), copySize);
    
    // アクセス時刻を更新（LRU用）
    chunk.lastAccessTime = Impl::GetCurrentTimeMs();
    m_impl->TouchChunk(chunkIndex);
    
    LOG_TRACE("ReadChunk: chunk={}, size={}", chunkIndex, copySize);
    
    return static_cast<int64_t>(copySize);
}

int64_t SparseFileCache::Read(int64_t offset, uint8_t* buffer, size_t size, int timeoutMs) {
    if (buffer == nullptr || size == 0) {
        return -1;
    }
    
    std::unique_lock<std::mutex> lock(m_impl->mutex);
    
    if (!m_impl->initialized || offset < 0 || offset >= m_impl->totalSize) {
        return -1;
    }
    
    // 読み取りサイズをファイル末尾で制限
    size_t actualSize = size;
    if (offset + static_cast<int64_t>(size) > m_impl->totalSize) {
        actualSize = static_cast<size_t>(m_impl->totalSize - offset);
    }
    
    size_t bytesRead = 0;
    int64_t currentOffset = offset;
    
    while (bytesRead < actualSize) {
        int64_t chunkIndex = currentOffset / static_cast<int64_t>(m_impl->config.chunkSize);
        int64_t offsetInChunk = currentOffset % static_cast<int64_t>(m_impl->config.chunkSize);
        
        // チャンクがキャッシュされるまで待機
        auto it = m_impl->chunks.find(chunkIndex);
        while (it == m_impl->chunks.end() || it->second.state != ChunkState::Cached) {
            // 再試行上限を超えて恒久的に失敗したチャンクは、タイムアウトを待たず
            // 即座にエラーを返す（無限リトライ・無限EAGAINストールを防ぐ）
            if (it != m_impl->chunks.end() &&
                it->second.state == ChunkState::Error &&
                it->second.failCount > kMaxChunkFailCount) {
                LOG_ERROR("Read: chunk {} permanently failed (failCount={}), aborting read",
                          chunkIndex, it->second.failCount);
                return bytesRead > 0 ? static_cast<int64_t>(bytesRead) : -1;
            }

            if (timeoutMs == 0) {
                // タイムアウトなし - 無限待機
                m_impl->chunkAvailableCv.wait(lock);
            } else {
                auto status = m_impl->chunkAvailableCv.wait_for(
                    lock, std::chrono::milliseconds(timeoutMs));
                if (status == std::cv_status::timeout) {
                    LOG_WARN("Read timeout waiting for chunk {}", chunkIndex);
                    return bytesRead > 0 ? static_cast<int64_t>(bytesRead) : 0;
                }
            }
            it = m_impl->chunks.find(chunkIndex);
        }
        
        auto& chunk = it->second;
        size_t chunkDataSize = chunk.data.size();
        
        // このチャンクから読み取れるバイト数
        size_t remainingInChunk = chunkDataSize - static_cast<size_t>(offsetInChunk);
        size_t toRead = std::min(remainingInChunk, actualSize - bytesRead);
        
        std::memcpy(buffer + bytesRead, chunk.data.data() + offsetInChunk, toRead);
        
        // アクセス時刻を更新
        chunk.lastAccessTime = Impl::GetCurrentTimeMs();
        m_impl->TouchChunk(chunkIndex);
        
        bytesRead += toRead;
        currentOffset += static_cast<int64_t>(toRead);
    }
    
    return static_cast<int64_t>(bytesRead);
}

bool SparseFileCache::WaitForChunk(int64_t chunkIndex, int timeoutMs) {
    std::unique_lock<std::mutex> lock(m_impl->mutex);
    
    if (!m_impl->initialized || chunkIndex < 0 || chunkIndex >= m_impl->chunkCount) {
        return false;
    }
    
    auto checkCached = [this, chunkIndex]() {
        auto it = m_impl->chunks.find(chunkIndex);
        return it != m_impl->chunks.end() && it->second.state == ChunkState::Cached;
    };
    
    if (checkCached()) {
        return true;  // 既にキャッシュ済み
    }
    
    if (timeoutMs == 0) {
        // 無限待機
        m_impl->chunkAvailableCv.wait(lock, checkCached);
        return true;
    } else {
        // タイムアウト付き待機
        return m_impl->chunkAvailableCv.wait_for(
            lock, std::chrono::milliseconds(timeoutMs), checkCached);
    }
}

// =============================================================================
// 情報取得
// =============================================================================

int64_t SparseFileCache::GetTotalSize() const {
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    return m_impl->totalSize;
}

int64_t SparseFileCache::GetChunkCount() const {
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    return m_impl->chunkCount;
}

size_t SparseFileCache::GetChunkSize() const {
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    return m_impl->config.chunkSize;
}

size_t SparseFileCache::GetActualChunkSize(int64_t chunkIndex) const {
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    return m_impl->CalculateActualChunkSize(chunkIndex);
}

int64_t SparseFileCache::GetChunkIndex(int64_t byteOffset) const {
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    if (!m_impl->initialized || byteOffset < 0) {
        return -1;
    }
    return byteOffset / static_cast<int64_t>(m_impl->config.chunkSize);
}

int64_t SparseFileCache::GetByteOffset(int64_t chunkIndex) const {
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    if (!m_impl->initialized || chunkIndex < 0) {
        return -1;
    }
    return chunkIndex * static_cast<int64_t>(m_impl->config.chunkSize);
}

size_t SparseFileCache::GetMemoryUsage() const {
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    return m_impl->memoryUsed;
}

int64_t SparseFileCache::GetCachedChunkCount() const {
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    return m_impl->cachedChunkCount;
}

int64_t SparseFileCache::GetDownloadedChunkCount() const {
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    return m_impl->downloadedChunkCount;
}

// =============================================================================
// チャンク要求コールバック
// =============================================================================

void SparseFileCache::SetChunkRequestCallback(ChunkRequestCallback callback) {
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    m_impl->chunkRequestCallback = std::move(callback);
}

void SparseFileCache::RequestChunk(int64_t chunkIndex) {
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    
    if (!m_impl->initialized || chunkIndex < 0 || chunkIndex >= m_impl->chunkCount) {
        return;
    }
    
    // 既にキャッシュ済みまたはダウンロード中なら何もしない
    auto it = m_impl->chunks.find(chunkIndex);
    if (it != m_impl->chunks.end()) {
        ChunkState state = it->second.state;
        if (state == ChunkState::Cached || state == ChunkState::Downloading || 
            state == ChunkState::Pending) {
            return;
        }
    }
    
    // 状態をPendingに設定
    m_impl->chunks[chunkIndex].state = ChunkState::Pending;
    
    // コールバック呼び出し
    if (m_impl->chunkRequestCallback) {
        int64_t byteOffset = chunkIndex * static_cast<int64_t>(m_impl->config.chunkSize);
        size_t size = m_impl->CalculateActualChunkSize(chunkIndex);
        m_impl->chunkRequestCallback(chunkIndex, byteOffset, size);
    }
}

// =============================================================================
// プライベートメソッド
// =============================================================================

void SparseFileCache::EvictOldChunks() {
    // 注: この関数はロックを取得した状態で呼び出す必要がある
    // WriteChunk内で直接LRU削除を行うため、現在は使用されていない
}

void SparseFileCache::NotifyChunkAvailable([[maybe_unused]] int64_t chunkIndex) {
    // WriteChunk内で直接notify_allを呼び出すため、現在は使用されていない
    m_impl->chunkAvailableCv.notify_all();
}

} // namespace io
} // namespace ytdlpspout
