// =============================================================================
// c_api.cpp - C言語API実装
// =============================================================================
//
// 機能:
//   - VideoPlayerをC言語APIでラップ
//   - スレッドセーフなエラー処理
//   - Python ctypes / 他言語FFI 互換
//
// =============================================================================

#include "ytdlpspout/ytdlpspout.h"
#include "player/VideoPlayer.h"
#include "audio/BeatMap.h"
#include "ytdlp/YtDlpResolver.h"
#include "utils/Logger.h"

#include <cstring>
#include <memory>
#include <string>
#include <mutex>

// バージョン文字列
static const char* YTDLPSPOUT_VERSION_STRING = "0.1.0";

// =============================================================================
// スレッドローカルエラー管理
// =============================================================================

namespace {

// スレッドローカルなエラーメッセージストレージ
thread_local std::string g_lastError;

void SetLastError(const std::string& error) {
    g_lastError = error;
}

// プレイヤーハンドル内部構造
struct PlayerContext {
    std::unique_ptr<ytdlpspout::VideoPlayer> player;
    std::shared_ptr<ytdlpspout::BeatMap> beatMap;
    
    // コールバック用ユーザーデータ
    void* progressUserData = nullptr;
    void* errorUserData = nullptr;
    void* completionUserData = nullptr;
    
    // コールバック関数
    YtdlpSpoutProgressCallback progressCallback = nullptr;
    YtdlpSpoutErrorCallback errorCallback = nullptr;
    YtdlpSpoutCompletionCallback completionCallback = nullptr;
    
    PlayerContext() : player(std::make_unique<ytdlpspout::VideoPlayer>()) {}
};

// ハンドルからコンテキストを取得
PlayerContext* GetContext(YtdlpSpoutHandle handle) {
    return static_cast<PlayerContext*>(handle);
}

// nullチェック付きプレイヤー取得
ytdlpspout::VideoPlayer* GetPlayer(YtdlpSpoutHandle handle) {
    if (!handle) {
        SetLastError("Invalid handle: nullptr");
        return nullptr;
    }
    auto* ctx = GetContext(handle);
    if (!ctx || !ctx->player) {
        SetLastError("Invalid handle: no player");
        return nullptr;
    }
    return ctx->player.get();
}

} // anonymous namespace

// =============================================================================
// API実装
// =============================================================================

