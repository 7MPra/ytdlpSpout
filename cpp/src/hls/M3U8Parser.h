// =============================================================================
// M3U8Parser.h - HLS m3u8プレイリストパーサー
// =============================================================================
//
// 機能:
//   - HLS m3u8プレイリストのパース
//   - セグメント情報の抽出（URL、duration、byterange）
//   - 暗号化キー情報の抽出（AES-128、SAMPLE-AES）
//   - 相対URLから絶対URLへの解決
//   - ライブ/VODプレイリストの判定
//
// 対応タグ:
//   - #EXTM3U
//   - #EXT-X-VERSION
//   - #EXT-X-TARGETDURATION
//   - #EXT-X-MEDIA-SEQUENCE
//   - #EXTINF
//   - #EXT-X-KEY
//   - #EXT-X-BYTERANGE
//   - #EXT-X-ENDLIST
//
// =============================================================================

#pragma once

#include <string>
#include <vector>
#include <optional>
#include <cstdint>

namespace ytdlpspout {
namespace hls {

/// @brief HLSセグメント情報
struct HlsSegment {
    int64_t index = 0;                    ///< セグメントインデックス（プレイリスト内）
    std::string url;                      ///< セグメントURL（絶対URL）
    double duration = 0.0;                ///< セグメントの長さ（秒）
    int64_t byteRangeStart = -1;          ///< バイト範囲開始位置（-1 = 未指定）
    int64_t byteRangeLength = 0;          ///< バイト範囲の長さ
    int64_t mediaSequence = 0;            ///< メディアシーケンス番号
};

/// @brief HLS暗号化キー情報
struct HlsEncryptionKey {
    std::string method;                   ///< 暗号化方式（"NONE", "AES-128", "SAMPLE-AES"）
    std::string keyUrl;                   ///< キーファイルのURL
    std::vector<uint8_t> iv;              ///< 初期化ベクトル（16バイト）
};

/// @brief HlsMap情報（初期化セグメント）
struct HlsMap {
    std::string url;                      ///< 初期化セグメントURL
    int64_t byteRangeStart = -1;          ///< バイト範囲開始位置
    int64_t byteRangeLength = 0;          ///< バイト範囲の長さ
};

/// @brief M3U8プレイリスト情報
struct M3U8Playlist {
    int version = 0;                      ///< プレイリストバージョン
    double targetDuration = 0.0;          ///< 最大セグメント長（秒）
    int64_t mediaSequence = 0;            ///< 開始メディアシーケンス番号
    bool isEndList = false;               ///< #EXT-X-ENDLISTが存在するか
    bool isLive = false;                  ///< ライブストリームか
    std::vector<HlsSegment> segments;     ///< セグメントリスト
    std::optional<HlsEncryptionKey> encryptionKey;  ///< 暗号化キー情報
    std::optional<HlsMap> map;            ///< 初期化セグメント情報 (#EXT-X-MAP)
    double totalDuration = 0.0;           ///< 全セグメントの合計時間
    bool hasKeyRotation = false;           ///< プレイリスト内で#EXT-X-KEYが途中で変化した（鍵ローテーション）か
};

/// @brief HLS品質バリアント情報（マスタープレイリスト用）
struct HlsVariant {
    std::string url;                      ///< サブプレイリストURL（絶対URL）
    int64_t bandwidth = 0;                ///< ビットレート（bps）
    int width = 0;                        ///< 解像度: 幅
    int height = 0;                       ///< 解像度: 高さ
    std::string codecs;                   ///< コーデック情報
    std::string name;                     ///< バリアント名（任意）
};

/// @brief マスタープレイリスト情報
struct MasterPlaylist {
    std::vector<HlsVariant> variants;     ///< 品質バリアントリスト
    bool isMaster = true;                 ///< マスタープレイリストフラグ
};

/// @brief M3U8プレイリストパーサー
class M3U8Parser {
public:
    // =========================================================================
    // パース
    // =========================================================================

    /// @brief M3U8プレイリストをパース
    /// @param m3u8Content m3u8ファイルの内容
    /// @param baseUrl ベースURL（相対URL解決用）
    /// @return パース結果（失敗時はnullopt）
    static std::optional<M3U8Playlist> Parse(
        const std::string& m3u8Content,
        const std::string& baseUrl
    );

    // =========================================================================
    // URL解決
    // =========================================================================

    /// @brief 相対URLを絶対URLに変換
    /// @param baseUrl ベースURL
    /// @param relativeUrl 相対URL（または絶対URL）
    /// @return 絶対URL
    static std::string ResolveUrl(
        const std::string& baseUrl,
        const std::string& relativeUrl
    );

    /// @brief URLからベースパスを取得（末尾のファイル名を除いた部分。クエリ/フラグメントは除去済み）
    /// @param url 対象URL
    /// @return ベースパス（末尾に'/'を含む）
    static std::string GetBasePath(const std::string& url);

    // =========================================================================
    // マスタープレイリスト対応
    // =========================================================================

    /// @brief マスタープレイリストかどうかを判定
    /// @param m3u8Content m3u8ファイルの内容
    /// @return マスタープレイリストの場合true
    static bool IsMasterPlaylist(const std::string& m3u8Content);

    /// @brief マスタープレイリストをパース
    /// @param m3u8Content m3u8ファイルの内容
    /// @param baseUrl ベースURL（相対URL解決用）
    /// @return パース結果（失敗時はnullopt）
    static std::optional<MasterPlaylist> ParseMaster(
        const std::string& m3u8Content,
        const std::string& baseUrl
    );

    /// @brief 最適なバリアントを選択（帯域幅が高い順）
    /// @param master マスタープレイリスト
    /// @param preferredBandwidth 希望帯域幅（0 = 最高帯域幅を選択）
    /// @return 選択されたバリアント（失敗時はnullopt）
    static std::optional<HlsVariant> SelectBestVariant(
        const MasterPlaylist& master,
        int64_t preferredBandwidth = 0
    );

private:
    // =========================================================================
    // 内部ヘルパー
    // =========================================================================

    /// @brief 文字列を行に分割（\n, \r\n対応）
    static std::vector<std::string> SplitLines(const std::string& content);

    /// @brief 文字列を大文字に変換
    static std::string ToUpper(const std::string& str);

    /// @brief 文字列の前後の空白を除去
    static std::string Trim(const std::string& str);

    /// @brief タグの値を取得（例: "#EXT-X-VERSION:3" -> "3"）
    static std::string GetTagValue(const std::string& line, const std::string& tagName);

    /// @brief #EXT-X-KEYタグをパース
    static std::optional<HlsEncryptionKey> ParseKeyTag(
        const std::string& line,
        const std::string& baseUrl
    );

    /// @brief #EXT-X-BYTERANGEタグをパース
    static bool ParseByteRangeTag(
        const std::string& line,
        int64_t& outLength,
        int64_t& outOffset
    );

    /// @brief 16進数文字列（0x...）をバイト配列に変換
    static std::vector<uint8_t> ParseHexString(const std::string& hexStr);

    /// @brief URLのホスト部分を取得
    static std::string GetUrlOrigin(const std::string& url);

    /// @brief URLのスキーム部分を取得（例: "https:"）。スキーム相対URL（//host/path）の解決に使用
    static std::string GetUrlScheme(const std::string& url);
};

}  // namespace hls
}  // namespace ytdlpspout
