// =============================================================================
// HlsCustomAVIOContext.h - HLS用カスタムAVIOContext
// =============================================================================
//
// 機能:
//   - キャッシュされたHLSセグメントを連結してFFmpegに提供
//   - バイトオフセットベースのシーク
//   - 時間ベースのシーク
//   - 仮想ファイルサイズの推定と更新
//
// 使用例:
//   HlsSegmentCache cache;
//   cache.Initialize(playlist);
//   
//   HlsCustomAVIOContext avioContext;
//   avioContext.Initialize(&cache, playlist);
//   avioContext.SetReadTimeout(5000);
//   
//   AVIOContext* avio = avioContext.GetAVIOContext();
//   // FFmpegのAVFormatContextに渡す
//   formatCtx->pb = avio;
//
// =============================================================================

#pragma once

#include "hls/M3U8Parser.h"

#include <memory>
#include <memory>
#include <cstdint>
#include <functional>

// 前方宣言
struct AVIOContext;

namespace ytdlpspout {
namespace hls {

// 前方宣言
class HlsSegmentCache;

/// @brief HLS用カスタムAVIOContext
/// 
/// HLSセグメントを連結した仮想ファイルとしてFFmpegに提供する。
/// セグメントキャッシュから読み取り、バイトオフセットと時間ベースのシークをサポート。
class HlsCustomAVIOContext {
public:
    // =========================================================================
    // コンストラクタ / デストラクタ
    // =========================================================================
    
    /// @brief コンストラクタ
    HlsCustomAVIOContext();
    
    /// @brief デストラクタ
    ~HlsCustomAVIOContext();
    
    // コピー禁止
    HlsCustomAVIOContext(const HlsCustomAVIOContext&) = delete;
    HlsCustomAVIOContext& operator=(const HlsCustomAVIOContext&) = delete;
    
    // =========================================================================
    // 初期化 / 終了
    // =========================================================================
    
    /// @brief 初期化
    /// @param cache セグメントキャッシュ（外部所有、ライフタイムはこのオブジェクトより長く保つこと）
    /// @param playlist プレイリスト情報
    /// @return 成功時true
    bool Initialize(HlsSegmentCache* cache, const M3U8Playlist& playlist);
    
    /// @brief クローズ（リソース解放）
    void Close();
    
    // =========================================================================
    // FFmpeg統合
    // =========================================================================
    
    /// @brief FFmpeg AVIOContextを取得
    /// @return AVIOContext（未初期化の場合nullptr）
    AVIOContext* GetAVIOContext() const;
    
    // =========================================================================
    // サイズ / 位置
    // =========================================================================
    
    /// @brief 仮想ファイル総サイズ（セグメントサイズの合計または推定値）
    /// @return 総バイト数
    int64_t GetSize() const;
    
    /// @brief 現在の読み取り位置（バイト）
    /// @return 現在のバイトオフセット
    int64_t GetPosition() const;
    
    /// @brief 現在のセグメントインデックス
    /// @return セグメントインデックス（0始まり）
    int64_t GetCurrentSegmentIndex() const;
    
    // =========================================================================
    // シーク
    // =========================================================================
    
    /// @brief バイトオフセットでシーク
    /// @param offset オフセット値
    /// @param whence SEEK_SET, SEEK_CUR, SEEK_END, またはAVSEEK_SIZE
    /// @return シーク後のオフセット（失敗時-1）
    int64_t Seek(int64_t offset, int whence);
    
    /// @brief 時間ベースシーク（秒）
    /// @param seconds 再生位置（秒）
    /// @return シーク後のバイトオフセット
    int64_t SeekToTime(double seconds);
    
    // =========================================================================
    // 設定
    // =========================================================================
    
    /// @brief 読み取り待機タイムアウト（ミリ秒）
    /// @param timeoutMs タイムアウト時間（0=即時、-1=無制限）
    void SetReadTimeout(int timeoutMs);
    
    /// @brief シークコールバックを設定
    /// @param callback シーク時に呼び出されるコールバック (引数: 新しいセグメントインデックス)
    void SetOnSeekCallback(std::function<void(int64_t)> callback);

    /// @brief セグメント再ダウンロード要求コールバックを設定
    ///
    /// 恒久的にダウンロード失敗したとマークされたセグメントを読み取ろうとした際、
    /// 一度だけ自動復旧を試みるために呼び出される（引数: 再ダウンロードすべきセグメントインデックス）。
    /// コールバック側は失敗マークの解除（HlsSegmentCache::ClearSegmentFailed）と
    /// 再ダウンロード要求（ChunkDownloader::RequestSegment等）を行うことを想定している。
    /// @param callback 再ダウンロード要求コールバック
    void SetOnSegmentRetryCallback(std::function<void(int64_t)> callback);

private:
    // =========================================================================
    // FFmpegコールバック
    // =========================================================================
    
    /// @brief read_packetコールバック
    /// @param opaque HlsCustomAVIOContextへのポインタ
    /// @param buf 読み取りバッファ
    /// @param bufSize バッファサイズ
    /// @return 読み取りバイト数（EOF時AVERROR_EOF、エラー時負の値）
    static int ReadPacket(void* opaque, uint8_t* buf, int bufSize);
    
    /// @brief seekコールバック
    /// @param opaque HlsCustomAVIOContextへのポインタ
    /// @param offset オフセット値
    /// @param whence シークモード
    /// @return 新しいオフセット（失敗時-1）
    static int64_t SeekCallback(void* opaque, int64_t offset, int whence);
    
    // =========================================================================
    // 内部実装
    // =========================================================================
    
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

}  // namespace hls
}  // namespace ytdlpspout
