// =============================================================================
// FrameConverter.cpp - フレーム形式変換 実装
// =============================================================================

#include "FrameConverter.h"
#include "graphics/D3D11Context.h"
#include "utils/Logger.h"
#include "utils/ErrorHandling.h"

// FFmpeg ヘッダー
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
#include <libswscale/swscale.h>
}

namespace ytdlpspout {

// =============================================================================
// 内部実装クラス
// =============================================================================

struct FrameConverter::Impl {
    D3D11Context* d3dContext = nullptr;
    
    // ソフトウェアスケーラー
    SwsContext* swsContext = nullptr;
    int lastSrcWidth = 0;
    int lastSrcHeight = 0;
    AVPixelFormat lastSrcFormat = AV_PIX_FMT_NONE;
    // MED-3: 出力(dst)解像度の変化も再生成条件に含めるため記録する
    int lastDstWidth = 0;
    int lastDstHeight = 0;
    
    // 出力設定
    int outputWidth = 0;
    int outputHeight = 0;
    bool forceSwConversion = false;
    
    // GPU変換サポートフラグ（一度失敗したら無効化）
    bool gpuConversionSupported = true;
    bool gpuConversionTested = false;
    
    // 一時バッファ
    std::vector<uint8_t> rgbaBuffer;
    
    bool initialized = false;
};

// =============================================================================
// コンストラクタ・デストラクタ
// =============================================================================

FrameConverter::FrameConverter() : m_impl(std::make_unique<Impl>()) {
}

FrameConverter::~FrameConverter() {
    Shutdown();
}

// =============================================================================
// 初期化
// =============================================================================

bool FrameConverter::Initialize(D3D11Context* d3dContext) {
    if (!d3dContext || !d3dContext->IsInitialized()) {
        LOG_ERROR("Invalid D3D11Context");
        return false;
    }

    m_impl->d3dContext = d3dContext;
    m_impl->initialized = true;

    LOG_DEBUG("FrameConverter initialized");
    return true;
}

void FrameConverter::Shutdown() {
    if (m_impl->swsContext) {
        sws_freeContext(m_impl->swsContext);
        m_impl->swsContext = nullptr;
    }

    m_impl->rgbaBuffer.clear();
    m_impl->d3dContext = nullptr;
    m_impl->initialized = false;
}

bool FrameConverter::IsInitialized() const {
    return m_impl->initialized;
}

// =============================================================================
// 変換
// =============================================================================

bool FrameConverter::Convert(AVFrame* frame, ID3D11Texture2D* dstTexture) {
    if (!m_impl->initialized || !frame || !dstTexture) {
        return false;
    }

    // HWフレームかどうかを判定
    bool isHWFrame = (frame->format == AV_PIX_FMT_D3D11);

    // GPU変換を試行（一度失敗したらソフトウェアにフォールバック）
    if (isHWFrame && !m_impl->forceSwConversion && m_impl->gpuConversionSupported) {
        if (ConvertHardware(frame, dstTexture)) {
            if (!m_impl->gpuConversionTested) {
                LOG_INFO("GPU NV12->RGBA conversion enabled");
                m_impl->gpuConversionTested = true;
            }
            return true;
        }
        // GPU変換が失敗した場合、以後はソフトウェアのみ使用
        if (!m_impl->gpuConversionTested) {
            LOG_INFO("GPU conversion not supported, using software conversion");
            m_impl->gpuConversionSupported = false;
            m_impl->gpuConversionTested = true;
        }
    }
    
    return ConvertSoftware(frame, dstTexture);
}

bool FrameConverter::ConvertSoftware(AVFrame* frame, ID3D11Texture2D* dstTexture) {
    if (!frame || !dstTexture) {
        return false;
    }

    D3D11_TEXTURE2D_DESC dstDesc;
    dstTexture->GetDesc(&dstDesc);

    int srcWidth = frame->width;
    int srcHeight = frame->height;
    AVPixelFormat srcFormat = static_cast<AVPixelFormat>(frame->format);
    
    int dstWidth = m_impl->outputWidth > 0 ? m_impl->outputWidth : dstDesc.Width;
    int dstHeight = m_impl->outputHeight > 0 ? m_impl->outputHeight : dstDesc.Height;

    // HWフレームの場合はCPUに転送
    AVFrame* srcFrame = frame;
    AVFrame* swFrame = nullptr;
    
    if (srcFormat == AV_PIX_FMT_D3D11) {
        swFrame = av_frame_alloc();
        if (!swFrame) {
            LOG_ERROR("Failed to allocate SW frame");
            return false;
        }

        int ret = av_hwframe_transfer_data(swFrame, frame, 0);
        if (ret < 0) {
            LOG_ERROR("Failed to transfer HW frame to SW: {}", FFmpegErrorToString(ret));
            av_frame_free(&swFrame);
            return false;
        }

        srcFrame = swFrame;
        srcFormat = static_cast<AVPixelFormat>(swFrame->format);
    }

    // SwsContext を更新
    // MED-3: 入力(src)側だけでなく出力(dst)解像度の変化もチェックする。
    // dstのみが変化した場合に再生成をスキップすると、古いdst解像度で構成された
    // SwsContextのまま新しいdstWidth/dstHeightでバッファ確保・sws_scaleを行うことになり、
    // バッファオーバーフローや不正なスケーリングを招く。
    if (!m_impl->swsContext ||
        m_impl->lastSrcWidth != srcWidth ||
        m_impl->lastSrcHeight != srcHeight ||
        m_impl->lastSrcFormat != srcFormat ||
        m_impl->lastDstWidth != dstWidth ||
        m_impl->lastDstHeight != dstHeight) {

        if (m_impl->swsContext) {
            sws_freeContext(m_impl->swsContext);
            m_impl->swsContext = nullptr;
        }

        m_impl->swsContext = sws_getContext(
            srcWidth, srcHeight, srcFormat,
            dstWidth, dstHeight, AV_PIX_FMT_BGRA,
            SWS_BILINEAR, nullptr, nullptr, nullptr
        );

        if (!m_impl->swsContext) {
            LOG_ERROR("Failed to create sws context");
            // 生成失敗時は次回呼び出しで確実に再試行されるようキャッシュ済み
            // 寸法情報を無効化しておく
            m_impl->lastSrcWidth = 0;
            m_impl->lastSrcHeight = 0;
            m_impl->lastSrcFormat = AV_PIX_FMT_NONE;
            m_impl->lastDstWidth = 0;
            m_impl->lastDstHeight = 0;
            if (swFrame) av_frame_free(&swFrame);
            return false;
        }

        m_impl->lastSrcWidth = srcWidth;
        m_impl->lastSrcHeight = srcHeight;
        m_impl->lastSrcFormat = srcFormat;
        m_impl->lastDstWidth = dstWidth;
        m_impl->lastDstHeight = dstHeight;

        LOG_DEBUG("Created sws context: {}x{} ({}) -> {}x{} (BGRA)",
                  srcWidth, srcHeight, av_get_pix_fmt_name(srcFormat),
                  dstWidth, dstHeight);
    }

    // RGBAバッファを確保（MED-3: dst解像度に追随して必要サイズを再確保する）
    size_t bufferSize = static_cast<size_t>(dstWidth) * dstHeight * 4;
    if (m_impl->rgbaBuffer.size() < bufferSize) {
        m_impl->rgbaBuffer.resize(bufferSize);
    }

    // スケーリング実行
    uint8_t* dstData[1] = { m_impl->rgbaBuffer.data() };
    int dstLinesize[1] = { dstWidth * 4 };

    int scaledHeight = sws_scale(
        m_impl->swsContext,
        srcFrame->data, srcFrame->linesize,
        0, srcHeight,
        dstData, dstLinesize
    );

    // MED-4: sws_scale の戻り値（出力スライス高さ）を検証する。
    // 失敗(<=0)または期待した高さと一致しない場合は、未初期化/前フレームの
    // バッファをそのままアップロードしてしまわないよう false を返す。
    if (scaledHeight <= 0 || scaledHeight != dstHeight) {
        LOG_ERROR("sws_scale failed or returned unexpected height: {} (expected {})",
                   scaledHeight, dstHeight);
        if (swFrame) {
            av_frame_free(&swFrame);
        }
        return false;
    }

    // テクスチャにコピー
    bool result = m_impl->d3dContext->UpdateTexture(
        dstTexture,
        m_impl->rgbaBuffer.data(),
        dstWidth * 4
    );

    if (swFrame) {
        av_frame_free(&swFrame);
    }

    return result;
}

bool FrameConverter::ConvertHardware(AVFrame* frame, ID3D11Texture2D* dstTexture) {
    if (!frame || !dstTexture) {
        return false;
    }

    if (frame->format != AV_PIX_FMT_D3D11) {
        return false;
    }

    // HWフレームからテクスチャを取得
    ID3D11Texture2D* srcTexture = nullptr;
    UINT srcIndex = 0;

    if (!GetHWTexture(frame, &srcTexture, &srcIndex)) {
        return false;
    }

    // NV12→RGBA GPU変換
    if (!m_impl->d3dContext->ConvertNV12ToRGBA(srcTexture, srcIndex, dstTexture)) {
        return false;
    }

    return true;
}

bool FrameConverter::GetHWTexture(AVFrame* frame, ID3D11Texture2D** outTexture, UINT* outIndex) {
    if (!frame || frame->format != AV_PIX_FMT_D3D11) {
        return false;
    }

    // D3D11フレームからテクスチャを取得
    // frame->data[0] = ID3D11Texture2D*
    // frame->data[1] = テクスチャ配列インデックス
    *outTexture = reinterpret_cast<ID3D11Texture2D*>(frame->data[0]);
    *outIndex = static_cast<UINT>(reinterpret_cast<intptr_t>(frame->data[1]));

    return (*outTexture != nullptr);
}

// =============================================================================
// 設定
// =============================================================================

void FrameConverter::SetOutputSize(int width, int height) {
    m_impl->outputWidth = width;
    m_impl->outputHeight = height;
}

void FrameConverter::ForceSwConversion(bool force) {
    m_impl->forceSwConversion = force;
}

} // namespace ytdlpspout
