// =============================================================================
// AudioDecoder.cpp - 音声デコーダー実装
// =============================================================================

#include "AudioDecoder.h"
#include "utils/Logger.h"

// FFmpeg ヘッダー（C言語）
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/opt.h>
#include <libavutil/channel_layout.h>
#include <libswresample/swresample.h>
}

#include <mutex>
#include <algorithm>

namespace ytdlpspout {

// =============================================================================
// 内部実装クラス
// =============================================================================

struct AudioDecoder::Impl {
    // FFmpegコンテキスト
    AVFormatContext* formatCtx = nullptr;
    AVCodecContext* codecCtx = nullptr;
    SwrContext* swrCtx = nullptr;
    AVFrame* frame = nullptr;
    AVPacket* packet = nullptr;
    
    // ストリーム情報
    int audioStreamIndex = -1;
    AVRational timeBase = { 0, 1 };
    
    // 音声情報
    int originalSampleRate = 0;
    int originalChannels = 0;
    int outputSampleRate = 0;
    double duration = 0.0;
    
    // 状態
    bool isOpen = false;
    bool isEOF = false;
    
    // 内部バッファ（リサンプリング出力用）
    uint8_t* resampleBuffer = nullptr;
    int resampleBufferSize = 0;
    int resampleBufferSamples = 0;
    int resampleBufferOffset = 0;
    
    // スレッドセーフ
    std::mutex mutex;
    
    ~Impl() {
        FreeResampleBuffer();
    }
    
    void FreeResampleBuffer() {
        if (resampleBuffer) {
            av_free(resampleBuffer);
            resampleBuffer = nullptr;
        }
        resampleBufferSize = 0;
        resampleBufferSamples = 0;
        resampleBufferOffset = 0;
    }
    
    bool AllocateResampleBuffer(int samples) {
        int requiredSize = samples * sizeof(float);  // モノラル float32
        if (resampleBufferSize < requiredSize) {
            FreeResampleBuffer();
            resampleBuffer = static_cast<uint8_t*>(av_malloc(requiredSize));
            if (!resampleBuffer) {
                LOG_ERROR("Failed to allocate resample buffer");
                return false;
            }
            resampleBufferSize = requiredSize;
        }
        return true;
    }
};

// =============================================================================
// コンストラクタ・デストラクタ
// =============================================================================

AudioDecoder::AudioDecoder() : m_impl(std::make_unique<Impl>()) {
}

AudioDecoder::~AudioDecoder() {
    Close();
}

AudioDecoder::AudioDecoder(AudioDecoder&& other) noexcept
    : m_impl(std::move(other.m_impl)) {
    other.m_impl = std::make_unique<Impl>();
}

AudioDecoder& AudioDecoder::operator=(AudioDecoder&& other) noexcept {
    if (this != &other) {
        Close();
        m_impl = std::move(other.m_impl);
        other.m_impl = std::make_unique<Impl>();
    }
    return *this;
}

// =============================================================================
// ファイル操作
// =============================================================================

bool AudioDecoder::Open(const std::string& path, int targetSampleRate) {
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    
    // 既に開いている場合は閉じる
    if (m_impl->isOpen) {
        Close();
    }
    
    // フォーマットコンテキストを開く
    m_impl->formatCtx = avformat_alloc_context();
    if (!m_impl->formatCtx) {
        LOG_ERROR("Failed to allocate format context");
        return false;
    }
    
    int ret = avformat_open_input(&m_impl->formatCtx, path.c_str(), nullptr, nullptr);
    if (ret < 0) {
        char errBuf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, errBuf, sizeof(errBuf));
        LOG_ERROR("Failed to open input '{}': {}", path, errBuf);
        avformat_free_context(m_impl->formatCtx);
        m_impl->formatCtx = nullptr;
        return false;
    }
    
    // ストリーム情報を取得
    ret = avformat_find_stream_info(m_impl->formatCtx, nullptr);
    if (ret < 0) {
        LOG_ERROR("Failed to find stream info");
        avformat_close_input(&m_impl->formatCtx);
        return false;
    }
    
