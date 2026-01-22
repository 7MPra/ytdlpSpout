// =============================================================================
// HlsCustomAVIOContext.cpp - HLS用カスタムAVIOContext実装
// =============================================================================

#include "hls/HlsCustomAVIOContext.h"
#include "hls/HlsSegmentCache.h"
#include "utils/Logger.h"

#include <algorithm>
#include <cstring>
#include <mutex>
#include <vector>
#include <functional>

// FFmpeg
extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/error.h>
}

namespace ytdlpspout {
namespace hls {

// =============================================================================
// 定数
// =============================================================================

/// @brief AVIOバッファサイズ（32KB）
static constexpr size_t AVIO_BUFFER_SIZE = 32 * 1024;

/// @brief 推定ビットレート（8 Mbps = 1 MB/s）
static constexpr double ESTIMATED_BYTES_PER_SECOND = 1.0 * 1024 * 1024;

// =============================================================================
// 実装クラス
// =============================================================================

struct HlsCustomAVIOContext::Impl {
    // 外部参照
    HlsSegmentCache* cache = nullptr;
    
    // プレイリスト情報
    M3U8Playlist playlist;
    
    // セグメントオフセットテーブル
    // segmentOffsets[i] = セグメント0〜i-1のサイズ合計（セグメントiの開始オフセット）
    std::vector<int64_t> segmentOffsets;
    
    // 各セグメントの実サイズ（-1 = 未知）
    std::vector<int64_t> segmentSizes;
    
    // 総サイズ（推定または実測）
    int64_t totalSize = 0;
    
    // 現在位置
    int64_t position = 0;
    
    // 現在のセグメントインデックス
    int64_t currentSegmentIndex = 0;
    
    // 現在のセグメント内オフセット
    int64_t currentSegmentOffset = 0;
    
    // 初期化セグメント情報
    bool hasInitSegment = false;
    int64_t initSegmentSize = 0;
    static constexpr int64_t ESTIMATED_INIT_SEGMENT_SIZE = 4096; // 4KB推定
    
    // 読み取りタイムアウト（ミリ秒）
    int readTimeoutMs = 5000;
    
    // AVIOContext
    AVIOContext* avioContext = nullptr;
    uint8_t* avioBuffer = nullptr;
    
    // 状態
    bool initialized = false;
    std::mutex mutex;
    
    /// @brief デストラクタ
    ~Impl() {
        // AVIOContextをクリーンアップ
        if (avioContext) {
            avio_context_free(&avioContext);
            avioContext = nullptr;
            avioBuffer = nullptr;  // avio_context_freeが解放
        }
    }
    
    /// @brief セグメントオフセットテーブルを構築（推定サイズ使用）
    void BuildSegmentOffsets() {
        size_t segmentCount = playlist.segments.size();
        segmentOffsets.resize(segmentCount);
        segmentOffsets.resize(segmentCount);
        segmentSizes.resize(segmentCount, -1);  // 全て未知
        
        int64_t offset = 0;
        
        // 初期化セグメントがある場合
        if (playlist.map.has_value()) {
            hasInitSegment = true;
            initSegmentSize = ESTIMATED_INIT_SEGMENT_SIZE;
            
            // バイトレンジ指定がある場合はサイズ確定
            if (playlist.map->byteRangeLength > 0) {
                initSegmentSize = playlist.map->byteRangeLength;
            }
            
            offset = initSegmentSize;
            LOG_DEBUG("HlsCustomAVIOContext: Has Initialization Segment, size={}", initSegmentSize);
        } else {
            hasInitSegment = false;
            initSegmentSize = 0;
        }
        
        for (size_t i = 0; i < segmentCount; ++i) {
            segmentOffsets[i] = offset;
            
            // 推定サイズ: duration * bytesPerSecond
            int64_t estimatedSize = static_cast<int64_t>(
                playlist.segments[i].duration * ESTIMATED_BYTES_PER_SECOND
            );
            
            offset += estimatedSize;
        }
        
        totalSize = offset;
        
        LOG_DEBUG("HlsCustomAVIOContext: Built segment offsets, segments={}, estimatedSize={}",
                  segmentCount, totalSize);
    }
    
