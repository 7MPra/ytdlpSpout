// =============================================================================
// HlsSegmentCache.h - HLSセグメントのメモリキャッシュ
// =============================================================================
//
// 機能:
//   - HLSセグメントのメモリ内キャッシュ管理
//   - LRUエビクション（メモリ制限超過時）
//   - AES-128-CBC復号のサポート
//   - 時間→セグメントインデックス変換
//   - スレッドセーフな読み書き
//
// 使用例:
//   HlsSegmentCacheConfig config;
//   config.maxMemoryBytes = 256 * 1024 * 1024;  // 256MB
//   
//   HlsSegmentCache cache(config);
//   cache.Initialize(playlist);
//   
//   // 暗号化キーを設定（必要な場合）
//   cache.SetEncryptionKey(keyData);
//   
//   // セグメントを書き込み
//   cache.WriteSegment(0, std::move(segmentData), isEncrypted);
//   
//   // セグメントを読み取り
//   auto data = cache.ReadSegment(0);
//
// =============================================================================

#pragma once

#include "hls/M3U8Parser.h"

#include <vector>
#include <optional>
#include <memory>
#include <cstdint>

namespace ytdlpspout {
namespace hls {

// =============================================================================
// 設定構造体
// =============================================================================

/// @brief HLSセグメントキャッシュ設定
struct HlsSegmentCacheConfig {
    /// @brief キャッシュの最大メモリサイズ（バイト）
    size_t maxMemoryBytes = 256 * 1024 * 1024;  // 256MB
    
    /// @brief キャッシュする最大セグメント数
    size_t maxSegments = 100;
};

// =============================================================================
// キャッシュクラス
// =============================================================================

/// @brief HLSセグメントのメモリキャッシュ
/// 
/// スレッドセーフなLRUキャッシュを提供し、HLSセグメントのメモリ管理を行う。
/// AES-128-CBC暗号化されたセグメントの復号もサポート。
class HlsSegmentCache {
public:
    // =========================================================================
    // コンストラクタ / デストラクタ
    // =========================================================================
    
    /// @brief コンストラクタ
    /// @param config キャッシュ設定
    explicit HlsSegmentCache(const HlsSegmentCacheConfig& config = HlsSegmentCacheConfig{});
    
    /// @brief デストラクタ
    ~HlsSegmentCache();
    
    // コピー禁止
    HlsSegmentCache(const HlsSegmentCache&) = delete;
    HlsSegmentCache& operator=(const HlsSegmentCache&) = delete;
    
    // =========================================================================
    // 初期化
    // =========================================================================
    
    /// @brief プレイリストで初期化
    /// @param playlist HLSプレイリスト
    void Initialize(const M3U8Playlist& playlist);
    
    /// @brief 暗号化キーを設定（キーデータは既にダウンロード済み）
    /// @param keyData 16バイトのAES-128キー
    /// @param explicitIv 明示的なIV（指定しない場合はメディアシーケンスから生成）
    void SetEncryptionKey(const std::vector<uint8_t>& keyData, 
                          const std::optional<std::vector<uint8_t>>& explicitIv = std::nullopt);
    
    // =========================================================================
    // 読み書き操作
    // =========================================================================
    
    /// @brief セグメントデータを書き込み
    /// @param segmentIndex セグメントインデックス（0始まり）
    /// @param data 暗号化されたまたは生データ
    /// @param isEncrypted 暗号化済みデータかどうか
    /// @return 成功時true
    bool WriteSegment(int64_t segmentIndex, std::vector<uint8_t>&& data, bool isEncrypted);
    
    /// @brief セグメントデータを読み取り（復号済み、LRU更新）
    /// @param segmentIndex セグメントインデックス
    /// @param timeoutMs 待機タイムアウト（0=即時、-1=無制限）
    /// @return データ（キャッシュミス/タイムアウト時nullopt）
    std::optional<std::vector<uint8_t>> ReadSegment(int64_t segmentIndex, int timeoutMs = 0);
    
    // =========================================================================
    // 状態確認
    // =========================================================================
    
    /// @brief キャッシュ済みか確認
    /// @param segmentIndex セグメントインデックス
    /// @return キャッシュ済みならtrue
    bool IsSegmentCached(int64_t segmentIndex) const;
    
    /// @brief 指定セグメントがキャッシュされるまで待機
    /// @param segmentIndex セグメントインデックス
    /// @param timeoutMs タイムアウト（0=無制限、正の値=タイムアウト）
    /// @return キャッシュされたらtrue、タイムアウトでfalse
    bool WaitForSegment(int64_t segmentIndex, int timeoutMs = 0);
    
    /// @brief セグメント情報を取得
    /// @param index セグメントインデックス
    /// @return セグメント情報へのポインタ（範囲外ならnullptr）
    const HlsSegment* GetSegmentInfo(int64_t index) const;
    
    /// @brief 総セグメント数
    /// @return プレイリスト内のセグメント総数
    size_t GetTotalSegmentCount() const;
    
    /// @brief キャッシュ済みセグメント数
    /// @return 現在キャッシュされているセグメント数
    size_t GetCachedSegmentCount() const;
    
    /// @brief 現在のキャッシュメモリ使用量
    /// @return 使用メモリ量（バイト）
    size_t GetCacheMemoryUsage() const;
    
    // =========================================================================
    // 時間変換
    // =========================================================================
    
    /// @brief 時間（秒）からセグメントインデックスを計算
    /// @param seconds 再生位置（秒）
    /// @return セグメントインデックス（範囲外の場合はクランプ）
    int64_t GetSegmentIndexFromTime(double seconds) const;
    
    /// @brief セグメントの開始時間を取得
    /// @param segmentIndex セグメントインデックス
    /// @return セグメント開始時間（秒）
    double GetSegmentStartTime(int64_t segmentIndex) const;
    
    // =========================================================================
    // 最適化
    // =========================================================================
    
    /// @brief キャッシュを指定セグメント周辺に最適化（LRU調整）
    /// @param currentSegmentIndex 現在再生中のセグメントインデックス
    /// @param prefetchCount 先読みするセグメント数
    void OptimizeForPlayback(int64_t currentSegmentIndex, int64_t prefetchCount);
    
private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

}  // namespace hls
}  // namespace ytdlpspout