    // 音声ストリームを探す
    m_impl->audioStreamIndex = av_find_best_stream(
        m_impl->formatCtx, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    
    if (m_impl->audioStreamIndex < 0) {
        LOG_ERROR("No audio stream found in '{}'", path);
        avformat_close_input(&m_impl->formatCtx);
        return false;
    }
    
    AVStream* stream = m_impl->formatCtx->streams[m_impl->audioStreamIndex];
    m_impl->timeBase = stream->time_base;
    
    // デコーダーを探す
    const AVCodec* codec = avcodec_find_decoder(stream->codecpar->codec_id);
    if (!codec) {
        LOG_ERROR("Unsupported audio codec");
        avformat_close_input(&m_impl->formatCtx);
        return false;
    }
    
    // コーデックコンテキストを作成
    m_impl->codecCtx = avcodec_alloc_context3(codec);
    if (!m_impl->codecCtx) {
        LOG_ERROR("Failed to allocate codec context");
        avformat_close_input(&m_impl->formatCtx);
        return false;
    }
    
    ret = avcodec_parameters_to_context(m_impl->codecCtx, stream->codecpar);
    if (ret < 0) {
        LOG_ERROR("Failed to copy codec parameters");
        avcodec_free_context(&m_impl->codecCtx);
        avformat_close_input(&m_impl->formatCtx);
        return false;
    }
    
    ret = avcodec_open2(m_impl->codecCtx, codec, nullptr);
    if (ret < 0) {
        LOG_ERROR("Failed to open codec");
        avcodec_free_context(&m_impl->codecCtx);
        avformat_close_input(&m_impl->formatCtx);
        return false;
    }
    
    // 元の音声情報を保存
    m_impl->originalSampleRate = m_impl->codecCtx->sample_rate;
    m_impl->originalChannels = m_impl->codecCtx->ch_layout.nb_channels;
    m_impl->outputSampleRate = (targetSampleRate > 0) ? targetSampleRate : m_impl->originalSampleRate;
    
    // 再生時間を計算
    if (stream->duration != AV_NOPTS_VALUE) {
        m_impl->duration = static_cast<double>(stream->duration) * 
                          av_q2d(stream->time_base);
    } else if (m_impl->formatCtx->duration != AV_NOPTS_VALUE) {
        m_impl->duration = static_cast<double>(m_impl->formatCtx->duration) / AV_TIME_BASE;
    }
    
    // SwrContext を作成（モノラル float32 に変換）
    AVChannelLayout outLayout;
    av_channel_layout_default(&outLayout, 1);  // モノラル
    
    ret = swr_alloc_set_opts2(
        &m_impl->swrCtx,
        &outLayout,                             // 出力: モノラル
        AV_SAMPLE_FMT_FLT,                      // 出力: float32
        m_impl->outputSampleRate,               // 出力サンプルレート
        &m_impl->codecCtx->ch_layout,           // 入力チャンネルレイアウト
        m_impl->codecCtx->sample_fmt,           // 入力フォーマット
        m_impl->originalSampleRate,             // 入力サンプルレート
        0, nullptr
    );
    
    av_channel_layout_uninit(&outLayout);
    
    if (ret < 0 || !m_impl->swrCtx) {
        LOG_ERROR("Failed to create SwrContext");
        avcodec_free_context(&m_impl->codecCtx);
        avformat_close_input(&m_impl->formatCtx);
        return false;
    }
    
    ret = swr_init(m_impl->swrCtx);
    if (ret < 0) {
        LOG_ERROR("Failed to initialize SwrContext");
        swr_free(&m_impl->swrCtx);
        avcodec_free_context(&m_impl->codecCtx);
        avformat_close_input(&m_impl->formatCtx);
        return false;
    }
    
    // フレームとパケットを確保
    m_impl->frame = av_frame_alloc();
    m_impl->packet = av_packet_alloc();
    
    if (!m_impl->frame || !m_impl->packet) {
        LOG_ERROR("Failed to allocate frame/packet");
        Close();
        return false;
    }
    
    m_impl->isOpen = true;
    m_impl->isEOF = false;
    
    LOG_INFO("Opened audio '{}': {}Hz {}ch -> {}Hz mono, duration={:.2f}s",
             path, m_impl->originalSampleRate, m_impl->originalChannels,
             m_impl->outputSampleRate, m_impl->duration);
    
    return true;
}

void AudioDecoder::Close() {
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    
    if (m_impl->frame) {
        av_frame_free(&m_impl->frame);
    }
    if (m_impl->packet) {
        av_packet_free(&m_impl->packet);
    }
    if (m_impl->swrCtx) {
        swr_free(&m_impl->swrCtx);
    }
    if (m_impl->codecCtx) {
        avcodec_free_context(&m_impl->codecCtx);
    }
    if (m_impl->formatCtx) {
        avformat_close_input(&m_impl->formatCtx);
    }
    
    m_impl->FreeResampleBuffer();
    
    m_impl->audioStreamIndex = -1;
    m_impl->originalSampleRate = 0;
    m_impl->originalChannels = 0;
    m_impl->outputSampleRate = 0;
    m_impl->duration = 0.0;
    m_impl->isOpen = false;
    m_impl->isEOF = false;
}

bool AudioDecoder::IsOpen() const {
    return m_impl->isOpen;
}

// =============================================================================
// デコード
// =============================================================================

int AudioDecoder::GetSamples(float* buffer, int maxSamples) {
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    
    if (!m_impl->isOpen || !buffer || maxSamples <= 0) {
        return 0;
    }
    
    int samplesWritten = 0;
    
    // 前回のリサンプリングバッファに残りがあれば先に出力
    if (m_impl->resampleBufferSamples > m_impl->resampleBufferOffset) {
        int available = m_impl->resampleBufferSamples - m_impl->resampleBufferOffset;
        int toCopy = std::min(available, maxSamples);
        
        float* src = reinterpret_cast<float*>(m_impl->resampleBuffer) + m_impl->resampleBufferOffset;
        std::copy(src, src + toCopy, buffer);
        
        m_impl->resampleBufferOffset += toCopy;
        samplesWritten += toCopy;
        buffer += toCopy;
        maxSamples -= toCopy;
        
        if (maxSamples <= 0) {
            return samplesWritten;
        }
    }
    
    // パケットを読み込んでデコード
    while (samplesWritten < maxSamples && !m_impl->isEOF) {
        int ret = av_read_frame(m_impl->formatCtx, m_impl->packet);
        
        if (ret == AVERROR_EOF) {
            // フラッシュ
            avcodec_send_packet(m_impl->codecCtx, nullptr);
        } else if (ret < 0) {
            LOG_ERROR("Error reading frame");
            m_impl->isEOF = true;
            break;
        }
        
        // 音声ストリームのみ処理
        if (ret >= 0 && m_impl->packet->stream_index != m_impl->audioStreamIndex) {
            av_packet_unref(m_impl->packet);
            continue;
        }
        
        if (ret >= 0) {
            ret = avcodec_send_packet(m_impl->codecCtx, m_impl->packet);
            av_packet_unref(m_impl->packet);
            
            if (ret < 0 && ret != AVERROR(EAGAIN)) {
                LOG_ERROR("Error sending packet to decoder");
                continue;
            }
        }
        
        // フレームを受け取ってリサンプリング
        while (true) {
            ret = avcodec_receive_frame(m_impl->codecCtx, m_impl->frame);
            
            if (ret == AVERROR(EAGAIN)) {
                break;  // 次のパケットが必要
            } else if (ret == AVERROR_EOF) {
                m_impl->isEOF = true;
                break;
            } else if (ret < 0) {
                LOG_ERROR("Error receiving frame from decoder");
                m_impl->isEOF = true;
                break;
            }
            
            // リサンプリング出力サンプル数を計算
            int outSamples = swr_get_out_samples(m_impl->swrCtx, m_impl->frame->nb_samples);
            if (outSamples <= 0) {
                continue;
            }
            
            // バッファを確保
            if (!m_impl->AllocateResampleBuffer(outSamples)) {
                m_impl->isEOF = true;
                break;
            }
            
            // リサンプリング実行
            uint8_t* outBuffers[1] = { m_impl->resampleBuffer };
            int convertedSamples = swr_convert(
                m_impl->swrCtx,
                outBuffers, outSamples,
                const_cast<const uint8_t**>(m_impl->frame->data),
                m_impl->frame->nb_samples
            );
            
            if (convertedSamples < 0) {
                LOG_ERROR("Error in swr_convert");
                continue;
            }
            
            m_impl->resampleBufferSamples = convertedSamples;
            m_impl->resampleBufferOffset = 0;
            
            // 出力バッファにコピー
            int remaining = maxSamples - samplesWritten;
            int toCopy = std::min(convertedSamples, remaining);
            
            float* src = reinterpret_cast<float*>(m_impl->resampleBuffer);
            std::copy(src, src + toCopy, buffer);
            
            m_impl->resampleBufferOffset = toCopy;
            samplesWritten += toCopy;
            buffer += toCopy;
            
            if (samplesWritten >= maxSamples) {
                break;
            }
        }
    }
    
    return samplesWritten;
}

// =============================================================================
// 情報取得
// =============================================================================

int AudioDecoder::GetSampleRate() const {
    return m_impl->outputSampleRate;
}

int AudioDecoder::GetChannels() const {
    return m_impl->originalChannels;
}

double AudioDecoder::GetDuration() const {
    return m_impl->duration;
}

bool AudioDecoder::IsEOF() const {
    return m_impl->isEOF;
}

// =============================================================================
// シーク
// =============================================================================

bool AudioDecoder::Seek(double seconds) {
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    
    if (!m_impl->isOpen) {
        return false;
    }
    
    // タイムスタンプを計算
    int64_t timestamp = static_cast<int64_t>(seconds / av_q2d(m_impl->timeBase));
    
    int ret = av_seek_frame(m_impl->formatCtx, m_impl->audioStreamIndex,
                            timestamp, AVSEEK_FLAG_BACKWARD);
    
    if (ret < 0) {
        LOG_ERROR("Failed to seek to {:.2f}s", seconds);
        return false;
    }
    
    // デコーダーをフラッシュ
    avcodec_flush_buffers(m_impl->codecCtx);
    
    // リサンプリングバッファをクリア
    m_impl->resampleBufferSamples = 0;
    m_impl->resampleBufferOffset = 0;
    
    m_impl->isEOF = false;
    
    LOG_DEBUG("Seeked to {:.2f}s", seconds);
    return true;
}

// =============================================================================
// ユーティリティ
// =============================================================================

bool AudioDecoder::IsUrl(const std::string& path) {
    return path.find("http://") == 0 || path.find("https://") == 0;
}

} // namespace ytdlpspout
