// =============================================================================
// ytdlpspout.h - 公開API（将来のDLL化用）
// =============================================================================
//
// 機能:
//   - 外部からリンク可能なC/C++ API
//   - Python/他言語からのFFI呼び出し対応
//   - シンプルな関数ベースのインターフェース
//
// 将来の拡張:
//   - DLLエクスポート (YTDLPSPOUT_API)
//   - C言語互換API
//   - Python ctypes / pybind11 バインディング
//
// =============================================================================

#pragma once

#include <stdint.h>

// =============================================================================
// DLLエクスポートマクロ
// =============================================================================

#ifdef YTDLPSPOUT_BUILD_DLL
    #ifdef _MSC_VER
        #define YTDLPSPOUT_API __declspec(dllexport)
    #else
        #define YTDLPSPOUT_API __attribute__((visibility("default")))
    #endif
#elif defined(YTDLPSPOUT_USE_DLL)
    #ifdef _MSC_VER
        #define YTDLPSPOUT_API __declspec(dllimport)
    #else
        #define YTDLPSPOUT_API
    #endif
#else
    #define YTDLPSPOUT_API
#endif

// =============================================================================
// C言語互換ラッパー
// =============================================================================

#ifdef __cplusplus
extern "C" {
#endif

// ハンドル型
typedef void* YtdlpSpoutHandle;

/// @brief プレイヤー設定構造体（C互換）
typedef struct YtdlpSpoutConfig {
    const char* inputFile;      ///< 入力ファイルパス
    const char* senderName;     ///< Spout Sender名
    int loop;                   ///< ループ再生 (0=false, 1=true)
    int useHardwareAccel;       ///< ハードウェアアクセラレーション
    int verbose;                ///< 詳細ログ
} YtdlpSpoutConfig;

// =============================================================================
// スライス読み込み設定
// =============================================================================

/// @brief スライス読み込み設定
typedef struct YtdlpSpoutSliceConfig {
    int enabled;                      ///< スライス読み込み有効（1=有効、0=無効）
    size_t chunkSize;                 ///< チャンクサイズ（バイト、デフォルト: 1MB）
    size_t maxCacheMemory;            ///< 最大キャッシュメモリ（バイト、デフォルト: 128MB）
    int maxConcurrentDownloads;       ///< 最大並列ダウンロード数（デフォルト: 4）
    int prefetchChunksAhead;          ///< 先読みチャンク数（デフォルト: 16）
    int criticalChunksAhead;          ///< 最優先チャンク数（デフォルト: 4）
    int enableContinuousDownload;     ///< 継続ダウンロード有効（1=有効、ファイル全体をバックグラウンドでダウンロード）
    const char* cachePath;            ///< ファイルキャッシュパス（NULL=メモリのみ）
} YtdlpSpoutSliceConfig;

/// @brief yt-dlp設定
typedef struct YtdlpSpoutYtDlpConfig {
    const char* path;                 ///< yt-dlpパス（NULL=自動検出）
    int preferredHeight;              ///< 希望解像度（デフォルト: 1080）
} YtdlpSpoutYtDlpConfig;

/// @brief HTTPヘッダー
typedef struct YtdlpSpoutHttpHeader {
    const char* key;                  ///< ヘッダーキー（例: "Cookie"）
    const char* value;                ///< ヘッダー値（例: "session_id=abc123"）
} YtdlpSpoutHttpHeader;

/// @brief 拡張設定
typedef struct YtdlpSpoutConfigEx {
    const char* source;               ///< ファイルパスまたはURL
    const char* senderName;           ///< Spout Sender名
    int outputWidth;                  ///< 出力幅（0=ソース解像度）
    int outputHeight;                 ///< 出力高さ（0=ソース解像度）
    int loop;                         ///< ループ再生（1=有効）
    int useHardwareAccel;             ///< ハードウェアアクセラレーション（1=有効）
    int verbose;                      ///< 詳細ログ（1=有効）
    YtdlpSpoutSliceConfig slice;      ///< スライス読み込み設定
    YtdlpSpoutYtDlpConfig ytdlp;      ///< yt-dlp設定
    const YtdlpSpoutHttpHeader* httpHeaders;  ///< HTTPヘッダー配列（NULL=なし）
    int httpHeadersCount;             ///< HTTPヘッダー数
    int isHlsHint;                    ///< HLS判定ヒント（-1=自動判定、0=非HLS、1=HLS。yt-dlp側の判定結果を伝搬する用途）
} YtdlpSpoutConfigEx;

/// @brief 再生状態
typedef enum YtdlpSpoutState {
    YTDLPSPOUT_STATE_STOPPED = 0,
    YTDLPSPOUT_STATE_PLAYING = 1,
    YTDLPSPOUT_STATE_PAUSED = 2,
    YTDLPSPOUT_STATE_ERROR = 3
} YtdlpSpoutState;

/// @brief 動画情報構造体
typedef struct YtdlpSpoutVideoInfo {
    int width;                  ///< 幅（ピクセル）
    int height;                 ///< 高さ（ピクセル）
    double fps;                 ///< フレームレート
    double duration;            ///< 再生時間（秒）
    int64_t totalFrames;        ///< 総フレーム数
} YtdlpSpoutVideoInfo;

/// @brief HLSキャッシュ統計
typedef struct YtdlpSpoutHlsCacheStats {
    int cachedSegments;         ///< キャッシュ済みセグメント数
    int totalSegments;          ///< 総セグメント数
    double downloadProgress;    ///< ダウンロード進捗 (0.0〜1.0)
    double bandwidth;           ///< 推定帯域幅 (bytes/sec)
    int isFullyCached;          ///< 完全キャッシュ済み (1=true, 0=false)
    int isHlsMode;              ///< HLSモードで再生中 (1=true, 0=false)
} YtdlpSpoutHlsCacheStats;

// =============================================================================
// API関数
// =============================================================================

/// @brief ライブラリバージョンを取得
/// @return バージョン文字列
YTDLPSPOUT_API const char* ytdlpspout_version(void);

/// @brief プレイヤーを作成
/// @return プレイヤーハンドル（失敗時はNULL）
YTDLPSPOUT_API YtdlpSpoutHandle ytdlpspout_create(void);

/// @brief プレイヤーを破棄
/// @param handle プレイヤーハンドル
YTDLPSPOUT_API void ytdlpspout_destroy(YtdlpSpoutHandle handle);

/// @brief 再生を開始
/// @param handle プレイヤーハンドル
/// @param config 設定
/// @return 成功時0、失敗時は非0
YTDLPSPOUT_API int ytdlpspout_start(YtdlpSpoutHandle handle, const YtdlpSpoutConfig* config);

/// @brief 再生を停止
/// @param handle プレイヤーハンドル
YTDLPSPOUT_API void ytdlpspout_stop(YtdlpSpoutHandle handle);

/// @brief 一時停止
/// @param handle プレイヤーハンドル
YTDLPSPOUT_API void ytdlpspout_pause(YtdlpSpoutHandle handle);

/// @brief 再生再開
/// @param handle プレイヤーハンドル
YTDLPSPOUT_API void ytdlpspout_resume(YtdlpSpoutHandle handle);

/// @brief シーク
/// @param handle プレイヤーハンドル
/// @param seconds 秒単位の位置
/// @return 成功時0、失敗時は非0
YTDLPSPOUT_API int ytdlpspout_seek(YtdlpSpoutHandle handle, double seconds);

/// @brief 1フレーム処理（非ブロッキング）
/// @param handle プレイヤーハンドル
/// @return 処理成功=1、終了/エラー=0
YTDLPSPOUT_API int ytdlpspout_process_frame(YtdlpSpoutHandle handle);

/// @brief 再生状態を取得
/// @param handle プレイヤーハンドル
/// @return YtdlpSpoutState
YTDLPSPOUT_API YtdlpSpoutState ytdlpspout_get_state(YtdlpSpoutHandle handle);

/// @brief 再生中かどうか
/// @param handle プレイヤーハンドル
/// @return 再生中なら1、それ以外は0
YTDLPSPOUT_API int ytdlpspout_is_playing(YtdlpSpoutHandle handle);

/// @brief 現在の再生時刻を取得
/// @param handle プレイヤーハンドル
/// @return 秒単位の再生時刻
YTDLPSPOUT_API double ytdlpspout_get_current_time(YtdlpSpoutHandle handle);

/// @brief 現在の再生位置を取得（get_current_timeのエイリアス）
/// @param handle プレイヤーハンドル
/// @return 秒単位の再生位置
YTDLPSPOUT_API double ytdlpspout_get_position(YtdlpSpoutHandle handle);

/// @brief 総再生時間を取得
/// @param handle プレイヤーハンドル
/// @return 秒単位の総再生時間
YTDLPSPOUT_API double ytdlpspout_get_duration(YtdlpSpoutHandle handle);

/// @brief 動画情報を取得
/// @param handle プレイヤーハンドル
/// @param info 情報を格納する構造体へのポインタ
/// @return 成功時0、失敗時は非0
YTDLPSPOUT_API int ytdlpspout_get_video_info(YtdlpSpoutHandle handle, YtdlpSpoutVideoInfo* info);

// =============================================================================
// ビート機能
// =============================================================================

/// @brief ビート単位でジャンプ
/// @param handle プレイヤーハンドル
/// @param beats ジャンプするビート数
/// @param forward 1=前進、0=後退
/// @return 成功時0、失敗時は非0
YTDLPSPOUT_API int ytdlpspout_jump_beats(YtdlpSpoutHandle handle, int beats, int forward);

/// @brief BPMを取得
/// @param handle プレイヤーハンドル
/// @return BPM値（ビートマップがない場合は0）
YTDLPSPOUT_API float ytdlpspout_get_bpm(YtdlpSpoutHandle handle);

/// @brief 必要なフレームバッファサイズを取得
/// @param handle プレイヤーハンドル
/// @return バッファサイズ（バイト）、エラー時は0
YTDLPSPOUT_API int ytdlpspout_get_frame_buffer_size(YtdlpSpoutHandle handle);

/// @brief 現在のフレームピクセルデータを取得（BGRA形式）
/// @param handle プレイヤーハンドル
/// @param buffer 出力バッファ（呼び出し側で確保）
/// @param bufferSize バッファサイズ
/// @param outWidth 出力: 幅
/// @param outHeight 出力: 高さ
/// @return 成功時0、失敗時は非0
YTDLPSPOUT_API int ytdlpspout_get_current_frame(
    YtdlpSpoutHandle handle,
    uint8_t* buffer,
    int bufferSize,
    int* outWidth,
    int* outHeight
);

/// @brief 最後のエラーメッセージを取得（スレッドローカル）
/// @return エラーメッセージ文字列
YTDLPSPOUT_API const char* ytdlpspout_get_last_error(void);

// =============================================================================
// コールバック型定義
// =============================================================================

// =============================================================================
// 拡張API関数
// =============================================================================

/// @brief デフォルト設定で初期化
/// @param config 設定構造体へのポインタ
YTDLPSPOUT_API void ytdlpspout_config_ex_init(YtdlpSpoutConfigEx* config);

/// @brief 拡張設定で再生開始
/// @param handle プレイヤーハンドル
/// @param config 拡張設定
/// @return 成功=0、失敗=-1
YTDLPSPOUT_API int ytdlpspout_start_ex(
    YtdlpSpoutHandle handle,
    const YtdlpSpoutConfigEx* config
);

/// @brief ダウンロード進捗を取得（0.0〜1.0）
/// @param handle プレイヤーハンドル
/// @return ダウンロード進捗
YTDLPSPOUT_API double ytdlpspout_get_download_progress(YtdlpSpoutHandle handle);

/// @brief 帯域幅を取得（bytes/sec）
/// @param handle プレイヤーハンドル
/// @return 推定帯域幅
YTDLPSPOUT_API double ytdlpspout_get_bandwidth(YtdlpSpoutHandle handle);

/// @brief 全チャンクがキャッシュ済みか確認
/// @param handle プレイヤーハンドル
/// @return 完全キャッシュ済み=1、それ以外=0
YTDLPSPOUT_API int ytdlpspout_is_fully_cached(YtdlpSpoutHandle handle);

/// @brief キャッシュ統計を取得
/// @param handle プレイヤーハンドル
/// @param cachedChunks キャッシュ済みチャンク数（出力）
/// @param totalChunks 総チャンク数（出力）
YTDLPSPOUT_API void ytdlpspout_get_cache_stats(
    YtdlpSpoutHandle handle,
    size_t* cachedChunks,
    size_t* totalChunks
);

/// @brief HLSキャッシュ統計を取得
/// @param handle インスタンスハンドル
/// @param stats 統計情報の出力先
/// @return 成功時0、失敗時-1
YTDLPSPOUT_API int ytdlpspout_get_hls_cache_stats(
    YtdlpSpoutHandle handle,
    YtdlpSpoutHlsCacheStats* stats
);

// =============================================================================
// コールバック型定義
// =============================================================================

/// @brief 進捗コールバック型
typedef void (*YtdlpSpoutProgressCallback)(double currentTime, double duration, void* userData);

/// @brief エラーコールバック型
typedef void (*YtdlpSpoutErrorCallback)(const char* message, void* userData);

/// @brief 完了コールバック型
typedef void (*YtdlpSpoutCompletionCallback)(void* userData);

/// @brief 進捗コールバックを設定
/// @param handle プレイヤーハンドル
/// @param callback コールバック関数
/// @param userData ユーザーデータ（コールバック時に渡される）
YTDLPSPOUT_API void ytdlpspout_set_progress_callback(
    YtdlpSpoutHandle handle,
    YtdlpSpoutProgressCallback callback,
    void* userData
);

/// @brief エラーコールバックを設定
/// @param handle プレイヤーハンドル
/// @param callback コールバック関数
/// @param userData ユーザーデータ
YTDLPSPOUT_API void ytdlpspout_set_error_callback(
    YtdlpSpoutHandle handle,
    YtdlpSpoutErrorCallback callback,
    void* userData
);

/// @brief 完了コールバックを設定
/// @param handle プレイヤーハンドル
/// @param callback コールバック関数
/// @param userData ユーザーデータ
YTDLPSPOUT_API void ytdlpspout_set_completion_callback(
    YtdlpSpoutHandle handle,
    YtdlpSpoutCompletionCallback callback,
    void* userData
);

#ifdef __cplusplus
} // extern "C"
#endif

