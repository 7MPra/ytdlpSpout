// =============================================================================
// ChunkDownloader.h - チャンク並列ダウンローダー
// =============================================================================
//
// 機能:
//   - スレッドプールによる並列ダウンロード
//   - 優先度付きリクエストキュー
//   - SparseFileCacheへの自動書き込み
//   - 帯域幅推定
//   - HLSセグメントダウンロード（v2.0追加）
//
// 使用方法:
//   SparseFileCache cache;
//   cache.Initialize(fileSize);
//   
//   ChunkDownloader downloader(&cache, 4);  // 4ワーカー
//   downloader.SetUrl("http://example.com/video.mp4");
//   downloader.Start();
//   downloader.RequestChunk(0, ChunkPriority::Critical);
//
// HLSセグメントダウンロード:
//   downloader.SetSegmentDownloadCallback([](int64_t idx, std::vector<uint8_t>&& data, bool ok) {
//       // セグメントデータを処理
//   });
//   downloader.RequestSegment("http://example.com/seg0.ts", 0, ChunkPriority::High);
//
// =============================================================================

#pragma once

#include <string>
#include <map>
#include <memory>
#include <cstdint>
#include <vector>
#include <functional>

namespace ytdlpspout {
namespace io {

// 前方宣言
class SparseFileCache;

/// @brief チャンクダウンロードの優先度
enum class ChunkPriority {
    Critical = 0,   // 最高優先度（再生に必要なチャンク）
    High = 1,       // 高優先度
    Medium = 2,     // 通常優先度
    Low = 3         // 低優先度（先読み）
};

/// @brief HLSセグメントのダウンロード完了コールバック
/// @param segmentIndex セグメントインデックス
/// @param data ダウンロードしたデータ（失敗時は空）
/// @param success 成功フラグ
using SegmentDownloadCallback = std::function<void(int64_t segmentIndex, std::vector<uint8_t>&& data, bool success)>;

/// @brief チャンク並列ダウンローダークラス
/// 
/// スレッドプールを使用して複数のチャンクを並列でダウンロードし、
/// SparseFileCacheに書き込む。優先度付きキューでリクエストを管理し、
/// 再生に必要なチャンクを優先的にダウンロードする。
class ChunkDownloader {
public:
    /// @brief コンストラクタ
    /// @param cache 書き込み先のSparseFileCache（nullの場合例外）
    /// @param numWorkers ワーカースレッド数（1以上、デフォルト4）
    explicit ChunkDownloader(SparseFileCache* cache, int numWorkers = 4);
    
    /// @brief デストラクタ（自動的にStopを呼び出す）
    ~ChunkDownloader();
    
    // コピー禁止
    ChunkDownloader(const ChunkDownloader&) = delete;
    ChunkDownloader& operator=(const ChunkDownloader&) = delete;
    
    // ムーブ禁止
    ChunkDownloader(ChunkDownloader&&) = delete;
    ChunkDownloader& operator=(ChunkDownloader&&) = delete;
    
    // =========================================================================
    // 設定
    // =========================================================================
    
    /// @brief ダウンロードURLを設定
    /// @param url ダウンロード元URL
    void SetUrl(const std::string& url);
    
    /// @brief HTTPヘッダーを設定
    /// @param headers ヘッダーマップ（User-Agent、Cookie等）
    void SetHeaders(const std::map<std::string, std::string>& headers);
    
    /// @brief HTTPヘッダーを設定（全リクエストに適用）
    /// @param headers ヘッダーマップ
    /// @note SetHeadersと同じ機能だが、HLS用に追加されたAPI
    void SetHttpHeaders(const std::map<std::string, std::string>& headers);
    
    // =========================================================================
    // ダウンロード制御
    // =========================================================================
    
    /// @brief ダウンロードを開始（ワーカースレッドを起動）
    void Start();
    
    /// @brief ダウンロードを停止（すべてのワーカースレッドを終了）
    void Stop();
    
    /// @brief チャンクのダウンロードをリクエスト
    /// @param offset チャンクのバイトオフセット
    /// @param priority ダウンロード優先度
    void RequestChunk(int64_t offset, ChunkPriority priority = ChunkPriority::Medium);
    
    /// @brief チャンクのダウンロードをキャンセル
    /// @param offset キャンセルするチャンクのバイトオフセット
    void CancelChunk(int64_t offset);
    
    // =========================================================================
    // HLSセグメントダウンロード
    // =========================================================================
    
    /// @brief セグメントダウンロード完了コールバックを設定
    /// @param callback ダウンロード完了時に呼ばれるコールバック
    void SetSegmentDownloadCallback(SegmentDownloadCallback callback);
    
    /// @brief HLSセグメントURLのダウンロードをリクエスト
    /// @param url セグメントURL（完全URL）
    /// @param segmentIndex セグメントインデックス
    /// @param priority 優先度（Critical, High, Medium, Low）
    /// @param byteRangeStart バイト範囲開始位置（-1の場合は範囲指定なし）
    /// @param byteRangeLength バイト範囲の長さ
    void RequestSegment(const std::string& url, int64_t segmentIndex, ChunkPriority priority,
                       int64_t byteRangeStart = -1, int64_t byteRangeLength = 0);
    
    /// @brief キューに入っているセグメントの優先度を変更
    /// @param segmentIndex セグメントインデックス
    /// @param newPriority 新しい優先度
    void ReprioritizeSegment(int64_t segmentIndex, ChunkPriority newPriority);
    
    /// @brief セグメントダウンロードキューをクリア
    void ClearSegmentQueue();
    
    // =========================================================================
    // 統計
    // =========================================================================
    
    /// @brief 推定帯域幅を取得
    /// @return 帯域幅（バイト/秒）
    double GetBandwidth() const;
    
    /// @brief ダウンロードが実行中かどうか
    /// @return 実行中の場合true
    bool IsRunning() const;
    
private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace io
} // namespace ytdlpspout
