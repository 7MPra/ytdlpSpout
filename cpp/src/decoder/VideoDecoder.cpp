// =============================================================================
// VideoDecoder.cpp - FFmpeg動画デコーダー実装
// =============================================================================

#include "VideoDecoder.h"
#include "HWAccelContext.h"
#include "io/CustomIOContext.h"
#include "utils/Logger.h"
#include "utils/ErrorHandling.h"

// FFmpeg ヘッダー（C言語）
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

#include <mutex>

namespace ytdlpspout {

// =============================================================================
// 内部実装クラス
// =============================================================================

struct VideoDecoder::Impl {
    // FFmpegコンテキスト
    AVFormatContext* formatCtx = nullptr;
    AVCodecContext* codecCtx = nullptr;
    AVFrame* frame = nullptr;
    AVFrame* hwFrame = nullptr;     // HWデコード時の一時フレーム
    AVPacket* packet = nullptr;
    
    // カスタムIOコンテキスト（HTTP URL用）
    std::unique_ptr<io::CustomIOContext> customIOContext;
    
    // 外部所有のIOコンテキスト（OpenWithCustomIO用）
    io::CustomIOContext* externalIOContext = nullptr;
    
    // ストリーム情報
    int videoStreamIndex = -1;
    AVRational timeBase = { 0, 1 };
    
    // 動画情報
    VideoInfo videoInfo;
    
    // 状態
    bool isOpen = false;
    bool isEOF = false;
    bool isHardwareAccelerated = false;
    int64_t currentFrameNumber = 0;
    
    // ハードウェアアクセラレーション
    std::unique_ptr<HWAccelContext> hwAccelCtx;
    
    // コールバック
    ErrorCallback errorCallback;
    
    // スレッドセーフ
    std::mutex mutex;
    
