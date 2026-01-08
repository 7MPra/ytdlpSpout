// =============================================================================
// PrefetchScheduler.h - プリフェッチスケジューラー
// =============================================================================
//
// 機能:
//   - 再生位置に応じた優先度計算
//   - 先読みバッファ管理
//   - プリフェッチ戦略の実装
//
// =============================================================================

#pragma once

#include <memory>
#include <cstdint>
#include <vector>

namespace ytdlpspout {
namespace io {

// 前方宣言
class SparseFileCache;
class ChunkDownloader;

/// @brief プリフェッチ設定
struct PrefetchConfig {
    int prefetchChunksAhead = 24;       // 先読みチャンク数（48MB）
    int prefetchChunksBehind = 2;       // 後方保持チャンク数
    int criticalChunksAhead = 6;        // 最優先チャンク数（即座にダウンロード）
    int updateIntervalMs = 50;          // 更新間隔（ミリ秒）
    bool enableContinuousDownload = true; // 継続ダウンロード有効
    int continuousDownloadBatch = 48;   // 継続ダウンロードのバッチサイズ
};

/// @brief プリフェッチスケジューラー
class PrefetchScheduler {
public:
    PrefetchScheduler();
    ~PrefetchScheduler();

    // コピー禁止
    PrefetchScheduler(const PrefetchScheduler&) = delete;
    PrefetchScheduler& operator=(const PrefetchScheduler&) = delete;

    // =========================================================================
    // 初期化
    // =========================================================================

    /// @brief 初期化
    /// @param cache スパースファイルキャッシュ
    /// @param downloader チャンクダウンローダー
    /// @param config 設定
    void Initialize(
        SparseFileCache* cache,
        ChunkDownloader* downloader,
        const PrefetchConfig& config = {}
    );

    /// @brief シャットダウン
    void Shutdown();

    // =========================================================================
    // 再生位置管理
    // =========================================================================

    /// @brief 現在の再生位置を更新
    /// @param byteOffset 現在のバイトオフセット
    void UpdatePlaybackPosition(int64_t byteOffset);

    /// @brief シーク通知
    /// @param byteOffset シーク先のバイトオフセット
    void NotifySeek(int64_t byteOffset);

    /// @brief 現在の再生位置を取得
    /// @return 現在のバイトオフセット
    int64_t GetCurrentPosition() const;

    // =========================================================================
    // プリフェッチ制御
    // =========================================================================

    /// @brief プリフェッチを開始
    void Start();

    /// @brief プリフェッチを停止
    void Stop();

    /// @brief 動作中か
    /// @return 動作中の場合true
    bool IsRunning() const;

    /// @brief プリフェッチ対象のチャンクを計算
    /// @return チャンクインデックスの配列（優先度順）
    std::vector<int64_t> GetPrefetchChunks() const;

    /// @brief 手動でプリフェッチを実行
    void TriggerPrefetch();

    // =========================================================================
    // 設定
    // =========================================================================

    /// @brief 設定を更新
    /// @param config 新しい設定
    void Configure(const PrefetchConfig& config);

    /// @brief 現在の設定を取得
    /// @return 現在の設定
    const PrefetchConfig& GetConfig() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace io
} // namespace ytdlpspout
