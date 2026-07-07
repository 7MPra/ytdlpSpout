// =============================================================================
// HlsSegmentCache.cpp - HLSセグメントのメモリキャッシュ 実装
// =============================================================================

#include "hls/HlsSegmentCache.h"
#include "hls/AesCbcDecryptor.h"
#include "utils/Logger.h"

#include <unordered_map>
#include <unordered_set>
#include <list>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <algorithm>

namespace ytdlpspout {
namespace hls {

// =============================================================================
// 内部データ構造
// =============================================================================

/// @brief キャッシュされたセグメントデータ
struct CachedSegment {
    std::vector<uint8_t> data;      ///< 復号済みデータ
    size_t originalSize = 0;        ///< 元のサイズ（統計用）
    std::chrono::steady_clock::time_point lastAccess;  ///< 最終アクセス時刻
    bool isDecrypted = false;       ///< 復号済みか
};

// =============================================================================
// 内部実装クラス
// =============================================================================

struct HlsSegmentCache::Impl {
    // -------------------------------------------------------------------------
    // 設定
    // -------------------------------------------------------------------------
    HlsSegmentCacheConfig config;
    M3U8Playlist playlist;
    bool initialized = false;
    
    // -------------------------------------------------------------------------
    // 暗号化設定
    // -------------------------------------------------------------------------
    std::vector<uint8_t> encryptionKey;
    std::optional<std::vector<uint8_t>> explicitIv;
    bool hasEncryptionKey = false;
    
    // -------------------------------------------------------------------------
    // キャッシュデータ
    // -------------------------------------------------------------------------
    std::unordered_map<int64_t, CachedSegment> cache;
    
    // LRU管理
    std::list<int64_t> lruList;  // front = 最新, back = 最古
    std::unordered_map<int64_t, std::list<int64_t>::iterator> lruMap;
    
    // 統計
    size_t memoryUsed = 0;
    size_t cachedCount = 0;
    
    // 保護対象セグメント（再生中）
    int64_t protectedStart = -1;
    int64_t protectedEnd = -1;

    // 恒久的にダウンロードが失敗したとマークされたセグメント
    std::unordered_set<int64_t> failedSegments;

    // -------------------------------------------------------------------------
    // 同期
    // -------------------------------------------------------------------------
    mutable std::mutex mutex;
    std::condition_variable segmentAvailableCv;

    // -------------------------------------------------------------------------
    // ヘルパー関数
    // -------------------------------------------------------------------------

    /// @brief セグメントインデックスが有効か確認
    /// -1（初期化セグメント）は常に許可。それ以外は [0, 総セグメント数) の範囲のみ許可。
    bool IsValidSegmentIndex(int64_t index) const {
        if (index == -1) {
            return true;
        }
        if (index < -1) {
            return false;
        }
        return static_cast<size_t>(index) < playlist.segments.size();
    }

    /// @brief LRUリストでセグメントを最新に移動
    void TouchSegment(int64_t index) {
        auto it = lruMap.find(index);
        if (it != lruMap.end()) {
            lruList.erase(it->second);
        }
        lruList.push_front(index);
        lruMap[index] = lruList.begin();
    }
    
    /// @brief LRUリストからセグメントを削除
    void RemoveFromLRU(int64_t index) {
        auto it = lruMap.find(index);
        if (it != lruMap.end()) {
            lruList.erase(it->second);
            lruMap.erase(it);
        }
    }
    
    /// @brief セグメントが保護されているか確認
    bool IsProtected(int64_t index) const {
        if (protectedStart < 0 || protectedEnd < 0) {
            return false;
        }
        return index >= protectedStart && index <= protectedEnd;
    }
    
    /// @brief 削除可能な最も古いセグメントを取得
    int64_t GetEvictableSegment() const {
        // LRUリストの末尾から探索
        for (auto it = lruList.rbegin(); it != lruList.rend(); ++it) {
            if (!IsProtected(*it)) {
                return *it;
            }
        }
        return -1;  // 削除可能なセグメントがない
    }
    
    /// @brief セグメントをエビクト（削除）
    void EvictSegment(int64_t index) {
        auto cacheIt = cache.find(index);
        if (cacheIt != cache.end()) {
            memoryUsed -= cacheIt->second.data.size();
            cachedCount--;
            cache.erase(cacheIt);
            RemoveFromLRU(index);
            LOG_TRACE("Evicted segment {} from cache", index);
        }
    }
    
