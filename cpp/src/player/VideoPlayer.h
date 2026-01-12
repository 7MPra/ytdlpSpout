// =============================================================================
// VideoPlayer.h - 動画再生制御
// =============================================================================
//
// 機能:
//   - VideoDecoder + D3D11Context + SpoutSender の統合
//   - 再生ループ制御
//   - シーク・一時停止・ループ再生
//   - フレームタイミング制御
//   - ビートマップ連携・ビートジャンプ
//
// =============================================================================

#pragma once

#include <string>
#include <memory>
#include <functional>
#include <atomic>

namespace ytdlpspout {

// 前方宣言
class BeatMap;
struct BeatInfo;
class BeatJumpController;

/// @brief プレイヤー設定
struct PlayerConfig {
    // === 入力ソース ===
    std::string source;                     // ファイルパス or URL（推奨）
    std::string filePath;                   // 入力ファイルパス（後方互換性のため維持、sourceが優先）
    
    // === 出力設定 ===
    std::string senderName = "ytdlpSpout";  // Spout Sender名
    int outputWidth = 0;                    // 出力幅（0=ソース解像度）
    int outputHeight = 0;                   // 出力高さ（0=ソース解像度）
    
    // === 再生設定 ===
    bool loop = false;                      // ループ再生
    bool useHardwareAccel = true;           // ハードウェアアクセラレーション使用
    bool verbose = false;                   // 詳細ログ
    
    // === スライス読み込み設定 ===
    struct SliceConfig {
        bool enabled = true;                   // スライス読み込み有効
        size_t chunkSize = 2 * 1024 * 1024;    // チャンクサイズ（2MB）
        size_t maxCacheMemory = 256 * 1024 * 1024;  // 最大キャッシュメモリ（256MB）
        int maxConcurrentDownloads = 6;        // 最大並列ダウンロード数
        int prefetchChunksAhead = 24;          // 先読みチャンク数
        int criticalChunksAhead = 6;           // 最優先チャンク数（即座にダウンロード）
        bool enableContinuousDownload = true;  // ファイル全体を継続ダウンロードするか
        std::string cachePath;                 // キャッシュ保存先（空=メモリのみ）
    } slice;
    
    // === yt-dlp設定 ===
    struct YtDlpConfig {
        std::string path;                   // yt-dlpパス（空=自動検出）
        int preferredHeight = 1080;         // 希望解像度
    } ytdlp;
    
    /// @brief 入力ソースを取得（source優先、なければfilePath）
    const std::string& GetSource() const {
        return source.empty() ? filePath : source;
    }
};

/// @brief 再生状態
enum class PlayerState {
    Stopped,    // 停止
    Playing,    // 再生中
    Paused,     // 一時停止
    Error       // エラー
};

/// @brief 動画プレイヤークラス
/// @details デコード・変換・送信を統合した再生制御
class VideoPlayer {
public:
    VideoPlayer();
    ~VideoPlayer();

    // コピー禁止
    VideoPlayer(const VideoPlayer&) = delete;
    VideoPlayer& operator=(const VideoPlayer&) = delete;

    // =========================================================================
    // 再生制御
    // =========================================================================

    /// @brief 再生を開始
    /// @param config プレイヤー設定
    /// @return 成功した場合true
    bool Start(const PlayerConfig& config);

    /// @brief 再生を停止
    void Stop();

    /// @brief 停止リクエストを設定（外部から停止を要求）
    void RequestStop();

    /// @brief 一時停止
    void Pause();

    /// @brief 再生再開
    void Resume();

    /// @brief 一時停止状態を切り替え
    void TogglePause();

    // =========================================================================
    // シーク
    // =========================================================================

    /// @brief 指定時刻にシーク
    /// @param seconds 秒単位の位置
    /// @return 成功した場合true
    bool Seek(double seconds);

    /// @brief 相対シーク
    /// @param deltaSeconds 現在位置からの相対秒数
    /// @return 成功した場合true
    bool SeekRelative(double deltaSeconds);

    // =========================================================================
    // 再生ループ
    // =========================================================================

    /// @brief 再生ループを実行（ブロッキング）
    /// @return 正常終了=0、エラー時は非0
    int RunLoop();