    /// @brief セグメントの実サイズで更新
    void UpdateSegmentSize(int64_t segmentIndex, int64_t actualSize) {
        // 初期化セグメントの更新
        if (segmentIndex == -1) {
            if (!hasInitSegment || initSegmentSize == actualSize) {
                return;
            }
            
            int64_t sizeDiff = actualSize - initSegmentSize;
            initSegmentSize = actualSize;
            
            // 全オフセットをシフト
            for (auto& off : segmentOffsets) {
                off += sizeDiff;
            }
            totalSize += sizeDiff;
            
            LOG_INFO("HlsCustomAVIOContext: Updated Init Segment size: -> {}, newTotal={}",
                      actualSize, totalSize);
            return;
        }

        if (segmentIndex < 0 || segmentIndex >= static_cast<int64_t>(segmentSizes.size())) {
            return;
        }
        
        // 既に更新済み
        if (segmentSizes[segmentIndex] == actualSize) {
            return;
        }
        
        int64_t oldSize = segmentSizes[segmentIndex];
        segmentSizes[segmentIndex] = actualSize;
        
        // 以前は推定サイズを使用していた場合
        if (oldSize < 0) {
            int64_t estimatedSize = static_cast<int64_t>(
                playlist.segments[segmentIndex].duration * ESTIMATED_BYTES_PER_SECOND
            );
            oldSize = estimatedSize;
        }
        
        int64_t sizeDiff = actualSize - oldSize;
        
        // 後続のセグメントオフセットを更新
        for (size_t i = segmentIndex + 1; i < segmentOffsets.size(); ++i) {
            segmentOffsets[i] += sizeDiff;
        }
        
        // 総サイズを更新
        totalSize += sizeDiff;
        
        LOG_TRACE("HlsCustomAVIOContext: Updated segment {} size: {} -> {}, newTotal={}",
                  segmentIndex, oldSize, actualSize, totalSize);
    }
    
    /// @brief バイトオフセットからセグメントインデックスと内部オフセットを計算
    void CalculateSegmentPosition(int64_t byteOffset, int64_t& outSegmentIndex, int64_t& outInternalOffset) {
        // 初期化セグメント内の場合
        if (hasInitSegment && byteOffset < initSegmentSize) {
            outSegmentIndex = -1;
            outInternalOffset = byteOffset;
            return;
        }

        if (byteOffset <= 0 || segmentOffsets.empty()) {
            outSegmentIndex = hasInitSegment ? 0 : 0;
            // initSegmentSize以上の場合はオフセット調整
            outInternalOffset = hasInitSegment ? 
                std::max<int64_t>(0, byteOffset - initSegmentSize) : 
                std::max<int64_t>(0, byteOffset);
            return;
        }
        
        // 二分探索でセグメントを検索
        auto it = std::upper_bound(segmentOffsets.begin(), segmentOffsets.end(), byteOffset);
        if (it == segmentOffsets.begin()) {
            outSegmentIndex = 0;
            outInternalOffset = byteOffset;
        } else {
            --it;
            outSegmentIndex = std::distance(segmentOffsets.begin(), it);
            outInternalOffset = byteOffset - *it;
        }
        
        // 範囲チェック
        int64_t maxIndex = static_cast<int64_t>(segmentOffsets.size()) - 1;
        if (outSegmentIndex > maxIndex) {
            outSegmentIndex = maxIndex;
            outInternalOffset = byteOffset - segmentOffsets[maxIndex];
        }
    }
    
    /// @brief セグメントのサイズを取得（推定または実測）
    int64_t GetSegmentSize(int64_t segmentIndex) {
        if (segmentIndex == -1) {
            return hasInitSegment ? initSegmentSize : 0;
        }

        if (segmentIndex < 0 || segmentIndex >= static_cast<int64_t>(segmentSizes.size())) {
            return 0;
        }
        
        if (segmentSizes[segmentIndex] >= 0) {
            return segmentSizes[segmentIndex];
        }
        
        // 推定サイズ
        return static_cast<int64_t>(
            playlist.segments[segmentIndex].duration * ESTIMATED_BYTES_PER_SECOND
        );
    }