    /// @brief 必要なスペースを確保（LRUエビクション）
    void EnsureSpace(size_t requiredBytes) {
        while (memoryUsed + requiredBytes > config.maxMemoryBytes && !lruList.empty()) {
            int64_t victim = GetEvictableSegment();
            if (victim < 0) {
                LOG_WARN("Cannot evict any segment (all protected), memory limit may exceed");
                break;
            }
            EvictSegment(victim);
        }
    }
    
    /// @brief セグメントのIVを取得
    std::vector<uint8_t> GetIvForSegment(int64_t index) const {
        // 明示的なIVがあればそれを使用
        if (explicitIv.has_value()) {
            return *explicitIv;
        }
        
        // プレイリストにIVが指定されている場合
        if (playlist.encryptionKey.has_value() && 
            !playlist.encryptionKey->iv.empty()) {
            return playlist.encryptionKey->iv;
        }
        
        // メディアシーケンス番号からIVを生成
        if (index >= 0 && static_cast<size_t>(index) < playlist.segments.size()) {
            int64_t mediaSeq = playlist.segments[index].mediaSequence;
            return AesCbcDecryptor::GenerateIvFromSequence(mediaSeq);
        }
        
        return AesCbcDecryptor::GenerateIvFromSequence(index);
    }
    
    /// @brief セグメントを復号
    std::vector<uint8_t> DecryptSegment(const std::vector<uint8_t>& encryptedData, int64_t index) {
        if (!hasEncryptionKey || encryptionKey.empty()) {
            LOG_WARN("No encryption key set, returning data as-is");
            return encryptedData;
        }
        
        AesCbcDecryptor decryptor;
        if (!decryptor.Initialize(encryptionKey)) {
            LOG_ERROR("Failed to initialize AES decryptor");
            return {};
        }
        
        auto iv = GetIvForSegment(index);
        auto decrypted = decryptor.Decrypt(encryptedData, iv);
        
        if (!decrypted.empty()) {
             char buffer[4];
             std::string hex;
             for(size_t i=0; i<std::min(decrypted.size(), size_t(16)); ++i) {
                 snprintf(buffer, sizeof(buffer), "%02X ", decrypted[i]);
                 hex += buffer;
             }
             LOG_DEBUG("Decrypted segment {}. Head: {}", index, hex);
        }
        
        decryptor.Close();
        
        if (decrypted.empty() && !encryptedData.empty()) {
            LOG_ERROR("AES decryption failed for segment {}", index);
        }
        
        return decrypted;
    }
    
    /// @brief セグメント累積時間を計算
    double CalculateSegmentStartTime(int64_t index) const {
        if (index < 0 || playlist.segments.empty()) {
            return 0.0;
        }
        
        double time = 0.0;
        size_t maxIdx = std::min(static_cast<size_t>(index), playlist.segments.size());
        for (size_t i = 0; i < maxIdx; ++i) {
            time += playlist.segments[i].duration;
        }
        return time;
    }
    
