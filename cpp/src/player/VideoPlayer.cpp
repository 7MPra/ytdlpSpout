// =============================================================================
// VideoPlayer.cpp - 動画再生制御 実装
// =============================================================================

#include "VideoPlayer.h"
#include "FrameTimer.h"
#include "decoder/VideoDecoder.h"
#include "decoder/FrameConverter.h"
#include "graphics/D3D11Context.h"
#if YTDLPSPOUT_ENABLE_SPOUT
#include "graphics/SpoutSender.h"
#endif
#include "graphics/TexturePool.h"
#include "audio/BeatMap.h"
#include "audio/BeatMapGenerator.h"
#include "io/SliceLoadingManager.h"
#include "io/CustomIOContext.h"
#include "hls/HlsSliceLoadingManager.h"
#include "utils/Logger.h"
#include "utils/ErrorHandling.h"

#include <chrono>
#include <thread>
#include <mutex>
#include <algorithm>

namespace ytdlpspout {

namespace {
// P-10: GUI側からの直近フレーム取得要求時刻からの経過時間判定に使用
int64_t NowNanos() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
// 直近このウィンドウ内にフレーム取得要求があった場合のみ非同期リードバックを行う
constexpr int64_t kReadbackActiveWindowNanos = 3LL * 1000 * 1000 * 1000;  // 3秒
}  // namespace

// =============================================================================
// 内部実装クラス
// =============================================================================

struct VideoPlayer::Impl {
    // コンポーネント
    std::unique_ptr<D3D11Context> d3dContext;
    std::unique_ptr<VideoDecoder> decoder;
    std::unique_ptr<FrameConverter> converter;
#if YTDLPSPOUT_ENABLE_SPOUT
    std::unique_ptr<SpoutSender> spoutSender;
#endif
    // Issue C: PooledTextureがweak_ptrでプールを参照するため、TexturePoolは
    // shared_ptr管理下で生成する必要がある（TexturePool.h参照）。
    std::shared_ptr<TexturePool> texturePool;
    std::unique_ptr<FrameTimer> frameTimer;
    std::unique_ptr<io::SliceLoadingManager> sliceManager;  // スライス読み込みマネージャー
    std::unique_ptr<hls::HlsSliceLoadingManager> hlsManager;  // HLSスライス読み込みマネージャー

    // 設定
    PlayerConfig config;

    // 状態
    std::atomic<PlayerState> state{ PlayerState::Stopped };
    std::atomic<bool> stopRequested{ false };
    double currentTime = 0.0;
    int64_t currentFrame = 0;
    uint64_t sentFrameCount = 0;

    // 動画情報
    VideoInfo videoInfo;

    // コールバック
    ProgressCallback progressCallback;
    ErrorCallback errorCallback;
    CompletionCallback completionCallback;

    // ビート関連
    std::shared_ptr<BeatMap> beatMap;
    BeatJumpController beatJump;
    VideoPlayer::BeatCallback beatCallback;
    int lastBeatIndex = -1;  // 最後に発火したビートのインデックス

    // HLSモードフラグ（HLSスライスローディング使用時はtrue）
    bool isHlsMode = false;
    
    // HLSストリームフラグ（シーク処理の最適化に使用、FFmpegネイティブHLS含む）
    bool isHlsStream = false;

    // シーク後タイマー再同期（最初のフレームの実際のPTSでFrameTimerを合わせる＝シーク加速防止）
    bool timerResyncPending = false;

    // 直近の有効PTS（NOPTSフレーム時の近似値算出に使用。0秒への誤同期を防ぐ）
    double lastValidPts = 0.0;

    // GUI側からの直近フレーム取得要求時刻（P-10: 未使用時のGPU→CPUリードバック省略に使用）
    // 初期値は0のためStart()完了時に現在時刻で初期化し、起動直後は必ずreadbackが有効になるようにする
    std::atomic<int64_t> lastFrameRequestNanos{ 0 };

    // スレッド同期
    std::mutex mutex;

    // フレームバッファ（GUI連携用）
    std::vector<uint8_t> lastFrameBuffer;
    std::mutex frameBufferMutex;
    bool hasValidFrame = false;

    // 非同期リードバック用ダブルバッファ
    D3D11Context::AsyncReadbackHandle pendingReadback;
    bool hasPendingReadback = false;

