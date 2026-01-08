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
#include "utils/Logger.h"
#include "utils/ErrorHandling.h"

#include <chrono>
#include <thread>
#include <mutex>

namespace ytdlpspout {

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
    std::unique_ptr<TexturePool> texturePool;
    std::unique_ptr<FrameTimer> frameTimer;
    std::unique_ptr<io::SliceLoadingManager> sliceManager;  // スライス読み込みマネージャー

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
    
    // スライス読み込みの有効/無効に応じて初期化方法を切り替え
    if (config.slice.enabled) {
        // スライス読み込みを使用
        m_impl->sliceManager = std::make_unique<io::SliceLoadingManager>();
        
        io::SliceLoadingConfig sliceConfig;
        sliceConfig.chunkSize = config.slice.chunkSize;
        sliceConfig.maxCacheMemory = config.slice.maxCacheMemory;
        sliceConfig.maxConcurrentDownloads = config.slice.maxConcurrentDownloads;
        sliceConfig.prefetchChunksAhead = config.slice.prefetchChunksAhead;
        sliceConfig.cachePath = config.slice.cachePath;
        sliceConfig.ytdlpPath = config.ytdlp.path;
        sliceConfig.preferredHeight = config.ytdlp.preferredHeight;
        
        if (!m_impl->sliceManager->Open(source, sliceConfig)) {
            // スライス読み込みに失敗した場合、従来の方法にフォールバック
            LOG_WARN("Slice loading failed, falling back to direct open");
            m_impl->sliceManager.reset();
            if (!m_impl->decoder->Open(source, device)) {
                m_impl->ReportError("Failed to open video source: " + source);
                return false;
            }
        } else {
            // スライス読み込み成功 - CustomIOContext経由でオープン
            io::CustomIOContext* ioContext = m_impl->sliceManager->GetCustomIOContext();
            if (ioContext && ioContext->IsInitialized()) {
                // CustomIOContext経由でデコーダーを開く（チャンクベース読み込み）
                if (!m_impl->decoder->OpenWithCustomIO(ioContext, device)) {
                    m_impl->ReportError("Failed to open decoder with custom IO");
                    return false;
                }
                LOG_INFO("Using slice loading with CustomIOContext for: {}", source);
            } else {
                // フォールバック: 解決したURLで従来の方法
                const std::string& resolvedUrl = m_impl->sliceManager->GetResolvedUrl();
                if (!m_impl->decoder->Open(resolvedUrl, device)) {
                    m_impl->ReportError("Failed to open video source: " + resolvedUrl);
                    return false;
                }
                LOG_WARN("Slice loading initialized but CustomIOContext unavailable, using direct URL");
            }
        }
    } else {
        // 従来の方法でオープン
        if (!m_impl->decoder->Open(source, device)) {
            m_impl->ReportError("Failed to open video file: " + source);
            return false;
        }
    }

    m_impl->videoInfo = m_impl->decoder->GetVideoInfo();

    // フレームコンバーター初期化
    m_impl->converter = std::make_unique<FrameConverter>();
    if (!m_impl->converter->Initialize(m_impl->d3dContext.get())) {
        m_impl->ReportError("Failed to initialize frame converter");
        return false;
    }

    // テクスチャプール初期化
    m_impl->texturePool = std::make_unique<TexturePool>();
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

    LOG_INFO("Video player started: {}x{} @ {:.2f}fps, duration: {:.2f}s",
             m_impl->videoInfo.width, m_impl->videoInfo.height,
             m_impl->videoInfo.fps, m_impl->videoInfo.duration);

    return true;
}

void VideoPlayer::Stop() {
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        m_impl->stopRequested = true;
    }

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
        // シーク時はタイマーをPTSに合わせてオフセット
        m_impl->frameTimer->SeekTo(seconds);
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
            if (m_impl->decoder && m_impl->decoder->IsEOF()) {
                if (m_impl->config.loop) {
                    LOG_DEBUG("Looping video");
                    m_impl->decoder->SeekToStart();
                    m_impl->currentTime = 0.0;
                    m_impl->currentFrame = 0;
                    continue;
                } else {
                    LOG_INFO("Playback completed");
                    if (m_impl->completionCallback) {
                        m_impl->completionCallback();
                    }
                    break;
                }
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

    // フレームをデコード
    if (!m_impl->decoder->DecodeNextFrame()) {
        return false;
    }

    // フレームを取得
    AVFrame* frame = m_impl->decoder->GetCurrentFrame();
    if (!frame) {
        return false;
    }

    // フレームタイミング待機（デコード後、送信前）
    // PTSに基づいた同期
    if (m_impl->frameTimer) {
        double pts = m_impl->decoder->GetCurrentPTS();
        m_impl->frameTimer->WaitUntilPTS(pts);
    }

    // テクスチャプールから取得
    auto pooledTexture = m_impl->texturePool->Acquire();
    if (!pooledTexture.IsValid()) {
        LOG_WARN("No available texture in pool");
        return true;  // スキップして続行
    }

    // フレームをテクスチャに変換
    if (!m_impl->converter->Convert(frame, pooledTexture.Get())) {
        LOG_WARN("Frame conversion failed");
        return true;  // スキップして続行
    }

    // フレームバッファにコピー（GUI連携用） - 非同期ダブルバッファリング
    {
        std::lock_guard<std::mutex> frameLock(m_impl->frameBufferMutex);
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

    // 時刻を更新
    m_impl->currentTime = m_impl->decoder->GetCurrentPTS();
    m_impl->currentFrame = m_impl->decoder->GetCurrentFrameNumber();

    // スライス読み込みマネージャーに再生位置を通知（プリフェッチ最適化用）
    if (m_impl->sliceManager) {
        m_impl->sliceManager->UpdatePlaybackPosition(m_impl->currentTime);
    }

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
    // 動画が読み込まれていない場合は0.0を返す
    if (m_impl->state == PlayerState::Stopped && !m_impl->sliceManager) {
        return 0.0;
    }
    if (!m_impl->sliceManager) {
        return 1.0;  // スライス読み込み無効時は100%
    }
    return m_impl->sliceManager->GetDownloadProgress();
}

bool VideoPlayer::IsFullyCached() const {
    // 動画が読み込まれていない場合はfalseを返す
    if (m_impl->state == PlayerState::Stopped && !m_impl->sliceManager) {
        return false;
    }
    if (!m_impl->sliceManager) {
        return true;  // スライス読み込み無効時は常にtrue
    }
    return m_impl->sliceManager->IsFullyCached();
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