    /// @brief 1フレーム処理（非ブロッキング）
    /// @return 処理成功=true、終了/エラー=false
    bool ProcessFrame();

    // =========================================================================
    // 状態取得
    // =========================================================================

    /// @brief 再生状態を取得
    /// @return PlayerState
    PlayerState GetState() const;

    /// @brief 再生中かどうか
    /// @return 再生中の場合true
    bool IsPlaying() const;

    /// @brief 一時停止中かどうか
    /// @return 一時停止中の場合true
    bool IsPaused() const;

    /// @brief 現在の再生時刻を取得
    /// @return 秒単位の再生時刻
    double GetPlaybackTime() const;

    /// @brief 総再生時間を取得
    /// @return 秒単位の総再生時間
    double GetDuration() const;

    /// @brief 現在のフレーム番号を取得
    /// @return フレーム番号
    int64_t GetCurrentFrame() const;

    /// @brief 総フレーム数を取得
    /// @return 総フレーム数
    int64_t GetTotalFrames() const;

    /// @brief 動画幅を取得
    /// @return 幅（ピクセル）
    int GetWidth() const;

    /// @brief 動画高さを取得
    /// @return 高さ（ピクセル）
    int GetHeight() const;

    /// @brief FPSを取得
    /// @return フレームレート
    double GetFPS() const;

    /// @brief 送信したフレーム数を取得
    /// @return フレーム数
    uint64_t GetSentFrameCount() const;

    // =========================================================================
    // スライス読み込み状態取得
    // =========================================================================

    /// @brief ダウンロード進捗を取得（0.0〜1.0）
    /// @return ダウンロード進捗（スライス読み込み無効時は1.0）
    double GetDownloadProgress() const;

    /// @brief 全チャンクがキャッシュ済みか
    /// @return キャッシュ済みの場合true（スライス読み込み無効時はtrue）
    bool IsFullyCached() const;

    // =========================================================================
    // フレームデータ取得（GUI連携用）
    // =========================================================================

    /// @brief 必要なフレームバッファサイズを取得
    /// @return バッファサイズ（バイト）、動画未読込時は0
    int GetFrameBufferSize() const;

    /// @brief 現在のフレームデータを取得（BGRA形式）
    /// @param buffer 出力バッファ（呼び出し側で確保）
    /// @param bufferSize バッファサイズ
    /// @param outWidth 出力: 幅
    /// @param outHeight 出力: 高さ
    /// @return 成功時0、失敗時は非0
    int GetCurrentFrameData(uint8_t* buffer, int bufferSize, int* outWidth, int* outHeight);

    // =========================================================================
    // コールバック
    // =========================================================================

    /// @brief 進捗コールバック型
    using ProgressCallback = std::function<void(double currentTime, double duration)>;

    /// @brief 進捗コールバックを設定
    void SetProgressCallback(ProgressCallback callback);

    /// @brief エラーコールバック型
    using ErrorCallback = std::function<void(const std::string& message)>;

    /// @brief エラーコールバックを設定
    void SetErrorCallback(ErrorCallback callback);

    /// @brief 終了コールバック型
    using CompletionCallback = std::function<void()>;

    /// @brief 終了コールバックを設定
    void SetCompletionCallback(CompletionCallback callback);

    // =========================================================================
    // ビートマップ連携
    // =========================================================================

    /// @brief ビートマップを設定
    /// @param beatMap ビートマップへの共有ポインタ
    void SetBeatMap(std::shared_ptr<BeatMap> beatMap);

    /// @brief ビートマップを取得
    /// @return ビートマップへの共有ポインタ（未設定時nullptr）
    std::shared_ptr<BeatMap> GetBeatMap() const;

    /// @brief ビート単位でジャンプ
    /// @param beats ジャンプするビート数
    /// @param forward true=前進、false=後退
    /// @return 成功した場合true
    bool JumpBeats(int beats, bool forward = true);

    /// @brief 最寄りのビートにジャンプ
    /// @return 成功した場合true
    bool JumpToNearestBeat();

    /// @brief ビートイベントコールバック型
    using BeatCallback = std::function<void(const BeatInfo&)>;

    /// @brief ビートコールバックを設定
    /// @param callback ビート通過時に呼ばれるコールバック
    void SetBeatCallback(BeatCallback callback);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace ytdlpspout