    // ヘルパー関数
    void ReportError(const std::string& message) {
        LOG_ERROR("{}", message);
        if (errorCallback) {
            errorCallback(message);
        }
        state = PlayerState::Error;
    }
};

// =============================================================================
// コンストラクタ・デストラクタ
// =============================================================================

VideoPlayer::VideoPlayer() : m_impl(std::make_unique<Impl>()) {
}

VideoPlayer::~VideoPlayer() {
    Stop();
}

// =============================================================================
// 再生制御
// =============================================================================

bool VideoPlayer::Start(const PlayerConfig& config) {
    std::lock_guard<std::mutex> lock(m_impl->mutex);

    if (m_impl->state == PlayerState::Playing) {
        LOG_WARN("Player is already running");
        return false;
    }

    const std::string& source = config.GetSource();
    LOG_INFO("Starting video player: {}", source);
    m_impl->config = config;
    m_impl->stopRequested = false;

    // D3D11コンテキスト初期化
    m_impl->d3dContext = std::make_unique<D3D11Context>();
    if (!m_impl->d3dContext->Initialize(true)) {
        m_impl->ReportError("Failed to initialize D3D11 context");
        return false;
    }

    // デコーダー初期化
    m_impl->decoder = std::make_unique<VideoDecoder>();
    
    ID3D11Device* device = config.useHardwareAccel ? m_impl->d3dContext->GetDevice() : nullptr;
    
    // HLS判定: FFI経由のヒント（yt-dlp側の判定結果）があればそれを優先し、
    // 未指定（-1）の場合のみURLヒューリスティックで自動判定する
    bool isHls;
    if (config.isHlsHint >= 0) {
        isHls = (config.isHlsHint != 0);
        LOG_INFO("HLS detection - using FFI hint: {}", isHls);
    } else {
        isHls = hls::HlsSliceLoadingManager::IsHlsUrl(source);
        LOG_INFO("HLS detection - HlsSliceLoadingManager::IsHlsUrl: {}", isHls);
    }
    LOG_INFO("Source URL preview: {}...", source.substr(0, std::min(source.length(), size_t(80))));
    LOG_INFO("Slice loading enabled in config: {}", config.slice.enabled);
    LOG_INFO("HTTP headers count: {}", config.httpHeaders.size());
    
    // スライスローディング使用判定
    bool useHlsSliceLoading = config.slice.enabled && isHls;
    bool useSliceLoading = config.slice.enabled && !isHls;

    // HLSプレイリストの総時間（decoder側でduration/totalFramesが取得できない場合の
    // フォールバックに使用。videoInfo代入より後段で参照するためここで保持する）
    double hlsDuration = 0.0;

    // HLSフラグを保存（シーク処理の最適化に使用）
    m_impl->isHlsStream = isHls;
    m_impl->isHlsMode = false;
    
    // スライス読み込みの有効/無効に応じて初期化方法を切り替え
    if (useHlsSliceLoading) {
        // HLSスライスローディングを使用
        LOG_INFO("HLS slice loading mode enabled for: {}", source);
        
        // HLS設定を構築
        hls::HlsSliceConfig hlsConfig;
        hlsConfig.maxCacheMemory = config.slice.maxCacheMemory;
        hlsConfig.maxConcurrentDownloads = config.slice.maxConcurrentDownloads;
        hlsConfig.prefetchSegmentsAhead = 5;  // デフォルト
        hlsConfig.readTimeoutMs = 30000;
        
        // HTTPヘッダーをコピー
        hlsConfig.httpHeaders = config.httpHeaders;
        
        // HLSマネージャーを開く
        m_impl->hlsManager = std::make_unique<hls::HlsSliceLoadingManager>();
        if (!m_impl->hlsManager->Open(source, hlsConfig)) {
            // HLSスライスローディングに失敗した場合、FFmpegネイティブにフォールバック
            LOG_WARN("HLS slice loading failed, falling back to FFmpeg native HLS");
            m_impl->hlsManager.reset();
            
            if (!m_impl->decoder->Open(source, device, config.httpHeaders)) {
                m_impl->ReportError("Failed to open HLS source: " + source);
                return false;
            }
        } else {
            // HLSマネージャーのAVIOContextでデコーダーを開く
            AVIOContext* avioCtx = m_impl->hlsManager->GetAVIOContext();
            if (!avioCtx) {
                m_impl->ReportError("Failed to get AVIO context from HLS manager");
                m_impl->hlsManager.reset();
                return false;
            }
            
            // mpegtsフォーマットヒントを削除し自動検出させる（fmp4の場合があるため）
            if (!m_impl->decoder->OpenWithAVIOContext(avioCtx, "", device, config.httpHeaders)) {
                m_impl->ReportError("Failed to open decoder with HLS AVIO");
                m_impl->hlsManager.reset();
                return false;
            }
            
            m_impl->isHlsMode = true;
            LOG_INFO("Using HLS slice loading with AVIOContext for: {}", source);

            // HLSプレイリストの総時間を取得（後段でvideoInfo.durationのフォールバックに使用）
            hlsDuration = m_impl->hlsManager->GetDuration();
        }
    } else if (useSliceLoading) {
        // スライス読み込みを使用（非HLSの場合のみ）
        m_impl->sliceManager = std::make_unique<io::SliceLoadingManager>();
        
        io::SliceLoadingConfig sliceConfig;
        sliceConfig.chunkSize = config.slice.chunkSize;
        sliceConfig.maxCacheMemory = config.slice.maxCacheMemory;
        sliceConfig.maxConcurrentDownloads = config.slice.maxConcurrentDownloads;
        sliceConfig.prefetchChunksAhead = config.slice.prefetchChunksAhead;
        sliceConfig.cachePath = config.slice.cachePath;
        sliceConfig.ytdlpPath = config.ytdlp.path;
        sliceConfig.preferredHeight = config.ytdlp.preferredHeight;
        sliceConfig.httpHeaders = config.httpHeaders;  // HTTPヘッダーを渡す
        
        if (!m_impl->sliceManager->Open(source, sliceConfig)) {
            // スライス読み込みに失敗した場合、従来の方法にフォールバック
            LOG_WARN("Slice loading failed, falling back to direct open");
            m_impl->sliceManager.reset();
            if (!m_impl->decoder->Open(source, device, config.httpHeaders)) {
                m_impl->ReportError("Failed to open video source: " + source);
                return false;
            }
        } else {
            // スライス読み込み成功 - CustomIOContext経由でオープン
            io::CustomIOContext* ioContext = m_impl->sliceManager->GetCustomIOContext();
            if (ioContext && ioContext->IsInitialized()) {
                // CustomIOContext経由でデコーダーを開く（チャンクベース読み込み）+ HTTPヘッダー
                if (!m_impl->decoder->OpenWithCustomIO(ioContext, device, config.httpHeaders)) {
                    m_impl->ReportError("Failed to open decoder with custom IO");
                    return false;
                }
                LOG_INFO("Using slice loading with CustomIOContext for: {}", source);
            } else {
                // フォールバック: 解決したURLで従来の方法
                const std::string& resolvedUrl = m_impl->sliceManager->GetResolvedUrl();
                if (!m_impl->decoder->Open(resolvedUrl, device, config.httpHeaders)) {
                    m_impl->ReportError("Failed to open video source: " + resolvedUrl);
                    return false;
                }
                LOG_WARN("Slice loading initialized but CustomIOContext unavailable, using direct URL");
            }
        }
    } else {
        // 従来の方法でオープン + HTTPヘッダー
        // HLSの場合: FFmpegネイティブHTTPハンドラ（スライス読み込みは未使用）
        // 非HLSの場合: スライスローディングが無効
        if (isHls) {
            LOG_INFO("Using FFmpeg native HLS (no HLS slice loading for this source)");
        }
        if (!m_impl->decoder->Open(source, device, config.httpHeaders)) {
            m_impl->ReportError("Failed to open video source: " + source);
            return false;
        }
    }

    m_impl->videoInfo = m_impl->decoder->GetVideoInfo();

    // HLSスライス再生時、decoder側でduration/totalFramesが取得できない場合は
    // プレイリスト総時間で補完する（GUI側のVOD判定・シークバー表示に必要）
    if (m_impl->videoInfo.duration <= 0.0 && hlsDuration > 0.0) {
        m_impl->videoInfo.duration = hlsDuration;
        if (m_impl->videoInfo.fps > 0.0) {
            m_impl->videoInfo.totalFrames = static_cast<int64_t>(hlsDuration * m_impl->videoInfo.fps);
        }
        LOG_INFO("HLS duration fallback applied: duration={:.2f}s, totalFrames={}",
                 m_impl->videoInfo.duration, m_impl->videoInfo.totalFrames);
    }

    // フレームコンバーター初期化
    m_impl->converter = std::make_unique<FrameConverter>();
    if (!m_impl->converter->Initialize(m_impl->d3dContext.get())) {
        m_impl->ReportError("Failed to initialize frame converter");
        return false;
    }

    // テクスチャプール初期化
    m_impl->texturePool = std::make_shared<TexturePool>();
    TexturePool::Config poolConfig;
    poolConfig.width = m_impl->videoInfo.width;
    poolConfig.height = m_impl->videoInfo.height;
    poolConfig.format = DXGI_FORMAT_B8G8R8A8_UNORM;
    poolConfig.poolSize = 3;

    if (!m_impl->texturePool->Initialize(m_impl->d3dContext.get(), poolConfig)) {
        m_impl->ReportError("Failed to initialize texture pool");
        return false;
    }

#if YTDLPSPOUT_ENABLE_SPOUT
    // Spout Sender初期化
    m_impl->spoutSender = std::make_unique<SpoutSender>();
    if (!m_impl->spoutSender->Initialize(m_impl->d3dContext->GetDevice(), config.senderName)) {
        m_impl->ReportError("Failed to initialize Spout sender");
        return false;
    }
#endif

    // フレームタイマー初期化
    m_impl->frameTimer = std::make_unique<FrameTimer>();
    m_impl->frameTimer->SetTargetFPS(m_impl->videoInfo.fps);
    m_impl->frameTimer->Start();  // 外部からProcessFrame()を呼ぶ場合にも必要

    m_impl->state = PlayerState::Playing;
    m_impl->currentTime = 0.0;
    m_impl->currentFrame = 0;
    m_impl->sentFrameCount = 0;
    m_impl->lastValidPts = 0.0;
    m_impl->timerResyncPending = false;

    // P-10: 起動直後はGUI側の初回フレーム取得を待たずreadbackを有効にしておく
    // （プレビューが空白のまま固まるのを防ぐための猶予期間）
    m_impl->lastFrameRequestNanos = NowNanos();

    LOG_INFO("Video player started: {}x{} @ {:.2f}fps, duration: {:.2f}s",
             m_impl->videoInfo.width, m_impl->videoInfo.height,
             m_impl->videoInfo.fps, m_impl->videoInfo.duration);

    return true;
}

void VideoPlayer::Stop() {
    // P-4: decoder/converter/texturePool/frameTimer等の破棄をProcessFrame()の
    // デコーダーアクセス区間・Seek()と同一のmutexで保護し、解放済みリソースへの
    // 並行アクセス（use-after-free）を防ぐ。ProcessFrame()はスリープ/送信中は
    // このロックを保持しないため、ここで最後まで保持してもシーク応答性は損なわれない。
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    m_impl->stopRequested = true;

    // ペンディングの非同期リードバックをクリーンアップ
    if (m_impl->hasPendingReadback && m_impl->d3dContext) {
        // GPU処理完了を待機
        m_impl->d3dContext->Flush();
        
        // ステージングテクスチャをプールに返却
        if (m_impl->pendingReadback.stagingTexture) {
            m_impl->d3dContext->ReleaseStagingTexture(m_impl->pendingReadback.stagingTexture.Get());
        }
        m_impl->pendingReadback = D3D11Context::AsyncReadbackHandle();
        m_impl->hasPendingReadback = false;
    }

    // コンポーネントを解放
#if YTDLPSPOUT_ENABLE_SPOUT
    m_impl->spoutSender.reset();
#endif
    m_impl->texturePool.reset();
    m_impl->converter.reset();
    m_impl->decoder.reset();
    m_impl->sliceManager.reset();  // スライス読み込みマネージャーを解放
    
    // HLSマネージャーをクローズ
    if (m_impl->hlsManager) {
        m_impl->hlsManager->Close();
        m_impl->hlsManager.reset();
    }
    m_impl->isHlsMode = false;
    
    m_impl->frameTimer.reset();
    m_impl->d3dContext.reset();

    m_impl->state = PlayerState::Stopped;
    LOG_INFO("Video player stopped (sent {} frames)", m_impl->sentFrameCount);
}

void VideoPlayer::RequestStop() {
    m_impl->stopRequested = true;
}

void VideoPlayer::Pause() {
    if (m_impl->state == PlayerState::Playing) {
        m_impl->state = PlayerState::Paused;
        if (m_impl->frameTimer) {
            m_impl->frameTimer->Pause();
        }
        LOG_DEBUG("Playback paused");
    }
}

void VideoPlayer::Resume() {
    if (m_impl->state == PlayerState::Paused) {
        m_impl->state = PlayerState::Playing;
        if (m_impl->frameTimer) {
            m_impl->frameTimer->Resume();
        }
        LOG_DEBUG("Playback resumed");
    }
}

void VideoPlayer::TogglePause() {
    if (m_impl->state == PlayerState::Playing) {
        Pause();
    } else if (m_impl->state == PlayerState::Paused) {
        Resume();
    }
}

// =============================================================================
// シーク
// =============================================================================

bool VideoPlayer::Seek(double seconds) {
    std::lock_guard<std::mutex> lock(m_impl->mutex);

    if (!m_impl->decoder) {
        return false;
    }

    LOG_DEBUG("Seeking to {:.2f}s", seconds);
    
    // HLSストリームの場合、シーク後にバッファリング安定化のためログ出力
    if (m_impl->isHlsStream) {
        LOG_INFO("HLS stream: performing seek with enhanced buffer flush");
    }
    
    // HLSスライス読み込みマネージャーにシークを通知
    if (m_impl->isHlsMode && m_impl->hlsManager) {
        m_impl->hlsManager->NotifySeek(seconds);
    }
    
    // スライス読み込みマネージャーにシークを通知
    if (m_impl->sliceManager) {
        m_impl->sliceManager->NotifySeek(seconds);
    }
    
    if (!m_impl->decoder->Seek(seconds)) {
        return false;
    }

    m_impl->currentTime = seconds;
    m_impl->currentFrame = static_cast<int64_t>(seconds * m_impl->videoInfo.fps);
    m_impl->lastBeatIndex = -1;  // シーク時にビートインデックスをリセット

    if (m_impl->frameTimer) {
        // シーク時はタイマーを要求位置で仮合わせ。実際のPTSは次のProcessFrameで取得するまで不明なため、
        // 最初の1フレームで timerResyncPending により実際のPTSへ再同期する（シーク後の加速防止）。
        m_impl->frameTimer->SeekTo(seconds);
        m_impl->timerResyncPending = true;
    }

    return true;
}

bool VideoPlayer::SeekRelative(double deltaSeconds) {
    double newTime = m_impl->currentTime + deltaSeconds;
    newTime = std::max(0.0, std::min(newTime, m_impl->videoInfo.duration));
    return Seek(newTime);
}

// =============================================================================
// 再生ループ
// =============================================================================

int VideoPlayer::RunLoop() {
    LOG_INFO("Entering playback loop");

    if (m_impl->frameTimer) {
        m_impl->frameTimer->Start();
    }

    while (!m_impl->stopRequested) {
        if (!ProcessFrame()) {
            // EOF または エラー
            // Issue A: ループ再生(config.loop==true)はProcessFrame()内で完結して処理される
            // ようになったため（DLL経路でProcessFrame()が直接呼ばれるGUIでもループが効くように
            // するための変更）、ここに到達するのはEOFかつloop=false時（＝再生完了）、またはエラーのみ。
            if (m_impl->decoder && m_impl->decoder->IsEOF()) {
                LOG_INFO("Playback completed");
                if (m_impl->completionCallback) {
                    m_impl->completionCallback();
                }
                break;
            } else if (m_impl->state == PlayerState::Error) {
                return 1;
            }
        }
    }

    LOG_INFO("Exiting playback loop");
    return 0;
}

bool VideoPlayer::ProcessFrame() {
    // 一時停止中は待機
    if (m_impl->state == PlayerState::Paused) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        return true;
    }