// =============================================================================
// C++ ラッパー（C++から使用する場合）
// =============================================================================

#ifdef __cplusplus
namespace ytdlpspout {
namespace api {

/// @brief C++ ラッパークラス
/// @details C API をラップしてRAIIパターンで使用可能にする
class Player {
public:
    Player() : m_handle(ytdlpspout_create()) {}
    ~Player() { if (m_handle) ytdlpspout_destroy(m_handle); }

    // コピー禁止、ムーブ許可
    Player(const Player&) = delete;
    Player& operator=(const Player&) = delete;
    Player(Player&& other) noexcept : m_handle(other.m_handle) { other.m_handle = nullptr; }
    Player& operator=(Player&& other) noexcept {
        if (this != &other) {
            if (m_handle) ytdlpspout_destroy(m_handle);
            m_handle = other.m_handle;
            other.m_handle = nullptr;
        }
        return *this;
    }

    bool IsValid() const { return m_handle != nullptr; }
    YtdlpSpoutHandle Handle() const { return m_handle; }

    bool Start(const YtdlpSpoutConfig& config) {
        return ytdlpspout_start(m_handle, &config) == 0;
    }

    void Stop() { ytdlpspout_stop(m_handle); }
    void Pause() { ytdlpspout_pause(m_handle); }
    void Resume() { ytdlpspout_resume(m_handle); }
    
    bool Seek(double seconds) {
        return ytdlpspout_seek(m_handle, seconds) == 0;
    }

    bool ProcessFrame() {
        return ytdlpspout_process_frame(m_handle) != 0;
    }

    YtdlpSpoutState GetState() const {
        return ytdlpspout_get_state(m_handle);
    }

    double GetCurrentTime() const {
        return ytdlpspout_get_current_time(m_handle);
    }

private:
    YtdlpSpoutHandle m_handle;
};

} // namespace api
} // namespace ytdlpspout
#endif
