// =============================================================================
// SliceLoadingManager.h - スライス読み込みマネージャー
// =============================================================================
//
// 機能:
//   - URL/ファイルを統一的に扱うスライス読み込み制御
//   - yt-dlp URLの自動解決
//   - CustomIOContextの管理
//   - プリフェッチ最適化
//
// =============================================================================

#pragma once

#include <string>
#include <memory>
#include <cstdint>
#include <map>

struct AVIOContext;

namespace ytdlpspout {
namespace io {

// 前方宣言
class CustomIOContext;

/// @brief スライス読み込み設定
struct SliceLoadingConfig {
    size_t chunkSize = 2 * 1024 * 1024;           // チャンクサイズ（2MB）- 高解像度向け
    size_t maxCacheMemory = 256 * 1024 * 1024;    // 最大キャッシュメモリ（256MB）
    int maxConcurrentDownloads = 6;               // 最大並列ダウンロード数（6ワーカー）
    int prefetchChunksAhead = 24;                 // 先読みチャンク数（48MB）
    int criticalChunksAhead = 6;                  // 最優先チャンク数（即座にダウンロード）
    bool enableContinuousDownload = true;         // ファイル全体を継続ダウンロードするか
    std::string cachePath;                        // ファイルキャッシュパス（空=メモリのみ）
    std::string ytdlpPath;                        // yt-dlpパス（空=自動検出）
    int preferredHeight = 1080;                   // 希望解像度
    std::map<std::string, std::string> httpHeaders;  // HTTPヘッダー（Cookie等）
};

/// @brief ソースタイプ
enum class SliceSourceType {
    Unknown,
    LocalFile,
    HttpUrl,
    YtDlpUrl
};

/// @brief スライス読み込みマネージャー
/// @details URL/ファイルを統一的に扱うスライス読み込み制御
class SliceLoadingManager {
public:
    SliceLoadingManager();
    ~SliceLoadingManager();

    // コピー禁止
    SliceLoadingManager(const SliceLoadingManager&) = delete;
    SliceLoadingManager& operator=(const SliceLoadingManager&) = delete;

    // =========================================================================
    // 初期化
    // =========================================================================

    /// @brief ソースを開く
    /// @param source ファイルパス or URL
    /// @param config スライス設定
    /// @return 成功時true
    bool Open(const std::string& source, const SliceLoadingConfig& config = {});

    /// @brief クローズ
    void Close();

    /// @brief 開いているか
    /// @return 開いている場合true
    bool IsOpen() const;

    // =========================================================================
    // FFmpeg連携
    // =========================================================================

    /// @brief AVIOContextを取得
    /// @return AVIOContextポインタ（所有権は移動しない、未初期化時nullptr）
    AVIOContext* GetAVIOContext();

    /// @brief CustomIOContextを取得（所有権は移動しない）
    /// @return CustomIOContextポインタ（未初期化時nullptr）
    CustomIOContext* GetCustomIOContext();

    /// @brief ファイルサイズを取得
    /// @return ファイルサイズ（未初期化時0）
    int64_t GetFileSize() const;

    /// @brief ソースタイプを取得
    /// @return ソースタイプ
    SliceSourceType GetSourceType() const;

    /// @brief 解決後のURLを取得
    /// @return 解決後のURL（ローカルファイルの場合はパス、yt-dlp URLの場合はストリームURL）
    const std::string& GetResolvedUrl() const;

    // =========================================================================
    // 再生位置連携
    // =========================================================================

    /// @brief 再生位置を更新（プリフェッチ最適化用）
    /// @param seconds 秒単位の再生位置
    void UpdatePlaybackPosition(double seconds);

    /// @brief 再生位置を更新（バイトオフセット）
    /// @param byteOffset バイトオフセット
    void UpdatePlaybackPositionBytes(int64_t byteOffset);

    /// @brief シーク通知（秒単位）
    /// @param seconds シーク先の秒数
    void NotifySeek(double seconds);

    /// @brief シーク通知（バイトオフセット）
    /// @param byteOffset シーク先のバイトオフセット
    void NotifySeekBytes(int64_t byteOffset);

    // =========================================================================
    // 統計
    // =========================================================================

    /// @brief ダウンロード進捗を取得（0.0〜1.0）
    /// @return ダウンロード進捗
    double GetDownloadProgress() const;

    /// @brief 推定帯域幅を取得
    /// @return 帯域幅（バイト/秒）
    double GetBandwidth() const;

    /// @brief 全チャンクがキャッシュ済みか
    /// @return キャッシュ済みの場合true
    bool IsFullyCached() const;

    /// @brief キャッシュ済みチャンク数を取得
    /// @return キャッシュ済みチャンク数
    size_t GetCachedChunkCount() const;

    /// @brief 総チャンク数を取得
    /// @return 総チャンク数
    size_t GetTotalChunkCount() const;

    // =========================================================================
    // 動画情報（yt-dlp解決後）
    // =========================================================================

    /// @brief 動画の再生時間を取得
    /// @return 再生時間（秒）、不明の場合0
    double GetDuration() const;

    /// @brief 動画のビットレートを取得
    /// @return ビットレート（bps）、不明の場合0
    int GetBitrate() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace io
} // namespace ytdlpspout