    if (m_impl->state != PlayerState::Playing) {
        return false;
    }

    // =========================================================================
    // P-4: デコーダーアクセス区間（Decode〜GetCurrentFrame〜Convert完了まで）を
    // Seek()/Stop()と同一のmutexで保護する。これにより、デコーダーが解放済み
    // （Stop()によるreset()）またはシーク中（Seek()によるavcodec_flush_buffers等）の
    // 状態でアクセスされることを防ぐ。FrameTimer::WaitUntilPTS（スリープ）と
    // Spout送信・readbackは、シーク応答性を維持するためロック外で行う
    // （変換済みテクスチャはプール所有でありデコーダーとは独立しているため安全）。
    // =========================================================================
    double waitPts = 0.0;
    double actualPts = 0.0;
    int64_t frameNumber = 0;
    PooledTexture pooledTexture;

    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);

        if (m_impl->state != PlayerState::Playing || !m_impl->decoder ||
            !m_impl->texturePool || !m_impl->converter) {
            return false;
        }

        // フレームをデコード
        if (!m_impl->decoder->DecodeNextFrame()) {
            // Issue A: DLL経路(GUI)ではPython側がProcessFrame()を毎フレーム直接呼び出し、
            // RunLoop()を経由しないため、ループ再生(config.loop)はここで完結させる必要がある
            // （RunLoop()内のEOFハンドリングはCLI専用でDLL経路には効かない）。
            if (m_impl->decoder->IsEOF() && m_impl->config.loop) {
                LOG_DEBUG("Looping video (ProcessFrame)");
                m_impl->decoder->SeekToStart();
                m_impl->currentTime = 0.0;
                m_impl->currentFrame = 0;
                m_impl->lastValidPts = 0.0;
                // シーク/ループ時と同様にFrameTimerをリセットしないと、WaitUntilPTSの基準時刻が
                // ずれたままになり2周目以降がペーシングなしの全速再生になる
                if (m_impl->frameTimer) {
                    m_impl->frameTimer->SeekTo(0.0);
                }
                m_impl->timerResyncPending = true;
                return true;  // このフレームはスキップし、次回呼び出しで先頭から再開
            }
            return false;
        }

        // フレームを取得
        AVFrame* frame = m_impl->decoder->GetCurrentFrame();
        if (!frame) {
            return false;
        }

        // P-6: PTSが未確定（AV_NOPTS_VALUE）の場合に0秒へ誤同期しないようにする。
        // タイマー再同期はTryGetCurrentPTSが成功するフレームまで持ち越し（timerResyncPending維持）、
        // 待機時刻は直前の有効PTS + フレーム間隔で近似する。
        double pts = 0.0;
        if (m_impl->decoder->TryGetCurrentPTS(pts)) {
            actualPts = pts;
            m_impl->lastValidPts = pts;
            if (m_impl->timerResyncPending && m_impl->frameTimer) {
                // シーク直後は要求位置と実際のキーフレームPTSがずれているため、最初の1フレームで
                // タイマーを実際のPTSに再同期する。これがないと WaitUntilPTS(pts) が常に負になり加速する。
                m_impl->frameTimer->SeekTo(pts);
                m_impl->timerResyncPending = false;
            }
            waitPts = pts;
        } else {
            actualPts = 0.0;  // GetCurrentPTS()の従来仕様（NOPTS時0.0）を維持
            double frameInterval = (m_impl->videoInfo.fps > 0.0) ? (1.0 / m_impl->videoInfo.fps) : (1.0 / 30.0);
            waitPts = m_impl->lastValidPts + frameInterval;
        }

        frameNumber = m_impl->decoder->GetCurrentFrameNumber();

        // テクスチャプールから取得
        pooledTexture = m_impl->texturePool->Acquire();
        if (!pooledTexture.IsValid()) {
            LOG_WARN("No available texture in pool");
            return true;  // スキップして続行
        }

        // フレームをテクスチャに変換
        if (!m_impl->converter->Convert(frame, pooledTexture.Get())) {
            LOG_WARN("Frame conversion failed");
            return true;  // スキップして続行
        }
    }  // ロック解放（以降デコーダーにはアクセスしない）

    // フレームタイミング待機（デコード後、送信前。ロック外でシーク応答性を維持）
    if (m_impl->frameTimer) {
        m_impl->frameTimer->WaitUntilPTS(waitPts);
    }

    // 送信・統計更新区間もStop()と同一mutexで保護する（スリープを含まないため
    // シーク応答性は損なわれない）。Python側のstop()はスレッドjoinが3秒で
    // タイムアウトするとProcessFrame実行中でもStop()を呼びうるため、
    // WaitUntilPTS中にStop()が完了した場合はここで検知してフレームを破棄する。
    // 注: beat/progressコールバックはPython側から同期的にseek()等を呼び返す
    // 可能性があるため、このロックの外（関数末尾）で発火する。
    {
    std::lock_guard<std::mutex> postLock(m_impl->mutex);
    if (m_impl->state != PlayerState::Playing || !m_impl->d3dContext) {
        return false;
    }

    // フレームバッファにコピー（GUI連携用） - 非同期ダブルバッファリング
    // P-10: 直近kReadbackActiveWindowNanos以内にGUI側からフレーム取得要求があった場合のみ実行し、
    // 未使用時のGPU→CPUリードバックを省略して電力・帯域を節約する
    {
        std::lock_guard<std::mutex> frameLock(m_impl->frameBufferMutex);
        bool readbackActive = (NowNanos() - m_impl->lastFrameRequestNanos.load(std::memory_order_relaxed))
                               < kReadbackActiveWindowNanos;

        if (readbackActive) {
            int width = m_impl->videoInfo.width;
            int height = m_impl->videoInfo.height;
            size_t bufferSize = static_cast<size_t>(width) * height * 4;

            if (m_impl->lastFrameBuffer.size() != bufferSize) {
                m_impl->lastFrameBuffer.resize(bufferSize);
            }

            // 前フレームの非同期リードバックが完了していれば結果を取得
            if (m_impl->hasPendingReadback) {
                if (m_impl->d3dContext->IsReadbackComplete(m_impl->pendingReadback)) {
                    if (m_impl->d3dContext->CompleteAsyncReadback(
                            m_impl->pendingReadback,
                            m_impl->lastFrameBuffer.data(),
                            bufferSize)) {
                        m_impl->hasValidFrame = true;
                    }
                    m_impl->hasPendingReadback = false;
                }
                // 完了していなければ次のフレームへ（ダブルバッファリング）
            }

            // 新しい非同期リードバックを開始（ペンディングがなければ）
            if (!m_impl->hasPendingReadback) {
                m_impl->pendingReadback = m_impl->d3dContext->BeginAsyncReadback(pooledTexture.Get());
                m_impl->hasPendingReadback = m_impl->pendingReadback.valid;
            }
        }
    }

#if YTDLPSPOUT_ENABLE_SPOUT
    // Spoutで送信
    if (!m_impl->spoutSender->SendTexture(pooledTexture.Get())) {
        LOG_WARN("Spout send failed");
    } else {
        m_impl->sentFrameCount++;
    }
#else
    // Spout無効時はフレームカウントのみ増加
    m_impl->sentFrameCount++;
#endif

    // 時刻を更新（ロック区間内で取得済みの値を使用。デコーダーへの再アクセスはしない）
    m_impl->currentTime = actualPts;
    m_impl->currentFrame = frameNumber;

    // HLSスライス読み込みマネージャーに再生位置を通知（プリフェッチ最適化用）
    if (m_impl->isHlsMode && m_impl->hlsManager) {
        m_impl->hlsManager->UpdatePlaybackPosition(m_impl->currentTime);
    }
    
    // スライス読み込みマネージャーに再生位置を通知（プリフェッチ最適化用）
    if (m_impl->sliceManager) {
        m_impl->sliceManager->UpdatePlaybackPosition(m_impl->currentTime);
    }
    }  // postLock解放（以降のコールバックはPython側へ再入しうるためロック外で発火）

    // ビートイベント発火
    if (m_impl->beatMap && m_impl->beatCallback) {
        // 現在のビートを検出
        const BeatInfo* currentBeat = m_impl->beatMap->GetNearestBeat(m_impl->currentTime);
        if (currentBeat) {
            // ビートのインデックスを探す
            int beatIndex = -1;
            for (size_t i = 0; i < m_impl->beatMap->beats.size(); ++i) {
                if (&m_impl->beatMap->beats[i] == currentBeat) {
                    beatIndex = static_cast<int>(i);
                    break;
                }
            }
            
            // 新しいビートを通過した場合のみ発火
            if (beatIndex != m_impl->lastBeatIndex && beatIndex >= 0) {
                // ビートのタイムスタンプが現在時刻より前（通過済み）であることを確認
                if (currentBeat->timestamp <= m_impl->currentTime) {
                    m_impl->lastBeatIndex = beatIndex;
                    m_impl->beatCallback(*currentBeat);
                    LOG_DEBUG("Beat fired: #{} at {:.3f}s (current: {:.3f}s)", 
                              beatIndex, currentBeat->timestamp, m_impl->currentTime);
                }
            }
        }
    }

    // 進捗コールバック
    if (m_impl->progressCallback) {
        m_impl->progressCallback(m_impl->currentTime, m_impl->videoInfo.duration);
    }

    return true;
}