    // シークコールバック
    std::function<void(int64_t)> onSeekCallback;
};

void HlsCustomAVIOContext::SetOnSeekCallback(std::function<void(int64_t)> callback) {
    if (!m_impl) return;
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    m_impl->onSeekCallback = callback;
}

// =============================================================================
// コンストラクタ・デストラクタ
// =============================================================================

HlsCustomAVIOContext::HlsCustomAVIOContext()
    : m_impl(std::make_unique<Impl>()) {
}

HlsCustomAVIOContext::~HlsCustomAVIOContext() {
    Close();
}

// =============================================================================
// 初期化 / 終了
// =============================================================================

bool HlsCustomAVIOContext::Initialize(HlsSegmentCache* cache, const M3U8Playlist& playlist) {
    // 既に初期化されている場合は先にクローズ
    Close();
    
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    
    // バリデーション
    if (!cache) {
        LOG_ERROR("HlsCustomAVIOContext: cache is null");
        return false;
    }
    
    if (playlist.segments.empty()) {
        LOG_ERROR("HlsCustomAVIOContext: playlist has no segments");
        return false;
    }
    
    m_impl->cache = cache;
    m_impl->playlist = playlist;
    
    // セグメントオフセットテーブルを構築
    m_impl->BuildSegmentOffsets();
    
    // AVIOバッファを割り当て
    m_impl->avioBuffer = static_cast<uint8_t*>(av_malloc(AVIO_BUFFER_SIZE));
    if (!m_impl->avioBuffer) {
        LOG_ERROR("HlsCustomAVIOContext: Failed to allocate AVIO buffer");
        return false;
    }
    
    // AVIOContextを作成
    m_impl->avioContext = avio_alloc_context(
        m_impl->avioBuffer,
        static_cast<int>(AVIO_BUFFER_SIZE),
        0,              // write_flag = 0 (読み取り専用)
        this,           // opaque
        &ReadPacket,    // read_packet
        nullptr,        // write_packet
        &SeekCallback   // seek
    );
    
    if (!m_impl->avioContext) {
        LOG_ERROR("HlsCustomAVIOContext: Failed to allocate AVIOContext");
        av_free(m_impl->avioBuffer);
        m_impl->avioBuffer = nullptr;
        return false;
    }
    
    // シーク可能フラグを設定
    m_impl->avioContext->seekable = AVIO_SEEKABLE_NORMAL;
    
    // 状態初期化
    m_impl->position = 0;
    m_impl->currentSegmentIndex = m_impl->hasInitSegment ? -1 : 0;
    m_impl->currentSegmentOffset = 0;
    m_impl->initialized = true;
    
    LOG_INFO("HlsCustomAVIOContext: Initialized with {} segments, estimatedSize={}",
             playlist.segments.size(), m_impl->totalSize);
    
    return true;
}

void HlsCustomAVIOContext::Close() {
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    
    if (!m_impl->initialized) {
        return;
    }
    
    LOG_INFO("HlsCustomAVIOContext: Closing...");
    
    // AVIOContextを解放
    if (m_impl->avioContext) {
        avio_context_free(&m_impl->avioContext);
        m_impl->avioContext = nullptr;
        m_impl->avioBuffer = nullptr;  // avio_context_freeが解放
    }
    
    // 状態リセット
    m_impl->cache = nullptr;
    m_impl->playlist = M3U8Playlist{};
    m_impl->segmentOffsets.clear();
    m_impl->segmentSizes.clear();
    m_impl->totalSize = 0;
    m_impl->position = 0;
    m_impl->currentSegmentIndex = 0;
    m_impl->currentSegmentOffset = 0;
    m_impl->initialized = false;
    
    LOG_INFO("HlsCustomAVIOContext: Closed");
}

// =============================================================================
// FFmpeg統合
// =============================================================================

AVIOContext* HlsCustomAVIOContext::GetAVIOContext() const {
    return m_impl->avioContext;
}

// =============================================================================
// サイズ / 位置
// =============================================================================

int64_t HlsCustomAVIOContext::GetSize() const {
    return m_impl->totalSize;
}

int64_t HlsCustomAVIOContext::GetPosition() const {
    return m_impl->position;
}

int64_t HlsCustomAVIOContext::GetCurrentSegmentIndex() const {
    return m_impl->currentSegmentIndex;
}

// =============================================================================
// シーク
// =============================================================================

int64_t HlsCustomAVIOContext::Seek(int64_t offset, int whence) {
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    
    if (!m_impl->initialized) {
        return -1;
    }
    
    // AVSEEK_SIZE: 総サイズを返す
    if (whence == AVSEEK_SIZE) {
        LOG_TRACE("HlsCustomAVIOContext: Seek AVSEEK_SIZE -> {}", m_impl->totalSize);
        return m_impl->totalSize;
    }
    
    int64_t newPosition;
    
    switch (whence) {
        case SEEK_SET:
            newPosition = offset;
            break;
        case SEEK_CUR:
            newPosition = m_impl->position + offset;
            break;
        case SEEK_END:
            newPosition = m_impl->totalSize + offset;
            break;
        default:
            LOG_ERROR("HlsCustomAVIOContext: Invalid whence: {}", whence);
            return -1;
    }
    
    // 負の位置へのシークは失敗
    if (newPosition < 0) {
        LOG_ERROR("HlsCustomAVIOContext: Seek to negative position: {}", newPosition);
        return -1;
    }
    
    LOG_TRACE("HlsCustomAVIOContext: Seek {} -> {} (whence={})", 
              m_impl->position, newPosition, whence);
    
    // 新しいセグメント位置を計算
    m_impl->CalculateSegmentPosition(
        newPosition, 
        m_impl->currentSegmentIndex, 
        m_impl->currentSegmentOffset
    );
    
    m_impl->position = newPosition;
    
    // セグメントが変更された場合、コールバックを呼び出し
    if (m_impl->onSeekCallback && m_impl->currentSegmentIndex != -1) {
        // 現在のセグメントインデックスを通知
        // ロックを持ったまま呼び出すため、受け側でデッドロックに注意が必要
        // しかし、AVIOContextのロックは通常Managerのロックの内側で取得されることはない（逆はある）
        // Manager::Open -> ManagerLock -> AVIO::Open -> AVIOLock (OK)
        // FFmpeg::Seek -> AVIOLock -> Callback -> Manager::Request (ManagerLock) (OK)
        // Manager::SeekToTime -> ManagerLock -> AVIO::SeekToTime -> AVIOLock (OK)
        // Manager::SeekToTime -> ManagerLock -> AVIO::SeekToTime -> AVIOLock -> Callback -> Manager::Request (Recursive ManagerLock!) (Danger)
        
        // したがって、SeekToTime（Manager起点のシーク）ではコールバックを呼ばない、
        // あるいはFFmpeg起点のSeek（AVIO::Seek）でのみ呼ぶようにする。
        // ここはAVIO::Seekなので、FFmpeg起点。Manager::SeekToTimeはAVIO::SeekToTimeを呼ぶ。
        // AVIO::SeekToTimeはAVIO::Seekを呼ばない（別実装）。
        // よって、ここ（Seek）で呼ぶのは安全。
        m_impl->onSeekCallback(m_impl->currentSegmentIndex);
    }
    
    return newPosition;
}

int64_t HlsCustomAVIOContext::SeekToTime(double seconds) {
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    
    if (!m_impl->initialized) {
        return -1;
    }
    
    // 負の時間は0にクランプ
    if (seconds < 0) {
        seconds = 0;
    }
    
    // キャッシュから時間→セグメントインデックスを取得
    int64_t segmentIndex = m_impl->cache->GetSegmentIndexFromTime(seconds);
    
    // セグメントの先頭へシーク
    if (segmentIndex < 0) {
        segmentIndex = 0;
    }
    
    int64_t maxIndex = static_cast<int64_t>(m_impl->segmentOffsets.size()) - 1;
    if (segmentIndex > maxIndex) {
        segmentIndex = maxIndex;
    }
    
    int64_t newPosition = m_impl->segmentOffsets[segmentIndex];
    
    LOG_TRACE("HlsCustomAVIOContext: SeekToTime {} sec -> segment {} (offset {})",
              seconds, segmentIndex, newPosition);
    
    m_impl->currentSegmentIndex = segmentIndex;
    m_impl->currentSegmentOffset = 0;
    m_impl->position = newPosition;
    
    return newPosition;
}

// =============================================================================
// 設定
// =============================================================================

void HlsCustomAVIOContext::SetReadTimeout(int timeoutMs) {
    m_impl->readTimeoutMs = timeoutMs;
}

// =============================================================================
// FFmpegコールバック
// =============================================================================

int HlsCustomAVIOContext::ReadPacket(void* opaque, uint8_t* buf, int bufSize) {
    HlsCustomAVIOContext* self = static_cast<HlsCustomAVIOContext*>(opaque);
    if (!self || !self->m_impl->initialized) {
        return AVERROR(EIO);
    }
    
    // 0バイト読み取り
    if (bufSize <= 0) {
        return 0;
    }
    
    auto& impl = *self->m_impl;
    int totalBytesRead = 0;
    
    while (totalBytesRead < bufSize) {
        // 読み取りに必要な情報をロック内で取得
        int64_t segmentToRead;
        int64_t offsetInSegment;
        int timeout;
        int64_t totalSegments;
        HlsSegmentCache* cache;
        
        {
            std::lock_guard<std::mutex> lock(impl.mutex);
            
            // EOF判定
            if (impl.position >= impl.totalSize) {
                if (totalBytesRead > 0) {
                    break;
                }
                LOG_TRACE("HlsCustomAVIOContext: ReadPacket EOF at position {}", impl.position);
                return AVERROR_EOF;
            }
            
            // 現在のセグメントインデックスを確認
            totalSegments = static_cast<int64_t>(impl.playlist.segments.size());
            
            // indexが範囲外かつ初期化セグメントでない場合
            if (impl.currentSegmentIndex != -1 && impl.currentSegmentIndex >= totalSegments) {
                // 全セグメント終了
                if (totalBytesRead > 0) {
                    break;
                }
                return AVERROR_EOF;
            }
            
            segmentToRead = impl.currentSegmentIndex;
            offsetInSegment = impl.currentSegmentOffset;
            timeout = impl.readTimeoutMs;
            cache = impl.cache;
        }
        
        // ロック外でデータ取得（待機可能）
        auto segmentData = cache->ReadSegment(segmentToRead, timeout);
        
        if (!segmentData) {
            // タイムアウトまたはデータなし
            if (totalBytesRead > 0) {
                // 部分的に読み取れた場合はそれを返す
                break;
            }
            LOG_WARN("HlsCustomAVIOContext: ReadSegment timeout for segment {}", 
                     segmentToRead);
            return AVERROR(EAGAIN);
        }
        
        // 再度ロックして状態更新
        {
            std::lock_guard<std::mutex> lock(impl.mutex);
            
            // セグメントが変わっていたらやり直し（シークされた場合）
            if (impl.currentSegmentIndex != segmentToRead) {
                // ループの先頭からやり直し
                continue;
            }
            
            // 実サイズでオフセットテーブルを更新
            int64_t actualSize = static_cast<int64_t>(segmentData->size());
            impl.UpdateSegmentSize(impl.currentSegmentIndex, actualSize);
            
            // 現在のセグメント内で読み取れるバイト数
            int64_t segmentRemaining = actualSize - impl.currentSegmentOffset;
            if (segmentRemaining <= 0) {
                // 次のセグメントへ移動
                if (impl.currentSegmentIndex == -1) {
                    impl.currentSegmentIndex = 0; // init -> seg0
                } else {
                    impl.currentSegmentIndex++;
                }
                impl.currentSegmentOffset = 0;
                continue;
            }
            
            // 読み取りサイズを決定
            int bytesToRead = std::min<int>(
                bufSize - totalBytesRead,
                static_cast<int>(segmentRemaining)
            );
            
            // データをコピー
            std::memcpy(
                buf + totalBytesRead,
                segmentData->data() + impl.currentSegmentOffset,
                bytesToRead
            );
            
            totalBytesRead += bytesToRead;
            impl.currentSegmentOffset += bytesToRead;
            impl.position += bytesToRead;
            
            // セグメント終端に達したら次のセグメントへ
            if (impl.currentSegmentOffset >= actualSize) {
                if (impl.currentSegmentIndex == -1) {
                    impl.currentSegmentIndex = 0;
                } else {
                    impl.currentSegmentIndex++;
                }
                impl.currentSegmentOffset = 0;
            }
        }
    }
    
    LOG_TRACE("HlsCustomAVIOContext: ReadPacket read {} bytes", totalBytesRead);
    
    return totalBytesRead;
}

int64_t HlsCustomAVIOContext::SeekCallback(void* opaque, int64_t offset, int whence) {
    HlsCustomAVIOContext* self = static_cast<HlsCustomAVIOContext*>(opaque);
    if (!self) {
        return -1;
    }
    
    return self->Seek(offset, whence);
}

}  // namespace hls
}  // namespace ytdlpspout