    // ヘルパー関数
    void ReportError(const std::string& message) {
        LOG_ERROR("{}", message);
        if (errorCallback) {
            errorCallback(message);
        }
    }
};

// =============================================================================
// コンストラクタ・デストラクタ
// =============================================================================

VideoDecoder::VideoDecoder() : m_impl(std::make_unique<Impl>()) {
}

VideoDecoder::~VideoDecoder() {
    Close();
}

VideoDecoder::VideoDecoder(VideoDecoder&&) noexcept = default;
VideoDecoder& VideoDecoder::operator=(VideoDecoder&&) noexcept = default;

// =============================================================================
// ファイル操作
// =============================================================================

bool VideoDecoder::Open(const std::string& filePath, ID3D11Device* d3dDevice) {
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    
    if (m_impl->isOpen) {
        CloseInternal();
    }
    
    LOG_INFO("Opening video: {}", filePath);
    
    // URL判定（http:// または https://）
    bool isUrl = (filePath.rfind("http://", 0) == 0) || (filePath.rfind("https://", 0) == 0);
    
    int ret;
    
    if (isUrl) {
        // CustomIOContextを使用してHTTP URLからストリーミング
        LOG_INFO("Detected URL, using CustomIOContext for streaming");
        
        m_impl->customIOContext = std::make_unique<io::CustomIOContext>();
        if (!m_impl->customIOContext->Initialize(filePath)) {
            m_impl->ReportError("Failed to initialize CustomIOContext for URL: " + filePath);
            m_impl->customIOContext.reset();
            return false;
        }
        
        // AVFormatContextを割り当てし、カスタムIOをアタッチ
        m_impl->formatCtx = avformat_alloc_context();
        if (!m_impl->formatCtx) {
            m_impl->ReportError("Failed to allocate AVFormatContext");
            m_impl->customIOContext->Close();
            m_impl->customIOContext.reset();
            return false;
        }
        
        m_impl->formatCtx->pb = m_impl->customIOContext->GetAVIOContext();
        
        // URLヒント付きでオープン（フォーマット検出のため）
        ret = avformat_open_input(&m_impl->formatCtx, filePath.c_str(), nullptr, nullptr);
        if (ret < 0) {
            m_impl->ReportError("Failed to open URL input: " + FFmpegErrorToString(ret));
            // formatCtxはavformat_open_inputの失敗時にNULLにセットされる
            m_impl->customIOContext->Close();
            m_impl->customIOContext.reset();
            return false;
        }
    } else {
        // ローカルファイル（従来通り）
        ret = avformat_open_input(&m_impl->formatCtx, filePath.c_str(), nullptr, nullptr);
        if (ret < 0) {
            m_impl->ReportError("Failed to open input file: " + FFmpegErrorToString(ret));
            return false;
        }
    }
    
    // ストリーム情報を取得
    ret = avformat_find_stream_info(m_impl->formatCtx, nullptr);
    if (ret < 0) {
        m_impl->ReportError("Failed to find stream info: " + FFmpegErrorToString(ret));
        avformat_close_input(&m_impl->formatCtx);
        return false;
    }
    
    // 最適な動画ストリームを検索
    ret = av_find_best_stream(m_impl->formatCtx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (ret < 0) {
        m_impl->ReportError("No video stream found");
        avformat_close_input(&m_impl->formatCtx);
        return false;
    }
    m_impl->videoStreamIndex = ret;
    
    AVStream* videoStream = m_impl->formatCtx->streams[m_impl->videoStreamIndex];
    m_impl->timeBase = videoStream->time_base;
    
    // デコーダーを検索
    const AVCodec* codec = avcodec_find_decoder(videoStream->codecpar->codec_id);
    if (!codec) {
        m_impl->ReportError("Decoder not found for codec");
        avformat_close_input(&m_impl->formatCtx);
        return false;
    }
    
    LOG_INFO("Video codec: {}", codec->name);
    
    // コーデックコンテキストを作成
    m_impl->codecCtx = avcodec_alloc_context3(codec);
    if (!m_impl->codecCtx) {
        m_impl->ReportError("Failed to allocate codec context");
        avformat_close_input(&m_impl->formatCtx);
        return false;
    }
    
    // パラメータをコピー
    ret = avcodec_parameters_to_context(m_impl->codecCtx, videoStream->codecpar);
    if (ret < 0) {
        m_impl->ReportError("Failed to copy codec parameters: " + FFmpegErrorToString(ret));
        avcodec_free_context(&m_impl->codecCtx);
        avformat_close_input(&m_impl->formatCtx);
        return false;
    }
    
    // ハードウェアアクセラレーションの設定
    if (d3dDevice) {
        m_impl->hwAccelCtx = std::make_unique<HWAccelContext>();
        if (m_impl->hwAccelCtx->Initialize(d3dDevice, m_impl->codecCtx)) {
            m_impl->isHardwareAccelerated = true;
            LOG_INFO("Hardware acceleration (D3D11VA) enabled");
        } else {
            LOG_WARN("Hardware acceleration not available, falling back to software decoding");
            m_impl->hwAccelCtx.reset();
        }
    }
    
    // デコーダーを開く
    ret = avcodec_open2(m_impl->codecCtx, codec, nullptr);
    if (ret < 0) {
        m_impl->ReportError("Failed to open decoder: " + FFmpegErrorToString(ret));
        m_impl->hwAccelCtx.reset();
        avcodec_free_context(&m_impl->codecCtx);
        avformat_close_input(&m_impl->formatCtx);
        return false;
    }
    
    // フレーム・パケット確保
    m_impl->frame = av_frame_alloc();
    m_impl->hwFrame = av_frame_alloc();
    m_impl->packet = av_packet_alloc();
    
    if (!m_impl->frame || !m_impl->hwFrame || !m_impl->packet) {
        m_impl->ReportError("Failed to allocate frame/packet");
        Close();
        return false;
    }
    
    // 動画情報を設定
    m_impl->videoInfo.width = m_impl->codecCtx->width;
    m_impl->videoInfo.height = m_impl->codecCtx->height;
    m_impl->videoInfo.codecName = codec->name;
    
    // FPS計算
    if (videoStream->avg_frame_rate.den > 0) {
        m_impl->videoInfo.fps = av_q2d(videoStream->avg_frame_rate);
    } else if (videoStream->r_frame_rate.den > 0) {
        m_impl->videoInfo.fps = av_q2d(videoStream->r_frame_rate);
    }
    
    // 再生時間
    if (m_impl->formatCtx->duration != AV_NOPTS_VALUE) {
        m_impl->videoInfo.duration = static_cast<double>(m_impl->formatCtx->duration) / AV_TIME_BASE;
    } else if (videoStream->duration != AV_NOPTS_VALUE) {
        m_impl->videoInfo.duration = av_q2d(m_impl->timeBase) * videoStream->duration;
    }
    
    // 総フレーム数
    if (m_impl->videoInfo.fps > 0 && m_impl->videoInfo.duration > 0) {
        m_impl->videoInfo.totalFrames = static_cast<int64_t>(m_impl->videoInfo.fps * m_impl->videoInfo.duration);
    } else if (videoStream->nb_frames > 0) {
        m_impl->videoInfo.totalFrames = videoStream->nb_frames;
    }
    
    // ビットレート
    m_impl->videoInfo.bitrate = m_impl->formatCtx->bit_rate;
    
    // ピクセルフォーマット
    const char* pixFmtName = av_get_pix_fmt_name(m_impl->codecCtx->pix_fmt);
    m_impl->videoInfo.pixelFormat = pixFmtName ? pixFmtName : "unknown";
    
    // 音声ストリームの有無
    for (unsigned int i = 0; i < m_impl->formatCtx->nb_streams; ++i) {
        if (m_impl->formatCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            m_impl->videoInfo.hasAudio = true;
            break;
        }
    }
    
    m_impl->isOpen = true;
    m_impl->isEOF = false;
    m_impl->currentFrameNumber = 0;
    
    LOG_INFO("Video opened: {}x{}, {:.2f} fps, {:.2f} sec, {} frames",
             m_impl->videoInfo.width, m_impl->videoInfo.height,
             m_impl->videoInfo.fps, m_impl->videoInfo.duration,
             m_impl->videoInfo.totalFrames);
    
    return true;
}

bool VideoDecoder::OpenWithCustomIO(io::CustomIOContext* ioContext, ID3D11Device* d3dDevice) {
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    
    // 引数チェック
    if (!ioContext) {
        m_impl->ReportError("OpenWithCustomIO: ioContext is nullptr");
        return false;
    }
    
    if (!ioContext->IsInitialized()) {
        m_impl->ReportError("OpenWithCustomIO: ioContext is not initialized");
        return false;
    }
    
    if (m_impl->isOpen) {
        CloseInternal();
    }
    
    LOG_INFO("Opening video with CustomIOContext: {}", ioContext->GetUrl());
    
    // 外部IOContextを記録（所有権は外部にある）
    m_impl->externalIOContext = ioContext;
    
    // AVFormatContextを割り当て
    m_impl->formatCtx = avformat_alloc_context();
    if (!m_impl->formatCtx) {
        m_impl->ReportError("Failed to allocate AVFormatContext");
        m_impl->externalIOContext = nullptr;
        return false;
    }
    
    // 外部CustomIOContextのAVIOContextをアタッチ
    m_impl->formatCtx->pb = ioContext->GetAVIOContext();
    
    // オープン（URLヒント付き）
    int ret = avformat_open_input(&m_impl->formatCtx, ioContext->GetUrl().c_str(), nullptr, nullptr);
    if (ret < 0) {
        m_impl->ReportError("Failed to open input with CustomIOContext: " + FFmpegErrorToString(ret));
        // avformat_open_inputが失敗するとformatCtxはNULLになる
        m_impl->externalIOContext = nullptr;
        return false;
    }
    
    // ストリーム情報を取得
    ret = avformat_find_stream_info(m_impl->formatCtx, nullptr);
    if (ret < 0) {
        m_impl->ReportError("Failed to find stream info: " + FFmpegErrorToString(ret));
        m_impl->formatCtx->pb = nullptr; // 外部所有のAVIOContextを解放しない
        avformat_close_input(&m_impl->formatCtx);
        m_impl->externalIOContext = nullptr;
        return false;
    }
    
    // 最適な動画ストリームを検索
    ret = av_find_best_stream(m_impl->formatCtx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (ret < 0) {
        m_impl->ReportError("No video stream found");
        m_impl->formatCtx->pb = nullptr;
        avformat_close_input(&m_impl->formatCtx);
        m_impl->externalIOContext = nullptr;
        return false;
    }
    m_impl->videoStreamIndex = ret;
    
    AVStream* videoStream = m_impl->formatCtx->streams[m_impl->videoStreamIndex];
    m_impl->timeBase = videoStream->time_base;
    
    // デコーダーを検索
    const AVCodec* codec = avcodec_find_decoder(videoStream->codecpar->codec_id);
    if (!codec) {
        m_impl->ReportError("Decoder not found for codec");
        m_impl->formatCtx->pb = nullptr;
        avformat_close_input(&m_impl->formatCtx);
        m_impl->externalIOContext = nullptr;
        return false;
    }
    
    LOG_INFO("Video codec: {}", codec->name);
    
    // コーデックコンテキストを作成
    m_impl->codecCtx = avcodec_alloc_context3(codec);
    if (!m_impl->codecCtx) {
        m_impl->ReportError("Failed to allocate codec context");
        m_impl->formatCtx->pb = nullptr;
        avformat_close_input(&m_impl->formatCtx);
        m_impl->externalIOContext = nullptr;
        return false;
    }
    
    // パラメータをコピー
    ret = avcodec_parameters_to_context(m_impl->codecCtx, videoStream->codecpar);
    if (ret < 0) {
        m_impl->ReportError("Failed to copy codec parameters: " + FFmpegErrorToString(ret));
        avcodec_free_context(&m_impl->codecCtx);
        m_impl->formatCtx->pb = nullptr;
        avformat_close_input(&m_impl->formatCtx);
        m_impl->externalIOContext = nullptr;
        return false;
    }
    
    // ハードウェアアクセラレーションの設定
    if (d3dDevice) {
        m_impl->hwAccelCtx = std::make_unique<HWAccelContext>();
        if (m_impl->hwAccelCtx->Initialize(d3dDevice, m_impl->codecCtx)) {
            m_impl->isHardwareAccelerated = true;
            LOG_INFO("Hardware acceleration (D3D11VA) enabled");
        } else {
            LOG_WARN("Hardware acceleration not available, falling back to software decoding");
            m_impl->hwAccelCtx.reset();
        }
    }
    
    // デコーダーを開く
    ret = avcodec_open2(m_impl->codecCtx, codec, nullptr);
    if (ret < 0) {
        m_impl->ReportError("Failed to open decoder: " + FFmpegErrorToString(ret));
        m_impl->hwAccelCtx.reset();
        avcodec_free_context(&m_impl->codecCtx);
        m_impl->formatCtx->pb = nullptr;
        avformat_close_input(&m_impl->formatCtx);
        m_impl->externalIOContext = nullptr;
        return false;
    }
    
    // フレーム・パケット確保
    m_impl->frame = av_frame_alloc();
    m_impl->hwFrame = av_frame_alloc();
    m_impl->packet = av_packet_alloc();
    
    if (!m_impl->frame || !m_impl->hwFrame || !m_impl->packet) {
        m_impl->ReportError("Failed to allocate frame/packet");
        CloseInternal();
        return false;
    }
    
    // 動画情報を設定
    m_impl->videoInfo.width = m_impl->codecCtx->width;
    m_impl->videoInfo.height = m_impl->codecCtx->height;
    m_impl->videoInfo.codecName = codec->name;
    
    // FPS計算
    if (videoStream->avg_frame_rate.den > 0) {
        m_impl->videoInfo.fps = av_q2d(videoStream->avg_frame_rate);
    } else if (videoStream->r_frame_rate.den > 0) {
        m_impl->videoInfo.fps = av_q2d(videoStream->r_frame_rate);
    }
    
    // 再生時間
    if (m_impl->formatCtx->duration != AV_NOPTS_VALUE) {
        m_impl->videoInfo.duration = static_cast<double>(m_impl->formatCtx->duration) / AV_TIME_BASE;
    } else if (videoStream->duration != AV_NOPTS_VALUE) {
        m_impl->videoInfo.duration = av_q2d(m_impl->timeBase) * videoStream->duration;
    }
    
    // 総フレーム数
    if (m_impl->videoInfo.fps > 0 && m_impl->videoInfo.duration > 0) {
        m_impl->videoInfo.totalFrames = static_cast<int64_t>(m_impl->videoInfo.fps * m_impl->videoInfo.duration);
    } else if (videoStream->nb_frames > 0) {
        m_impl->videoInfo.totalFrames = videoStream->nb_frames;
    }
    
    // ビットレート
    m_impl->videoInfo.bitrate = m_impl->formatCtx->bit_rate;
    
    // ピクセルフォーマット
    const char* pixFmtName = av_get_pix_fmt_name(m_impl->codecCtx->pix_fmt);
    m_impl->videoInfo.pixelFormat = pixFmtName ? pixFmtName : "unknown";
    
    // 音声ストリームの有無
    for (unsigned int i = 0; i < m_impl->formatCtx->nb_streams; ++i) {
        if (m_impl->formatCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            m_impl->videoInfo.hasAudio = true;
            break;
        }
    }
    
    m_impl->isOpen = true;
    m_impl->isEOF = false;
    m_impl->currentFrameNumber = 0;
    
    LOG_INFO("Video opened with CustomIO: {}x{}, {:.2f} fps, {:.2f} sec, {} frames",
             m_impl->videoInfo.width, m_impl->videoInfo.height,
             m_impl->videoInfo.fps, m_impl->videoInfo.duration,
             m_impl->videoInfo.totalFrames);
    
    return true;
}

void VideoDecoder::Close() {
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    CloseInternal();
}

void VideoDecoder::CloseInternal() {
    // 注意: このメソッドはmutexがロックされた状態で呼ばれる
    // Open()やClose()から呼ばれることを想定
    
    if (!m_impl->isOpen) {
        return;
    }
    
    LOG_DEBUG("Closing video decoder");
    
    // リソース解放
    if (m_impl->packet) {
        av_packet_free(&m_impl->packet);
    }
    if (m_impl->hwFrame) {
        av_frame_free(&m_impl->hwFrame);
    }
    if (m_impl->frame) {
        av_frame_free(&m_impl->frame);
    }
    
    m_impl->hwAccelCtx.reset();
    
    if (m_impl->codecCtx) {
        avcodec_free_context(&m_impl->codecCtx);
    }
    
    // 外部IOコンテキスト使用時はavio_context解放をスキップ
    if (m_impl->formatCtx) {
        if (m_impl->externalIOContext) {
            // 外部所有のAVIOContextは解放しない
            // avformat_close_inputがavio_contextを解放しないようにnullptr設定
            m_impl->formatCtx->pb = nullptr;
        }
        avformat_close_input(&m_impl->formatCtx);
    }
    
    // 内部CustomIOContext解放
    if (m_impl->customIOContext) {
        m_impl->customIOContext->Close();
        m_impl->customIOContext.reset();
    }
    
    // 外部IOContextは所有権が外部にあるのでポインタのみクリア
    m_impl->externalIOContext = nullptr;
    
    m_impl->videoStreamIndex = -1;
    m_impl->isOpen = false;
    m_impl->isEOF = false;
    m_impl->isHardwareAccelerated = false;
    m_impl->currentFrameNumber = 0;
    m_impl->videoInfo = VideoInfo{};
}

bool VideoDecoder::IsOpen() const {
    return m_impl->isOpen;
}

// =============================================================================
// デコード
// =============================================================================

bool VideoDecoder::DecodeNextFrame() {
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    
    if (!m_impl->isOpen) {
        return false;
    }
    
    if (m_impl->isEOF) {
        return false;
    }
    
    int ret;
    
    while (true) {
        // パケット読み取り
        ret = av_read_frame(m_impl->formatCtx, m_impl->packet);
        
        if (ret < 0) {
            if (ret == AVERROR_EOF) {
                m_impl->isEOF = true;
                LOG_DEBUG("Reached end of file");
            } else {
                LOG_WARN("Error reading frame: {}", FFmpegErrorToString(ret));
            }
            return false;
        }
        
        // 動画ストリームでない場合はスキップ
        if (m_impl->packet->stream_index != m_impl->videoStreamIndex) {
            av_packet_unref(m_impl->packet);
            continue;
        }
        
        // パケットをデコーダーに送信
        ret = avcodec_send_packet(m_impl->codecCtx, m_impl->packet);
        av_packet_unref(m_impl->packet);
        
        if (ret < 0) {
            if (ret == AVERROR(EAGAIN)) {
                // フレームを先に受け取る必要がある
            } else {
                LOG_WARN("Error sending packet: {}", FFmpegErrorToString(ret));
                continue;
            }
        }
        
        // フレームを受け取る
        AVFrame* targetFrame = m_impl->isHardwareAccelerated ? m_impl->hwFrame : m_impl->frame;
        ret = avcodec_receive_frame(m_impl->codecCtx, targetFrame);
        
        if (ret == AVERROR(EAGAIN)) {
            // 次のパケットが必要
            continue;
        } else if (ret < 0) {
            LOG_WARN("Error receiving frame: {}", FFmpegErrorToString(ret));
            return false;
        }
        
        // ハードウェアフレームからソフトウェアフレームに転送（必要に応じて）
        if (m_impl->isHardwareAccelerated && targetFrame->format == m_impl->hwAccelCtx->GetHWPixelFormat()) {
            // HWフレームをそのまま使用（後でGPU変換）
            // m_impl->frame に hwFrame をコピーしない（Zero-Copyのため）
        }
        
        m_impl->currentFrameNumber++;
        return true;
    }
}

AVFrame* VideoDecoder::GetCurrentFrame() const {
    if (!m_impl->isOpen) {
        return nullptr;
    }
    
    if (m_impl->isHardwareAccelerated) {
        return m_impl->hwFrame;
    }
    return m_impl->frame;
}

double VideoDecoder::GetCurrentPTS() const {
    AVFrame* frame = GetCurrentFrame();
    if (!frame || frame->pts == AV_NOPTS_VALUE) {
        return 0.0;
    }
    return av_q2d(m_impl->timeBase) * frame->pts;
}

int64_t VideoDecoder::GetCurrentFrameNumber() const {
    return m_impl->currentFrameNumber;
}

// =============================================================================
// シーク
// =============================================================================

bool VideoDecoder::Seek(double seconds) {
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    
    if (!m_impl->isOpen) {
        return false;
    }
    
    LOG_DEBUG("Seeking to {:.2f} seconds", seconds);
    
    // タイムスタンプに変換
    int64_t timestamp = static_cast<int64_t>(seconds * AV_TIME_BASE);
    
    // シーク実行
    int ret = av_seek_frame(m_impl->formatCtx, -1, timestamp, AVSEEK_FLAG_BACKWARD);
    if (ret < 0) {
        LOG_WARN("Seek failed: {}", FFmpegErrorToString(ret));
        return false;
    }
    
    // デコーダーをフラッシュ
    avcodec_flush_buffers(m_impl->codecCtx);
    
    m_impl->isEOF = false;
    
    // フレーム番号を推定
    if (m_impl->videoInfo.fps > 0) {
        m_impl->currentFrameNumber = static_cast<int64_t>(seconds * m_impl->videoInfo.fps);
    }
    
    return true;
}

bool VideoDecoder::SeekToFrame(int64_t frameNumber) {
    if (m_impl->videoInfo.fps <= 0) {
        return false;
    }
    double seconds = static_cast<double>(frameNumber) / m_impl->videoInfo.fps;
    return Seek(seconds);
}

bool VideoDecoder::SeekToStart() {
    return Seek(0.0);
}

// =============================================================================
// 情報取得
// =============================================================================

VideoInfo VideoDecoder::GetVideoInfo() const {
    return m_impl->videoInfo;
}

bool VideoDecoder::IsHardwareAccelerated() const {
    return m_impl->isHardwareAccelerated;
}

bool VideoDecoder::IsEOF() const {
    return m_impl->isEOF;
}

void VideoDecoder::SetErrorCallback(ErrorCallback callback) {
    m_impl->errorCallback = std::move(callback);
}

} // namespace ytdlpspout