// =============================================================================
// 状態取得
// =============================================================================

PlayerState VideoPlayer::GetState() const {
    return m_impl->state;
}

bool VideoPlayer::IsPlaying() const {
    return m_impl->state == PlayerState::Playing;
}

bool VideoPlayer::IsPaused() const {
    return m_impl->state == PlayerState::Paused;
}

double VideoPlayer::GetPlaybackTime() const {
    return m_impl->currentTime;
}

double VideoPlayer::GetDuration() const {
    return m_impl->videoInfo.duration;
}

int64_t VideoPlayer::GetCurrentFrame() const {
    return m_impl->currentFrame;
}

int64_t VideoPlayer::GetTotalFrames() const {
    return m_impl->videoInfo.totalFrames;
}

int VideoPlayer::GetWidth() const {
    return m_impl->videoInfo.width;
}

int VideoPlayer::GetHeight() const {
    return m_impl->videoInfo.height;
}

double VideoPlayer::GetFPS() const {
    return m_impl->videoInfo.fps;
}

uint64_t VideoPlayer::GetSentFrameCount() const {
    return m_impl->sentFrameCount;
}

// =============================================================================
// スライス読み込み状態取得
// =============================================================================

double VideoPlayer::GetDownloadProgress() const {
    // PLY-1: Stop()がm_impl->mutex保持下でsliceManager/hlsManagerをreset()するため、
    // ここでも同じmutexを取得してからポインタを参照する（use-after-free防止）。
    std::lock_guard<std::mutex> lock(m_impl->mutex);

    // 動画が読み込まれていない場合は0.0を返す
    if (m_impl->state == PlayerState::Stopped && !m_impl->sliceManager && !m_impl->hlsManager) {
        return 0.0;
    }

    // HLSスライス読み込みモード
    if (m_impl->isHlsMode && m_impl->hlsManager) {
        return m_impl->hlsManager->GetDownloadProgress();
    }

    // 通常スライス読み込み
    if (m_impl->sliceManager) {
        return m_impl->sliceManager->GetDownloadProgress();
    }

    return 1.0;  // スライス読み込み無効時は100%
}

