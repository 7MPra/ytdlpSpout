// =============================================================================
// YtDlpResolver.h - yt-dlp連携モジュール
// =============================================================================
//
// 機能:
//   - YouTube等のURLからストリームURL取得
//   - メタデータ（タイトル、解像度等）取得
//   - フォーマット選択ロジック
//   - Windows CreateProcess経由でyt-dlp呼び出し
//
// 使用例:
//   YtDlpResolver resolver;
//   if (YtDlpResolver::IsSupportedUrl(url)) {
//       auto streamUrl = resolver.GetStreamUrl(url, 1080);
//       if (streamUrl) {
//           // CustomIOContextで再生
//       }
//   }
//
// =============================================================================

#pragma once

#include <string>
#include <vector>
#include <optional>
#include <cstdint>

namespace ytdlpspout {
namespace ytdlp {

// =============================================================================
// データ構造
// =============================================================================

/// @brief フォーマット情報
struct FormatInfo {
    std::string formatId;       // フォーマットID（例: "137", "22"）
    std::string url;            // ストリームURL
    int width = 0;              // 幅（ピクセル）
    int height = 0;             // 高さ（ピクセル）
    int fps = 0;                // フレームレート
    std::string vcodec;         // ビデオコーデック（例: "avc1.640028"）
    std::string acodec;         // オーディオコーデック（例: "mp4a.40.2"）
    int64_t filesize = 0;       // ファイルサイズ（バイト）
    int tbr = 0;                // ビットレート（kbps）
    std::string ext;            // 拡張子（例: "mp4", "webm"）
    bool hasVideo = false;      // ビデオトラックあり
    bool hasAudio = false;      // オーディオトラックあり
};

/// @brief 動画メタデータ
struct VideoMetadata {
    std::string id;             // 動画ID
    std::string title;          // タイトル
    std::string uploader;       // アップローダー名
    double duration = 0.0;      // 再生時間（秒）
    std::vector<FormatInfo> formats;  // 利用可能なフォーマット
    bool isLive = false;        // ライブ配信か
    std::string thumbnailUrl;   // サムネイルURL
};

/// @brief ソースタイプ
enum class SourceType {
    Unknown,        // 不明
    LocalFile,      // ローカルファイル
    HttpUrl,        // 直接HTTP URL（動画ファイル直リンク）
    YtDlpUrl        // yt-dlp対応URL（YouTube等）
};

// =============================================================================
// YtDlpResolver クラス
// =============================================================================

/// @brief yt-dlp連携クラス
/// @details YouTube等のURLから再生可能なストリームURLを取得
class YtDlpResolver {
public:
    YtDlpResolver();
    ~YtDlpResolver();

    // コピー禁止
    YtDlpResolver(const YtDlpResolver&) = delete;
    YtDlpResolver& operator=(const YtDlpResolver&) = delete;

    // =========================================================================
    // 設定
    // =========================================================================

    /// @brief yt-dlpの実行ファイルパスを設定
    /// @param path パス（空の場合はデフォルト "yt-dlp"）
    void SetYtDlpPath(const std::string& path);

    /// @brief yt-dlpのパスを取得
    /// @return 現在のパス
    const std::string& GetYtDlpPath() const;

    /// @brief タイムアウトを設定
    /// @param timeoutMs タイムアウト（ミリ秒）
    void SetTimeout(int timeoutMs);

    // =========================================================================
    // URL解決
    // =========================================================================

    /// @brief URLからメタデータを取得
    /// @param url 動画URL
    /// @return メタデータ（失敗時はnullopt）
    std::optional<VideoMetadata> ResolveUrl(const std::string& url);

    /// @brief ストリームURLを直接取得
    /// @param url 動画URL
    /// @param preferredHeight 希望する高さ（デフォルト: 1080）
    /// @return ストリームURL（失敗時はnullopt）
    std::optional<std::string> GetStreamUrl(const std::string& url, int preferredHeight = 1080);

    // =========================================================================
    // フォーマット選択
    // =========================================================================

    /// @brief メタデータからベストフォーマットを選択
    /// @param metadata 動画メタデータ
    /// @param preferredHeight 希望する高さ
    /// @return 選択されたフォーマット（失敗時はnullopt）
    std::optional<FormatInfo> SelectBestFormat(const VideoMetadata& metadata, int preferredHeight);

    // =========================================================================
    // JSON解析（テスト用に公開）
    // =========================================================================

    /// @brief JSONからメタデータを解析
    /// @param json JSON文字列
    /// @return メタデータ（失敗時はnullopt）
    std::optional<VideoMetadata> ParseMetadataJson(const std::string& json);

    // =========================================================================
    // 静的ユーティリティ
    // =========================================================================

    /// @brief URLがyt-dlp対応かどうか判定
    /// @param url URL
    /// @return 対応している場合true
    static bool IsSupportedUrl(const std::string& url);

    /// @brief URLのソースタイプを判定
    /// @param pathOrUrl パスまたはURL
    /// @return ソースタイプ
    static SourceType GetSourceType(const std::string& pathOrUrl);

private:
    /// @brief yt-dlpコマンドを実行
    /// @param args コマンドライン引数
    /// @param output 出力先
    /// @return 成功した場合true
    bool ExecuteYtDlp(const std::vector<std::string>& args, std::string& output);

    /// @brief コーデックがHWデコード対応かどうか
    /// @param vcodec コーデック名
    /// @return 対応している場合true
    static bool IsHwDecodableCodec(const std::string& vcodec);

    std::string m_ytdlpPath = "yt-dlp";
    int m_timeoutMs = 30000;
};

} // namespace ytdlp
} // namespace ytdlpspout
