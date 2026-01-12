// =============================================================================
// HWAccelContext.cpp - ハードウェアアクセラレーション管理 実装
// =============================================================================

#include "HWAccelContext.h"
#include "utils/Logger.h"
#include "utils/ErrorHandling.h"

#include <d3d11.h>

// FFmpeg ヘッダー
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
#include <libavutil/pixdesc.h>
}

namespace ytdlpspout {

// =============================================================================
// 内部実装クラス
// =============================================================================

struct HWAccelContext::Impl {
    ID3D11Device* device = nullptr;           // D3D11デバイス（借用）
    AVBufferRef* hwDeviceCtx = nullptr;       // HWデバイスコンテキスト
    AVBufferRef* hwFrameCtx = nullptr;        // HWフレームコンテキスト
    AVPixelFormat hwPixelFormat = AV_PIX_FMT_NONE;
    bool initialized = false;
};

// =============================================================================
// コンストラクタ・デストラクタ
// =============================================================================

HWAccelContext::HWAccelContext() : m_impl(std::make_unique<Impl>()) {
}

HWAccelContext::~HWAccelContext() {
    Shutdown();
}

HWAccelContext::HWAccelContext(HWAccelContext&&) noexcept = default;
HWAccelContext& HWAccelContext::operator=(HWAccelContext&&) noexcept = default;

// =============================================================================
// 初期化
// =============================================================================

bool HWAccelContext::Initialize(ID3D11Device* device, AVCodecContext* codecCtx) {
    if (!device || !codecCtx) {
        LOG_ERROR("Invalid device or codec context");
        return false;
    }

    LOG_DEBUG("Initializing D3D11VA hardware acceleration");

    m_impl->device = device;
    m_impl->hwPixelFormat = AV_PIX_FMT_D3D11;

    // HWデバイスコンテキストを作成
    m_impl->hwDeviceCtx = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
    if (!m_impl->hwDeviceCtx) {
        LOG_ERROR("Failed to allocate HW device context");
        return false;
    }

    // D3D11VAデバイスコンテキストを設定
    AVHWDeviceContext* deviceCtx = reinterpret_cast<AVHWDeviceContext*>(m_impl->hwDeviceCtx->data);
    AVD3D11VADeviceContext* d3d11vaDeviceCtx = reinterpret_cast<AVD3D11VADeviceContext*>(deviceCtx->hwctx);

    // D3D11デバイスを設定（参照カウントを増やす）
    d3d11vaDeviceCtx->device = device;
    device->AddRef();

    // デバイスコンテキストを初期化
    int ret = av_hwdevice_ctx_init(m_impl->hwDeviceCtx);
    if (ret < 0) {
        LOG_ERROR("Failed to init HW device context: {}", FFmpegErrorToString(ret));
        av_buffer_unref(&m_impl->hwDeviceCtx);
        return false;
    }

    // コーデックコンテキストにHWデバイスを設定
    codecCtx->hw_device_ctx = av_buffer_ref(m_impl->hwDeviceCtx);
    if (!codecCtx->hw_device_ctx) {
        LOG_ERROR("Failed to reference HW device context");
        av_buffer_unref(&m_impl->hwDeviceCtx);
        return false;
    }

    // ピクセルフォーマット取得コールバックを設定
    codecCtx->get_format = HWAccelContext::GetHWFormat;
    codecCtx->opaque = this;

    m_impl->initialized = true;
    LOG_INFO("D3D11VA hardware acceleration initialized successfully");

    return true;
}

void HWAccelContext::Shutdown() {
    if (!m_impl->initialized) {
        return;
    }

    LOG_DEBUG("Shutting down HW acceleration context");

    if (m_impl->hwFrameCtx) {
        av_buffer_unref(&m_impl->hwFrameCtx);
    }

    if (m_impl->hwDeviceCtx) {
        av_buffer_unref(&m_impl->hwDeviceCtx);
    }

    m_impl->device = nullptr;
    m_impl->initialized = false;
}

bool HWAccelContext::IsInitialized() const {
    return m_impl->initialized;
}

// =============================================================================
// 情報取得
// =============================================================================

AVPixelFormat HWAccelContext::GetHWPixelFormat() const {
    return m_impl->hwPixelFormat;
}

AVBufferRef* HWAccelContext::GetHWDeviceContext() const {
    return m_impl->hwDeviceCtx;
}

AVBufferRef* HWAccelContext::GetHWFrameContext() const {
    return m_impl->hwFrameCtx;
}

ID3D11Device* HWAccelContext::GetDevice() const {
    return m_impl->device;
}

// =============================================================================
// FFmpegコールバック
// =============================================================================

AVPixelFormat HWAccelContext::GetHWFormat(AVCodecContext* ctx, const AVPixelFormat* pixFmts) {
    // コンテキストからHWAccelContextを取得
    HWAccelContext* hwCtx = static_cast<HWAccelContext*>(ctx->opaque);
    AVPixelFormat hwFormat = hwCtx ? hwCtx->GetHWPixelFormat() : AV_PIX_FMT_D3D11;

    // サポートされているフォーマットを検索
    for (const AVPixelFormat* p = pixFmts; *p != AV_PIX_FMT_NONE; ++p) {
        if (*p == hwFormat) {
            LOG_DEBUG("Selected HW pixel format: {}", av_get_pix_fmt_name(*p));
            return *p;
        }
    }

    // HWフォーマットが見つからない場合はソフトウェアフォールバック
    LOG_WARN("HW pixel format not found, falling back to software");
    return pixFmts[0];
}

} // namespace ytdlpspout