bool VideoPlayer::IsFullyCached() const {
    // PLY-1: Stop()と同一のmutexで保護し、reset()済みポインタの参照を防ぐ。
    std::lock_guard<std::mutex> lock(m_impl->mutex);

    // 動画が読み込まれていない場合はfalseを返す
    if (m_impl->state == PlayerState::Stopped && !m_impl->sliceManager && !m_impl->hlsManager) {
        return false;
    }

    // HLSスライス読み込みモード
    if (m_impl->isHlsMode && m_impl->hlsManager) {
        return m_impl->hlsManager->IsFullyCached();
    }

    // 通常スライス読み込み
    if (m_impl->sliceManager) {
        return m_impl->sliceManager->IsFullyCached();
    }

    return true;  // スライス読み込み無効時は常にtrue
}

bool VideoPlayer::IsHlsMode() const {
    // PLY-1: isHlsModeはStop()がmutex保持下で書き換えるため、読み取り側も同期する。
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    return m_impl->isHlsMode;
}

VideoPlayer::HlsCacheStats VideoPlayer::GetHlsCacheStats() const {
    // PLY-1: Stop()と同一のmutexで保護し、reset()済みのhlsManager/sliceManagerへの
    // 並行アクセス（use-after-free）を防ぐ。破棄済み/未使用時は既定値（空stats）を返す。
    std::lock_guard<std::mutex> lock(m_impl->mutex);

    HlsCacheStats stats;

    // HLSモードの場合
    if (m_impl->isHlsMode && m_impl->hlsManager) {
        stats.cachedSegments = static_cast<int>(m_impl->hlsManager->GetCachedSegmentCount());
        stats.totalSegments = static_cast<int>(m_impl->hlsManager->GetTotalSegmentCount());
        stats.downloadProgress = m_impl->hlsManager->GetDownloadProgress();
        stats.bandwidth = m_impl->hlsManager->GetBandwidth();
        stats.isFullyCached = m_impl->hlsManager->IsFullyCached();
        stats.isHlsMode = true;
    }
    // 通常スライス読み込みの場合
    else if (m_impl->sliceManager) {
        stats.cachedSegments = static_cast<int>(m_impl->sliceManager->GetCachedChunkCount());
        stats.totalSegments = static_cast<int>(m_impl->sliceManager->GetTotalChunkCount());
        stats.downloadProgress = m_impl->sliceManager->GetDownloadProgress();
        stats.bandwidth = m_impl->sliceManager->GetBandwidth();
        stats.isFullyCached = m_impl->sliceManager->IsFullyCached();
        stats.isHlsMode = false;
    }
    // スライス読み込み無効の場合
    else {
        stats.cachedSegments = 0;
        stats.totalSegments = 0;
        stats.downloadProgress = (m_impl->state != PlayerState::Stopped) ? 1.0 : 0.0;
        stats.bandwidth = 0.0;
        stats.isFullyCached = (m_impl->state != PlayerState::Stopped);
        stats.isHlsMode = false;
    }

    return stats;
}