    /// @brief 時間からセグメントインデックスを検索
    int64_t FindSegmentIndexFromTime(double seconds) const {
        if (playlist.segments.empty()) {
            return 0;
        }
        
        if (seconds <= 0.0) {
            return 0;
        }
        
        double accumulated = 0.0;
        for (size_t i = 0; i < playlist.segments.size(); ++i) {
            accumulated += playlist.segments[i].duration;
            if (accumulated > seconds) {
                return static_cast<int64_t>(i);
            }
        }
        
        // 最後のセグメントを返す
        return static_cast<int64_t>(playlist.segments.size() - 1);
    }
};

// =============================================================================
// コンストラクタ / デストラクタ
// =============================================================================

HlsSegmentCache::HlsSegmentCache(const HlsSegmentCacheConfig& config)
    : m_impl(std::make_unique<Impl>())
{
    m_impl->config = config;
    LOG_DEBUG("HlsSegmentCache created: maxMemory={}MB, maxSegments={}",
              config.maxMemoryBytes / (1024 * 1024), config.maxSegments);
}

HlsSegmentCache::~HlsSegmentCache() {
    LOG_DEBUG("HlsSegmentCache destroyed");
}

// =============================================================================
// 初期化
// =============================================================================

void HlsSegmentCache::Initialize(const M3U8Playlist& playlist) {
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    
    // 既存データをクリア
    m_impl->cache.clear();
    m_impl->lruList.clear();
    m_impl->lruMap.clear();
    m_impl->memoryUsed = 0;
    m_impl->cachedCount = 0;
    m_impl->protectedStart = -1;
    m_impl->protectedEnd = -1;
    m_impl->failedSegments.clear();

    // プレイリストを保存
    m_impl->playlist = playlist;
    m_impl->initialized = true;
    
    LOG_INFO("HlsSegmentCache initialized: {} segments, total duration={:.2f}s",
             playlist.segments.size(), playlist.totalDuration);
}

void HlsSegmentCache::SetEncryptionKey(const std::vector<uint8_t>& keyData,
                                       const std::optional<std::vector<uint8_t>>& explicitIv) {
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    
    if (keyData.size() != 16) {
        LOG_ERROR("Invalid AES key size: {} (expected 16)", keyData.size());
        return;
    }
    
    m_impl->encryptionKey = keyData;
    m_impl->explicitIv = explicitIv;
    m_impl->hasEncryptionKey = true;
    
    LOG_DEBUG("Encryption key set (with explicit IV: {})", explicitIv.has_value());
}

// =============================================================================
// 読み書き操作
// =============================================================================

bool HlsSegmentCache::WriteSegment(int64_t segmentIndex, std::vector<uint8_t>&& data, bool isEncrypted) {
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    
    if (!m_impl->initialized) {
        LOG_ERROR("Cache not initialized");
        return false;
    }
    
    // インデックス範囲チェック
    // -1 は初期化セグメントとして許可
    bool isInitSegment = (segmentIndex == -1);
    if (!isInitSegment && (segmentIndex < 0 || 
        static_cast<size_t>(segmentIndex) >= m_impl->playlist.segments.size())) {
        LOG_ERROR("Invalid segment index: {} (total: {})", 
                  segmentIndex, m_impl->playlist.segments.size());
        return false;
    }
    
    // 空データチェック
    if (data.empty()) {
        LOG_WARN("Empty data for segment {}", segmentIndex);
        return false;
    }
    
    size_t originalSize = data.size();
    std::vector<uint8_t> finalData;
    
    // 復号処理
    if (isEncrypted && m_impl->hasEncryptionKey) {
        finalData = m_impl->DecryptSegment(data, segmentIndex);
        if (finalData.empty()) {
            LOG_ERROR("Failed to decrypt segment {}", segmentIndex);
            return false;
        }
    } else {
        finalData = std::move(data);
    }
    
    size_t dataSize = finalData.size();
    
    // 既存のセグメントを更新する場合、メモリ使用量を調整
    auto existingIt = m_impl->cache.find(segmentIndex);
    if (existingIt != m_impl->cache.end()) {
        m_impl->memoryUsed -= existingIt->second.data.size();
        m_impl->cachedCount--;
    }
    
    // スペースを確保
    m_impl->EnsureSpace(dataSize);
    
    // キャッシュに保存
    CachedSegment cachedSeg;
    cachedSeg.data = std::move(finalData);
    cachedSeg.originalSize = originalSize;
    cachedSeg.lastAccess = std::chrono::steady_clock::now();
    cachedSeg.isDecrypted = isEncrypted;
    
    m_impl->cache[segmentIndex] = std::move(cachedSeg);
    m_impl->memoryUsed += dataSize;
    m_impl->cachedCount++;

    m_impl->TouchSegment(segmentIndex);

    // 成功した書き込みは失敗マークを解除する（再ダウンロード成功時の復旧など）
    m_impl->failedSegments.erase(segmentIndex);

    LOG_DEBUG("HlsSegmentCache: Wrote segment {} ({} bytes, encrypted={})",
              segmentIndex, dataSize, isEncrypted);

    // 待機中のスレッドに通知
    LOG_DEBUG("HlsSegmentCache: Notifying waiting threads for segment {}", segmentIndex);
    m_impl->segmentAvailableCv.notify_all();
    
    return true;
}

std::optional<std::vector<uint8_t>> HlsSegmentCache::ReadSegment(int64_t segmentIndex, int timeoutMs) {
    std::unique_lock<std::mutex> lock(m_impl->mutex);
    
    if (!m_impl->initialized) {
        return std::nullopt;
    }
    
    // インデックス範囲チェック
    bool isInitSegment = (segmentIndex == -1);
    if (!isInitSegment && (segmentIndex < 0 || 
        static_cast<size_t>(segmentIndex) >= m_impl->playlist.segments.size())) {
        return std::nullopt;
    }
    
    // キャッシュを検索
    auto it = m_impl->cache.find(segmentIndex);

    // キャッシュにない場合の待機処理
    while (it == m_impl->cache.end()) {
        // 恒久失敗としてマーク済みの場合は、タイムアウトを待たずに即座に失敗を返す
        if (m_impl->failedSegments.find(segmentIndex) != m_impl->failedSegments.end()) {
            LOG_WARN("ReadSegment: Segment {} is marked as permanently failed", segmentIndex);
            return std::nullopt;
        }

        if (timeoutMs == 0) {
            // 即時リターン
            return std::nullopt;
        } else if (timeoutMs < 0) {
            // 無制限待機
            m_impl->segmentAvailableCv.wait(lock);
        } else {
            // タイムアウト付き待機
            auto status = m_impl->segmentAvailableCv.wait_for(
                lock, std::chrono::milliseconds(timeoutMs));
            if (status == std::cv_status::timeout) {
                return std::nullopt;
            }
        }
        it = m_impl->cache.find(segmentIndex);
    }
    
    // LRUを更新
    it->second.lastAccess = std::chrono::steady_clock::now();
    m_impl->TouchSegment(segmentIndex);
    
    LOG_TRACE("Read segment {}: size={}", segmentIndex, it->second.data.size());
    
    // データのコピーを返す
    return it->second.data;
}

// =============================================================================
// 状態確認
// =============================================================================

bool HlsSegmentCache::IsSegmentCached(int64_t segmentIndex) const {
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    
    if (segmentIndex != -1 && (segmentIndex < 0 || 
        static_cast<size_t>(segmentIndex) >= m_impl->playlist.segments.size())) {
        return false;
    }
    
    return m_impl->cache.find(segmentIndex) != m_impl->cache.end();
}

bool HlsSegmentCache::WaitForSegment(int64_t segmentIndex, int timeoutMs) {
    LOG_DEBUG("WaitForSegment: Waiting for segment {} (timeout: {}ms)", segmentIndex, timeoutMs);
    std::unique_lock<std::mutex> lock(m_impl->mutex);
    
    // 範囲チェック
    bool isInitSegment = (segmentIndex == -1);
    if (!isInitSegment && (segmentIndex < 0 || 
        static_cast<size_t>(segmentIndex) >= m_impl->playlist.segments.size())) {
        LOG_WARN("WaitForSegment: Invalid segment index {}", segmentIndex);
        return false;
    }
    
    // 既にキャッシュされている場合は即座にtrue
    if (m_impl->cache.find(segmentIndex) != m_impl->cache.end()) {
        LOG_DEBUG("WaitForSegment: Segment {} already cached, returning immediately", segmentIndex);
        return true;
    }

    // 既に恒久失敗としてマーク済みの場合は、タイムアウトを待たずに即座にfalse
    if (m_impl->failedSegments.find(segmentIndex) != m_impl->failedSegments.end()) {
        LOG_WARN("WaitForSegment: Segment {} is marked as permanently failed, returning immediately", segmentIndex);
        return false;
    }

    LOG_DEBUG("WaitForSegment: Segment {} not in cache, waiting...", segmentIndex);

    // 待機ループ（キャッシュ済み、または失敗マーク済みのいずれかで起床する）
    auto predicate = [this, segmentIndex]() {
        return m_impl->cache.find(segmentIndex) != m_impl->cache.end() ||
               m_impl->failedSegments.find(segmentIndex) != m_impl->failedSegments.end();
    };

    if (timeoutMs <= 0) {
        // 無制限待機
        LOG_DEBUG("WaitForSegment: Waiting indefinitely for segment {}", segmentIndex);
        m_impl->segmentAvailableCv.wait(lock, predicate);

        bool cached = m_impl->cache.find(segmentIndex) != m_impl->cache.end();
        if (cached) {
            LOG_DEBUG("WaitForSegment: Segment {} now available after indefinite wait", segmentIndex);
        } else {
            LOG_WARN("WaitForSegment: Segment {} marked as permanently failed after indefinite wait", segmentIndex);
        }
        return cached;
    } else {
        // タイムアウト付き待機
        LOG_DEBUG("WaitForSegment: Waiting up to {}ms for segment {}", timeoutMs, segmentIndex);
        bool woke = m_impl->segmentAvailableCv.wait_for(
            lock,
            std::chrono::milliseconds(timeoutMs),
            predicate
        );

        if (!woke) {
            LOG_WARN("WaitForSegment: Timeout waiting for segment {} after {}ms", segmentIndex, timeoutMs);
            return false;
        }

        bool cached = m_impl->cache.find(segmentIndex) != m_impl->cache.end();
        if (cached) {
            LOG_DEBUG("WaitForSegment: Segment {} became available", segmentIndex);
        } else {
            LOG_WARN("WaitForSegment: Segment {} marked as permanently failed", segmentIndex);
        }
        return cached;
    }
}

// =============================================================================
// 失敗マーク管理（恒久ダウンロード失敗の伝搬）
// =============================================================================

void HlsSegmentCache::MarkSegmentFailed(int64_t segmentIndex) {
    std::lock_guard<std::mutex> lock(m_impl->mutex);

    if (!m_impl->initialized || !m_impl->IsValidSegmentIndex(segmentIndex)) {
        LOG_WARN("MarkSegmentFailed: Invalid or uninitialized segment index {}", segmentIndex);
        return;
    }

    m_impl->failedSegments.insert(segmentIndex);
    LOG_WARN("HlsSegmentCache: Segment {} marked as permanently failed", segmentIndex);

    // 待機中のスレッド（WaitForSegment/ReadSegment）を起床させる
    m_impl->segmentAvailableCv.notify_all();
}

bool HlsSegmentCache::IsSegmentFailed(int64_t segmentIndex) const {
    std::lock_guard<std::mutex> lock(m_impl->mutex);

    if (!m_impl->initialized || !m_impl->IsValidSegmentIndex(segmentIndex)) {
        return false;
    }

    return m_impl->failedSegments.find(segmentIndex) != m_impl->failedSegments.end();
}

void HlsSegmentCache::ClearSegmentFailed(int64_t segmentIndex) {
    std::lock_guard<std::mutex> lock(m_impl->mutex);

    if (!m_impl->initialized || !m_impl->IsValidSegmentIndex(segmentIndex)) {
        return;
    }

    m_impl->failedSegments.erase(segmentIndex);
    LOG_DEBUG("HlsSegmentCache: Cleared failed mark for segment {}", segmentIndex);
}

const HlsSegment* HlsSegmentCache::GetSegmentInfo(int64_t index) const {
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    
    if (index < 0 || static_cast<size_t>(index) >= m_impl->playlist.segments.size()) {
        return nullptr;
    }
    
    return &m_impl->playlist.segments[index];
}

size_t HlsSegmentCache::GetTotalSegmentCount() const {
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    return m_impl->playlist.segments.size();
}

size_t HlsSegmentCache::GetCachedSegmentCount() const {
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    return m_impl->cachedCount;
}

size_t HlsSegmentCache::GetCacheMemoryUsage() const {
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    return m_impl->memoryUsed;
}

// =============================================================================
// 時間変換
// =============================================================================

int64_t HlsSegmentCache::GetSegmentIndexFromTime(double seconds) const {
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    return m_impl->FindSegmentIndexFromTime(seconds);
}

double HlsSegmentCache::GetSegmentStartTime(int64_t segmentIndex) const {
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    return m_impl->CalculateSegmentStartTime(segmentIndex);
}

// =============================================================================
// 最適化
// =============================================================================

void HlsSegmentCache::OptimizeForPlayback(int64_t currentSegmentIndex, int64_t prefetchCount) {
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    
    if (!m_impl->initialized) {
        return;
    }
    
    // 保護範囲を設定
    m_impl->protectedStart = std::max(int64_t(0), currentSegmentIndex - 1);
    m_impl->protectedEnd = std::min(
        static_cast<int64_t>(m_impl->playlist.segments.size() - 1),
        currentSegmentIndex + prefetchCount
    );
    
    // 現在のセグメントをLRUで最新にする
    if (m_impl->cache.find(currentSegmentIndex) != m_impl->cache.end()) {
        m_impl->TouchSegment(currentSegmentIndex);
    }
    
    LOG_TRACE("OptimizeForPlayback: protected range [{}, {}]",
              m_impl->protectedStart, m_impl->protectedEnd);
}

}  // namespace hls
}  // namespace ytdlpspout
