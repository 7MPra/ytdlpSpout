// =============================================================================
// SparseFileCache.h - スパースファイルキャッシュ
// =============================================================================
//
// 機能:
//   - チャンク単位のメモリキャッシュ
//   - LRUベースのメモリ制限管理
//   - スレッドセーフな読み書き
//   - チャンク状態管理（Pending, Downloading, Cached）
//
// =============================================================================

#pragma once

#include <vector>
#include <memory>
#include <cstdint>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <optional>

namespace ytdlpspout {
namespace io {

/// @brief チャンクの状態
enum class ChunkState {
    Empty,          // 未ダウンロード
    Pending,        // ダウンロード待ち
    Downloading,    // ダウンロード中
    Cached,         // キャッシュ済み
    Error           // エラー
};

/// @brief チャンク情報
struct ChunkInfo {
    int64_t chunkIndex = 0;         // チャンクインデックス
    int64_t byteOffset = 0;         // バイトオフセット
    size_t size = 0;                // チャンクサイズ
    ChunkState state = ChunkState::Empty;
    int64_t lastAccessTime = 0;     // 最終アクセス時刻（LRU用）
};

/// @brief スパースファイルキャッシュ設定
struct SparseFileCacheConfig {
    size_t chunkSize = 1 * 1024 * 1024;        // チャンクサイズ（デフォルト1MB）
    size_t maxMemoryBytes = 64 * 1024 * 1024;  // 最大メモリ使用量（デフォルト64MB）
    size_t maxChunks = 0;                       // 最大チャンク数（0=自動計算）
};

/// @brief チャンクデータ取得コールバック
/// @param chunkIndex チャンクインデックス
/// @param byteOffset バイトオフセット
/// @param size チャンクサイズ
using ChunkRequestCallback = std::function<void(int64_t chunkIndex, int64_t byteOffset, size_t size)>;

/// @brief チャンクの再試行上限回数
/// @details この回数を超えてError状態に遷移したチャンク（GetChunkFailCount() > kMaxChunkFailCount）は
///          恒久的失敗とみなす。PrefetchSchedulerはこれを超えたチャンクを再リクエストせず、
///          SparseFileCache::Read()はタイムアウトを待たず即座にエラーを返す。
constexpr int kMaxChunkFailCount = 2;

/// @brief スパースファイルキャッシュクラス
class SparseFileCache {
public:
    explicit SparseFileCache(const SparseFileCacheConfig& config = {});
    ~SparseFileCache();

    // コピー禁止
    SparseFileCache(const SparseFileCache&) = delete;
    SparseFileCache& operator=(const SparseFileCache&) = delete;

    // =========================================================================
    // 初期化
    // =========================================================================

    /// @brief ファイルサイズを設定して初期化
    /// @param totalSize ファイルの総サイズ
    void Initialize(int64_t totalSize);

    /// @brief キャッシュをクリア
    void Clear();

    /// @brief 初期化済みか
    /// @return 初期化済みの場合true
    bool IsInitialized() const;

    // =========================================================================
    // チャンク操作
    // =========================================================================

    /// @brief チャンクの状態を取得
    /// @param chunkIndex チャンクインデックス
    /// @return チャンクの状態
    ChunkState GetChunkState(int64_t chunkIndex) const;

    /// @brief チャンクの状態を設定
    /// @param chunkIndex チャンクインデックス
    /// @param state 新しい状態
    void SetChunkState(int64_t chunkIndex, ChunkState state);

    /// @brief チャンクの失敗回数を取得
    /// @param chunkIndex チャンクインデックス
    /// @return Error状態への累積遷移回数（WriteChunk成功時に0にリセットされる）
    int GetChunkFailCount(int64_t chunkIndex) const;

    /// @brief チャンクデータを書き込み
    /// @param chunkIndex チャンクインデックス
    /// @param data データ
    /// @param size データサイズ
    /// @return 成功した場合true
    bool WriteChunk(int64_t chunkIndex, const uint8_t* data, size_t size);

    /// @brief チャンクデータを読み取り
    /// @param chunkIndex チャンクインデックス
    /// @param buffer 出力バッファ
    /// @param bufferSize バッファサイズ
    /// @return 読み取ったバイト数（-1=エラー）
    int64_t ReadChunk(int64_t chunkIndex, uint8_t* buffer, size_t bufferSize);

    /// @brief バイト位置からデータを読み取り（ブロッキング）
    /// @param offset バイトオフセット
    /// @param buffer 出力バッファ
    /// @param size 読み取りサイズ
    /// @param timeoutMs タイムアウト（ミリ秒、0=無制限）
    /// @return 読み取ったバイト数（-1=エラー、0=タイムアウト）
    int64_t Read(int64_t offset, uint8_t* buffer, size_t size, int timeoutMs = 0);

    /// @brief チャンクがキャッシュ済みになるまで待機
    /// @param chunkIndex チャンクインデックス
    /// @param timeoutMs タイムアウト（ミリ秒、0=無制限）
    /// @return キャッシュ済みになった場合true
    bool WaitForChunk(int64_t chunkIndex, int timeoutMs = 0);

    // =========================================================================
    // 情報取得
    // =========================================================================

    /// @brief 総ファイルサイズを取得
    /// @return ファイルサイズ
    int64_t GetTotalSize() const;

    /// @brief チャンク数を取得
    /// @return 総チャンク数
    int64_t GetChunkCount() const;

    /// @brief チャンクサイズを取得
    /// @return チャンクサイズ（バイト）
    size_t GetChunkSize() const;

    /// @brief 特定チャンクの実際のサイズを取得（最後のチャンクは小さい可能性）
    /// @param chunkIndex チャンクインデックス
    /// @return チャンクサイズ
    size_t GetActualChunkSize(int64_t chunkIndex) const;

    /// @brief バイトオフセットからチャンクインデックスを計算
    /// @param byteOffset バイトオフセット
    /// @return チャンクインデックス
    int64_t GetChunkIndex(int64_t byteOffset) const;

    /// @brief チャンクインデックスからバイトオフセットを計算
    /// @param chunkIndex チャンクインデックス
    /// @return バイトオフセット
    int64_t GetByteOffset(int64_t chunkIndex) const;

    /// @brief 現在のメモリ使用量を取得
    /// @return メモリ使用量（バイト）
    size_t GetMemoryUsage() const;

    /// @brief キャッシュ済みチャンク数を取得
    /// @return キャッシュ済みチャンク数
    int64_t GetCachedChunkCount() const;

    /// @brief ダウンロード完了済みチャンク数を取得（LRU削除されても減らない）
    /// @return ダウンロード完了済みチャンク数
    int64_t GetDownloadedChunkCount() const;

    // =========================================================================
    // チャンク要求コールバック
    // =========================================================================

    /// @brief チャンク要求コールバックを設定
    /// @param callback コールバック関数
    void SetChunkRequestCallback(ChunkRequestCallback callback);

    /// @brief チャンクを要求
    /// @param chunkIndex チャンクインデックス
    void RequestChunk(int64_t chunkIndex);

private:
    /// @brief LRUによる古いチャンクの削除
    void EvictOldChunks();

    /// @brief チャンクが利用可能になったことを通知
    void NotifyChunkAvailable(int64_t chunkIndex);

    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace io
} // namespace ytdlpspout