// =============================================================================
// コールバック
// =============================================================================

void VideoPlayer::SetProgressCallback(ProgressCallback callback) {
    m_impl->progressCallback = std::move(callback);
}

void VideoPlayer::SetErrorCallback(ErrorCallback callback) {
    m_impl->errorCallback = std::move(callback);
}

void VideoPlayer::SetCompletionCallback(CompletionCallback callback) {
    m_impl->completionCallback = std::move(callback);
}

// =============================================================================
// ビートマップ連携
// =============================================================================

void VideoPlayer::SetBeatMap(std::shared_ptr<BeatMap> beatMap) {
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    m_impl->beatMap = beatMap;
    m_impl->beatJump.SetBeatMap(beatMap);
    m_impl->lastBeatIndex = -1;  // ビートインデックスをリセット
    
    if (beatMap) {
        LOG_INFO("BeatMap set: BPM={:.1f}, {} beats", 
                 beatMap->bpm, beatMap->beats.size());
    } else {
        LOG_DEBUG("BeatMap cleared");
    }
}

std::shared_ptr<BeatMap> VideoPlayer::GetBeatMap() const {
    return m_impl->beatMap;
}

bool VideoPlayer::JumpBeats(int beats, bool forward) {
    if (!m_impl->beatMap) {
        LOG_WARN("Cannot jump beats: no beatmap set");
        return false;
    }
    
    // ジャンプ単位を決定
    BeatJumpController::JumpUnit unit;
    switch (beats) {
        case 1: unit = BeatJumpController::JumpUnit::Beat_1; break;
        case 2: unit = BeatJumpController::JumpUnit::Beat_2; break;
        case 4: unit = BeatJumpController::JumpUnit::Beat_4; break;
        case 8: unit = BeatJumpController::JumpUnit::Beat_8; break;
        case 16: unit = BeatJumpController::JumpUnit::Beat_16; break;
        default:
            LOG_WARN("Unsupported beat jump count: {}", beats);
            return false;
    }
    
    double targetTime = m_impl->beatJump.CalculateJumpTarget(
        m_impl->currentTime, unit, forward);
    
    LOG_DEBUG("JumpBeats: {} beats {} from {:.2f}s to {:.2f}s",
              beats, forward ? "forward" : "backward", 
              m_impl->currentTime, targetTime);
    
    return Seek(targetTime);
}

