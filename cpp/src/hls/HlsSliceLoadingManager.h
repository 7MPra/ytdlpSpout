// =============================================================================
// HlsSliceLoadingManager.h - HLSスライス読み込み統合マネージャー
// =============================================================================
//
// 機能:
//   - HLSストリームの統合管理
//   - m3u8プレイリストの自動解析
//   - セグメントのプリフェッチ最適化
//   - AES-128復号のサポート
//   - FFmpeg AVIOContext統合
//
// 使用例:
//   HlsSliceLoadingManager manager;
//   
//   HlsSliceConfig config;
//   config.maxCacheMemory = 256 * 1024 * 1024;
//   config.prefetchSegmentsAhead = 5;
//   
//   if (manager.Open("https://example.com/playlist.m3u8", config)) {
//       AVIOContext* avio = manager.GetAVIOContext();
//       // FFmpegのAVFormatContextに渡す
//       formatCtx->pb = avio;
//       
//       // 再生位置を更新（プリフェッチ最適化）
//       manager.UpdatePlaybackPosition(currentSeconds);
//   }
//
// =============================================================================

#pragma once

#include <string>
#include <memory>
#include <map>
#include <cstdint>

// 前方宣言
struct AVIOContext;

namespace ytdlpspout {
namespace hls {

// =============================================================================
// 設定構造体
// =============================================================================

/// @brief HLSスライス読み込み設定
struct HlsSliceConfig {
    /// @brief キャッシュの最大メモリサイズ（バイト）
    size_t maxCacheMemory = 256 * 1024 * 1024;  // 256MB
    
    /// @brief 最大同時ダウンロード数
    int maxConcurrentDownloads = 4;
    
    /// @brief 先読みセグメント数
    int prefetchSegmentsAhead = 5;
    
    /// @brief 読み取りタイムアウト（ミリ秒）
    int readTimeoutMs = 30000;
    
    /// @brief HTTPヘッダー（Cookie、User-Agent等）
    std::map<std::string, std::string> httpHeaders;
};

// =============================================================================
// マネージャークラス
// =============================================================================

/// @brief HLSスライス読み込み統合マネージャー
///
/// HLSストリームのすべてのコンポーネント（M3U8Parser, HlsSegmentCache,
/// HlsCustomAVIOContext, ChunkDownloader）を統合管理する。
/// プリフェッチ最適化とシーク時の優先度再計算を自動で行う。
class HlsSliceLoadingManager {
public:
    // =========================================================================
    // コンストラクタ / デストラクタ
    // =========================================================================
    
    /// @brief コンストラクタ
    HlsSliceLoadingManager();
    
    /// @brief デストラクタ
    ~HlsSliceLoadingManager();
    
    // コピー禁止
    HlsSliceLoadingManager(const HlsSliceLoadingManager&) = delete;
    HlsSliceLoadingManager& operator=(const HlsSliceLoadingManager&) = delete;
    
    // =========================================================================
    // 初期化 / 終了
    // =========================================================================
    
    /// @brief HLS URLで開く
    /// @param hlsUrl m3u8のURL
    /// @param config 設定
    /// @return 成功時true
    bool Open(const std::string& hlsUrl, const HlsSliceConfig& config);
    
    /// @brief クローズ
    void Close();
    
    /// @brief 開いているか
    /// @return 開いている場合true
    bool IsOpen() const;
    
    // =========================================================================
    // FFmpeg統合
    // =========================================================================
    
    /// @brief FFmpeg AVIOContextを取得
    /// @return AVIOContext（未初期化時nullptr）
    AVIOContext* GetAVIOContext() const;
    
    // =========================================================================
    // URL判定（静的メソッド）
    // =========================================================================
    
    /// @brief HLS URLか判定
    /// @param url URL文字列
    /// @return HLS URL（.m3u8, .m3u）ならtrue
    static bool IsHlsUrl(const std::string& url);
    
    // =========================================================================
    // 再生情報
    // =========================================================================
    
    /// @brief 総時間（秒）
    /// @return プレイリストの総再生時間
    double GetDuration() const;
    
    // =========================================================================
    // 統計情報
    // =========================================================================
    
    /// @brief ダウンロード進捗（0.0〜1.0）
    /// @return ダウンロード済みセグメントの割合
    double GetDownloadProgress() const;
    
    /// @brief 推定帯域幅（bytes/sec）
    /// @return 推定帯域幅
    double GetBandwidth() const;
    
    /// @brief 完全キャッシュ済みか
    /// @return すべてのセグメントがキャッシュ済みならtrue
    bool IsFullyCached() const;
    
    /// @brief キャッシュ済みセグメント数
    /// @return キャッシュ済みのセグメント数
    size_t GetCachedSegmentCount() const;
    
    /// @brief 総セグメント数
    /// @return プレイリスト内のセグメント総数
    size_t GetTotalSegmentCount() const;
    
    // =========================================================================
    // 再生位置連携
    // =========================================================================
    
    /// @brief 再生位置更新（プリフェッチ用）
    /// @param seconds 現在の再生位置（秒）
    /// 
    /// 現在位置から prefetchSegmentsAhead 個先までのセグメントを
    /// 優先的にダウンロードキューに追加する。
    void UpdatePlaybackPosition(double seconds);
    
    /// @brief シーク通知（優先度再計算）
    /// @param seconds シーク先の位置（秒）
    /// 
    /// 既存のダウンロードキューをクリアし、シーク先から
    /// プリフェッチを開始する。
    void NotifySeek(double seconds);
    
private:
    // =========================================================================
    // 内部実装
    // =========================================================================
    
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

}  // namespace hls
}  // namespace ytdlpspout
