// =============================================================================
// ChunkDownloader.h - チャンク並列ダウンローダー
// =============================================================================
//
// 機能:
//   - スレッドプールによる並列ダウンロード
//   - 優先度付きリクエストキュー
//   - SparseFileCacheへの自動書き込み
//   - 帯域幅推定
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
// =============================================================================

#pragma once

#include <string>
#include <map>
#include <memory>
#include <cstdint>

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