extern "C" {

const char* ytdlpspout_version(void) {
    return YTDLPSPOUT_VERSION_STRING;
}

YtdlpSpoutHandle ytdlpspout_create(void) {
    try {
        auto* ctx = new PlayerContext();
        ctx->player = std::make_unique<ytdlpspout::VideoPlayer>();
        SetLastError("");  // Clear error
        return static_cast<YtdlpSpoutHandle>(ctx);
    } catch (const std::exception& e) {
        SetLastError(std::string("Failed to create player: ") + e.what());
        return nullptr;
    }
}

void ytdlpspout_destroy(YtdlpSpoutHandle handle) {
    if (!handle) {
        return;
    }
    
    try {
        auto* ctx = GetContext(handle);
        if (ctx && ctx->player) {
            ctx->player->Stop();
        }
        delete ctx;
    } catch (...) {
        // Ignore exceptions during destruction
    }
}

int ytdlpspout_start(YtdlpSpoutHandle handle, const YtdlpSpoutConfig* config) {
    if (!handle) {
        SetLastError("Invalid handle: nullptr");
        return -1;
    }
    if (!config) {
        SetLastError("Invalid config: nullptr");
        return -2;
    }
    if (!config->inputFile) {
        SetLastError("Invalid config: inputFile is null");
        return -3;
    }
    
    auto* ctx = GetContext(handle);
    if (!ctx || !ctx->player) {
        SetLastError("Invalid handle: no player");
        return -4;
    }
    
    try {
        std::string inputPath = config->inputFile;
        
        // yt-dlp対応URLの場合はストリームURLを解決
        auto sourceType = ytdlpspout::ytdlp::YtDlpResolver::GetSourceType(inputPath);
        if (sourceType == ytdlpspout::ytdlp::SourceType::YtDlpUrl) {
            LOG_INFO("Resolving yt-dlp URL: {}", inputPath);
            ytdlpspout::ytdlp::YtDlpResolver resolver;
            auto streamUrl = resolver.GetStreamUrl(inputPath, 1080);  // デフォルト1080p
            if (!streamUrl) {
                SetLastError("Failed to resolve yt-dlp URL");
                return -7;
            }
            inputPath = *streamUrl;
            LOG_INFO("Resolved to: {}", inputPath);
        }
        
        ytdlpspout::PlayerConfig playerConfig;
        playerConfig.filePath = inputPath;
        playerConfig.senderName = config->senderName ? config->senderName : "ytdlpSpout";
        playerConfig.loop = config->loop != 0;
        playerConfig.useHardwareAccel = config->useHardwareAccel != 0;
        
        // コールバックを設定
        if (ctx->progressCallback) {
            ctx->player->SetProgressCallback(
                [ctx](double current, double duration) {
                    if (ctx->progressCallback) {
                        ctx->progressCallback(current, duration, ctx->progressUserData);
                    }
                }
            );
        }
        
        if (ctx->errorCallback) {
            ctx->player->SetErrorCallback(
                [ctx](const std::string& message) {
                    if (ctx->errorCallback) {
                        ctx->errorCallback(message.c_str(), ctx->errorUserData);
                    }
                }
            );
        }
        
        if (ctx->completionCallback) {
            ctx->player->SetCompletionCallback(
                [ctx]() {
                    if (ctx->completionCallback) {
                        ctx->completionCallback(ctx->completionUserData);
                    }
                }
            );
        }
        
        if (!ctx->player->Start(playerConfig)) {
            SetLastError("Failed to start playback");
            return -5;
        }
        
        SetLastError("");
        return 0;
    } catch (const std::exception& e) {
        SetLastError(std::string("Exception during start: ") + e.what());
        return -6;
    }
}

void ytdlpspout_stop(YtdlpSpoutHandle handle) {
    auto* player = GetPlayer(handle);
    if (player) {
        player->Stop();
    }
}

void ytdlpspout_pause(YtdlpSpoutHandle handle) {
    auto* player = GetPlayer(handle);
    if (player) {
        player->Pause();
    }
}

void ytdlpspout_resume(YtdlpSpoutHandle handle) {
    auto* player = GetPlayer(handle);
    if (player) {
        player->Resume();
    }
}

int ytdlpspout_seek(YtdlpSpoutHandle handle, double seconds) {
    auto* player = GetPlayer(handle);
    if (!player) {
        return -1;
    }
    
    if (!player->Seek(seconds)) {
        SetLastError("Seek failed");
        return -2;
    }
    
    return 0;
}

int ytdlpspout_process_frame(YtdlpSpoutHandle handle) {
    auto* player = GetPlayer(handle);
    if (!player) {
        return 0;
    }
    
    return player->ProcessFrame() ? 1 : 0;
}

YtdlpSpoutState ytdlpspout_get_state(YtdlpSpoutHandle handle) {
    auto* player = GetPlayer(handle);
    if (!player) {
        return YTDLPSPOUT_STATE_ERROR;
    }
    
    switch (player->GetState()) {
        case ytdlpspout::PlayerState::Stopped:
            return YTDLPSPOUT_STATE_STOPPED;
        case ytdlpspout::PlayerState::Playing:
            return YTDLPSPOUT_STATE_PLAYING;
        case ytdlpspout::PlayerState::Paused:
            return YTDLPSPOUT_STATE_PAUSED;
        case ytdlpspout::PlayerState::Error:
        default:
            return YTDLPSPOUT_STATE_ERROR;
    }
}

int ytdlpspout_is_playing(YtdlpSpoutHandle handle) {
    auto* player = GetPlayer(handle);
    if (!player) {
        return 0;
    }
    return player->IsPlaying() ? 1 : 0;
}

double ytdlpspout_get_position(YtdlpSpoutHandle handle) {
    auto* player = GetPlayer(handle);
    if (!player) {
        return 0.0;
    }
    return player->GetPlaybackTime();
}

double ytdlpspout_get_current_time(YtdlpSpoutHandle handle) {
    return ytdlpspout_get_position(handle);
}

double ytdlpspout_get_duration(YtdlpSpoutHandle handle) {
    auto* player = GetPlayer(handle);
    if (!player) {
        return 0.0;
    }
    return player->GetDuration();
}

int ytdlpspout_get_video_info(YtdlpSpoutHandle handle, YtdlpSpoutVideoInfo* info) {
    if (!info) {
        SetLastError("Invalid info: nullptr");
        return -1;
    }
    
    auto* player = GetPlayer(handle);
    if (!player) {
        return -2;
    }
    
    info->width = player->GetWidth();
    info->height = player->GetHeight();
    info->fps = player->GetFPS();
    info->duration = player->GetDuration();
    info->totalFrames = player->GetTotalFrames();
    
    return 0;
}

int ytdlpspout_jump_beats(YtdlpSpoutHandle handle, int beats, int forward) {
    auto* player = GetPlayer(handle);
    if (!player) {
        return -1;
    }
    
    if (!player->GetBeatMap()) {
        SetLastError("No beatmap loaded");
        return -2;
    }
    
    if (!player->JumpBeats(beats, forward != 0)) {
        SetLastError("Jump beats failed");
        return -3;
    }
    
    return 0;
}

float ytdlpspout_get_bpm(YtdlpSpoutHandle handle) {
    auto* player = GetPlayer(handle);
    if (!player) {
        return 0.0f;
    }
    
    auto beatMap = player->GetBeatMap();
    if (!beatMap) {
        return 0.0f;
    }
    
    return beatMap->bpm;
}

int ytdlpspout_get_frame_buffer_size(YtdlpSpoutHandle handle) {
    auto* player = GetPlayer(handle);
    if (!player) {
        return 0;
    }
    
    return player->GetFrameBufferSize();
}

int ytdlpspout_get_current_frame(
    YtdlpSpoutHandle handle,
    uint8_t* buffer,
    int bufferSize,
    int* outWidth,
    int* outHeight)
{
    if (!handle) {
        SetLastError("Invalid handle: nullptr");
        return -1;
    }
    if (!buffer) {
        SetLastError("Invalid buffer: nullptr");
        return -2;
    }
    
    auto* player = GetPlayer(handle);
    if (!player) {
        return -3;
    }
    
    int result = player->GetCurrentFrameData(buffer, bufferSize, outWidth, outHeight);
    if (result != 0) {
        switch (result) {
            case -1:
                SetLastError("Invalid buffer parameters");
                break;
            case -2:
                SetLastError("No valid frame available");
                break;
            case -3:
                SetLastError("Buffer too small");
                break;
            default:
                SetLastError("Unknown error getting frame data");
                break;
        }
    }
    
    return result;
}

const char* ytdlpspout_get_last_error(void) {
    return g_lastError.c_str();
}

void ytdlpspout_set_progress_callback(
    YtdlpSpoutHandle handle,
    YtdlpSpoutProgressCallback callback,
    void* userData)
{
    if (!handle) return;
    
    auto* ctx = GetContext(handle);
    if (ctx) {
        ctx->progressCallback = callback;
        ctx->progressUserData = userData;
    }
}

void ytdlpspout_set_error_callback(
    YtdlpSpoutHandle handle,
    YtdlpSpoutErrorCallback callback,
    void* userData)
{
    if (!handle) return;
    
    auto* ctx = GetContext(handle);
    if (ctx) {
        ctx->errorCallback = callback;
        ctx->errorUserData = userData;
    }
}

void ytdlpspout_set_completion_callback(
    YtdlpSpoutHandle handle,
    YtdlpSpoutCompletionCallback callback,
    void* userData)
{
    if (!handle) return;
    
    auto* ctx = GetContext(handle);
    if (ctx) {
        ctx->completionCallback = callback;
        ctx->completionUserData = userData;
    }
}

// =============================================================================
// 拡張API実装
// =============================================================================

void ytdlpspout_config_ex_init(YtdlpSpoutConfigEx* config) {
    if (!config) return;
    memset(config, 0, sizeof(*config));
    config->senderName = "ytdlpSpout";
    config->loop = 0;
    config->useHardwareAccel = 1;
    config->verbose = 0;
    config->slice.enabled = 1;
    config->slice.chunkSize = 2 * 1024 * 1024;          // 2MB - 高解像度向け
    config->slice.maxCacheMemory = 256 * 1024 * 1024;   // 256MB
    config->slice.maxConcurrentDownloads = 6;            // 6ワーカー
    config->slice.prefetchChunksAhead = 24;              // 48MB先読み
    config->slice.criticalChunksAhead = 6;
    config->slice.enableContinuousDownload = 1;
    config->slice.cachePath = nullptr;
    config->ytdlp.path = nullptr;
    config->ytdlp.preferredHeight = 1080;
}

int ytdlpspout_start_ex(YtdlpSpoutHandle handle, const YtdlpSpoutConfigEx* config) {
    if (!handle) {
        SetLastError("Invalid handle: nullptr");
        return -1;
    }
    if (!config) {
        SetLastError("Invalid config: nullptr");
        return -1;
    }
    if (!config->source) {
        SetLastError("Invalid config: source is null");
        return -1;
    }
    
    auto* ctx = GetContext(handle);
    if (!ctx || !ctx->player) {
        SetLastError("Invalid handle: no player");
        return -1;
    }
    
    try {
        std::string inputPath = config->source;
        
        // yt-dlp対応URLの場合はストリームURLを解決
        auto sourceType = ytdlpspout::ytdlp::YtDlpResolver::GetSourceType(inputPath);
        if (sourceType == ytdlpspout::ytdlp::SourceType::YtDlpUrl) {
            LOG_INFO("Resolving yt-dlp URL: {}", inputPath);
            ytdlpspout::ytdlp::YtDlpResolver resolver;
            int preferredHeight = config->ytdlp.preferredHeight > 0 ? config->ytdlp.preferredHeight : 1080;
            auto streamUrl = resolver.GetStreamUrl(inputPath, preferredHeight);
            if (!streamUrl) {
                SetLastError("Failed to resolve yt-dlp URL");
                return -1;
            }
            inputPath = *streamUrl;
            LOG_INFO("Resolved to: {}", inputPath);
        }
        
        ytdlpspout::PlayerConfig playerConfig;
        playerConfig.filePath = inputPath;
        playerConfig.source = config->source;  // オリジナルのソースも保持
        playerConfig.senderName = config->senderName ? config->senderName : "ytdlpSpout";
        playerConfig.loop = config->loop != 0;
        playerConfig.useHardwareAccel = config->useHardwareAccel != 0;
        playerConfig.verbose = config->verbose != 0;
        
        // スライス設定をPlayerConfigに反映
        playerConfig.slice.enabled = config->slice.enabled != 0;
        playerConfig.slice.chunkSize = config->slice.chunkSize > 0 
            ? config->slice.chunkSize : 1024 * 1024;
        playerConfig.slice.maxCacheMemory = config->slice.maxCacheMemory > 0 
            ? config->slice.maxCacheMemory : 128 * 1024 * 1024;
        playerConfig.slice.maxConcurrentDownloads = config->slice.maxConcurrentDownloads > 0 
            ? config->slice.maxConcurrentDownloads : 4;
        playerConfig.slice.prefetchChunksAhead = config->slice.prefetchChunksAhead > 0 
            ? config->slice.prefetchChunksAhead : 8;
        if (config->slice.cachePath) {
            playerConfig.slice.cachePath = config->slice.cachePath;
        }
        
        // yt-dlp設定をPlayerConfigに反映
        if (config->ytdlp.path) {
            playerConfig.ytdlp.path = config->ytdlp.path;
        }
        playerConfig.ytdlp.preferredHeight = config->ytdlp.preferredHeight > 0 
            ? config->ytdlp.preferredHeight : 1080;
        
        if (config->slice.enabled && config->verbose) {
            LOG_INFO("Slice loading enabled: chunkSize={}, maxCache={}MB, concurrent={}, prefetch={}",
                playerConfig.slice.chunkSize,
                playerConfig.slice.maxCacheMemory / (1024 * 1024),
                playerConfig.slice.maxConcurrentDownloads,
                playerConfig.slice.prefetchChunksAhead);
        }
        
        // コールバックを設定
        if (ctx->progressCallback) {
            ctx->player->SetProgressCallback(
                [ctx](double current, double duration) {
                    if (ctx->progressCallback) {
                        ctx->progressCallback(current, duration, ctx->progressUserData);
                    }
                }
            );
        }
        
        if (ctx->errorCallback) {
            ctx->player->SetErrorCallback(
                [ctx](const std::string& message) {
                    if (ctx->errorCallback) {
                        ctx->errorCallback(message.c_str(), ctx->errorUserData);
                    }
                }
            );
        }
        
        if (ctx->completionCallback) {
            ctx->player->SetCompletionCallback(
                [ctx]() {
                    if (ctx->completionCallback) {
                        ctx->completionCallback(ctx->completionUserData);
                    }
                }
            );
        }
        
        if (!ctx->player->Start(playerConfig)) {
            SetLastError("Failed to start playback");
            return -1;
        }
        
        SetLastError("");
        return 0;
    } catch (const std::exception& e) {
        SetLastError(std::string("Exception during start: ") + e.what());
        return -1;
    }
}

double ytdlpspout_get_download_progress(YtdlpSpoutHandle handle) {
    if (!handle) return 0.0;
    auto* ctx = GetContext(handle);
    if (!ctx || !ctx->player) return 0.0;
    return ctx->player->GetDownloadProgress();
}

double ytdlpspout_get_bandwidth(YtdlpSpoutHandle handle) {
    if (!handle) return 0.0;
    // TODO: 帯域幅測定は将来実装予定
    // 現時点ではVideoPlayerからの取得方法がないため0を返す
    return 0.0;
}

int ytdlpspout_is_fully_cached(YtdlpSpoutHandle handle) {
    if (!handle) return 0;
    auto* ctx = GetContext(handle);
    if (!ctx || !ctx->player) return 0;
    return ctx->player->IsFullyCached() ? 1 : 0;
}

void ytdlpspout_get_cache_stats(
    YtdlpSpoutHandle handle,
    size_t* cachedChunks,
    size_t* totalChunks)
{
    if (cachedChunks) *cachedChunks = 0;
    if (totalChunks) *totalChunks = 0;
    
    if (!handle) return;
    auto* ctx = GetContext(handle);
    if (!ctx || !ctx->player) return;
    
    // ダウンロード進捗から推定
    // 注: 正確なチャンク数を取得するにはVideoPlayerに追加APIが必要
    double progress = ctx->player->GetDownloadProgress();
    if (progress >= 1.0) {
        // 完全にキャッシュ済み
        if (cachedChunks) *cachedChunks = 1;
        if (totalChunks) *totalChunks = 1;
    }
}

} // extern "C"