bool VideoPlayer::JumpToNearestBeat() {
    if (!m_impl->beatMap) {
        LOG_WARN("Cannot jump to nearest beat: no beatmap set");
        return false;
    }
    
    double targetTime = m_impl->beatJump.QuantizeToNearestBeat(m_impl->currentTime);
    
    LOG_DEBUG("JumpToNearestBeat: from {:.2f}s to {:.2f}s",
              m_impl->currentTime, targetTime);
    
    return Seek(targetTime);
}

void VideoPlayer::SetBeatCallback(BeatCallback callback) {
    m_impl->beatCallback = std::move(callback);
}

// =============================================================================
// フレームデータ取得（GUI連携用）
// =============================================================================

int VideoPlayer::GetFrameBufferSize() const {
    if (m_impl->videoInfo.width <= 0 || m_impl->videoInfo.height <= 0) {
        return 0;
    }
    return m_impl->videoInfo.width * m_impl->videoInfo.height * 4;  // BGRA = 4 bytes per pixel
}

int VideoPlayer::GetCurrentFrameData(uint8_t* buffer, int bufferSize, int* outWidth, int* outHeight) {
    // P-10: GUI側がフレームを要求したことを記録し、ProcessFrame()側の
    // GPU→CPU非同期リードバックを直近この要求から一定時間だけ有効にする
    m_impl->lastFrameRequestNanos = NowNanos();

    if (!buffer || bufferSize <= 0) {
        return -1;  // Invalid buffer
    }

    std::lock_guard<std::mutex> lock(m_impl->frameBufferMutex);

    if (!m_impl->hasValidFrame || m_impl->lastFrameBuffer.empty()) {
        return -2;  // No valid frame available
    }

    int width = m_impl->videoInfo.width;
    int height = m_impl->videoInfo.height;
    size_t requiredSize = static_cast<size_t>(width) * height * 4;

    if (static_cast<size_t>(bufferSize) < requiredSize) {
        return -3;  // Buffer too small
    }

    // フレームデータをコピー
    memcpy(buffer, m_impl->lastFrameBuffer.data(), requiredSize);

    if (outWidth) {
        *outWidth = width;
    }
    if (outHeight) {
        *outHeight = height;
    }

    return 0;  // Success
}

} // namespace ytdlpspout
