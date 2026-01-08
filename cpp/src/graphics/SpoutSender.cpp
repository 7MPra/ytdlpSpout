// =============================================================================
// SpoutSender.cpp - Spout送信ラッパー実装
// =============================================================================

#include "SpoutSender.h"
#include "utils/Logger.h"
#include "utils/ErrorHandling.h"

// Spout2 SDK (SpoutGL/SpoutDirectX を使用してD3D11共有テクスチャを作成)
#include <SpoutGL/SpoutDirectX.h>
#include <SpoutGL/SpoutSenderNames.h>

#include <chrono>
#include <mutex>

namespace ytdlpspout {

// =============================================================================
// 内部実装クラス
// =============================================================================

struct SpoutSender::Impl {
    spoutDirectX spoutDX;                       // SpoutDirectXオブジェクト
    spoutSenderNames senderNames;               // Sender名管理
    ID3D11Device* device = nullptr;             // D3D11デバイス（借用）
    ID3D11Texture2D* sharedTexture = nullptr;   // 共有テクスチャ
    HANDLE shareHandle = nullptr;               // 共有ハンドル
    std::string senderName;                     // Sender名
    unsigned int width = 0;                     // 送信幅
    unsigned int height = 0;                    // 送信高さ
    uint64_t frameCount = 0;                    // 送信フレーム数
    bool initialized = false;                   // 初期化済みフラグ
    bool senderCreated = false;                 // Sender作成済みフラグ

    // FPS計算用
    std::chrono::steady_clock::time_point lastFpsTime;
    uint64_t lastFpsFrameCount = 0;
    double currentFps = 0.0;
    std::mutex mutex;
    
    Impl() = default;
    
    ~Impl() {
        ReleaseSender();
    }
    
    void ReleaseSender() {
        if (senderCreated) {
            senderNames.ReleaseSenderName(senderName.c_str());
            senderCreated = false;
        }
        if (sharedTexture) {
            sharedTexture->Release();
            sharedTexture = nullptr;
        }
        shareHandle = nullptr;
    }
};

// =============================================================================
// コンストラクタ・デストラクタ
// =============================================================================

SpoutSender::SpoutSender() : m_impl(std::make_unique<Impl>()) {
    m_impl->lastFpsTime = std::chrono::steady_clock::now();
}

SpoutSender::~SpoutSender() {
    Close();
}

SpoutSender::SpoutSender(SpoutSender&&) noexcept = default;
SpoutSender& SpoutSender::operator=(SpoutSender&&) noexcept = default;

// =============================================================================
// 初期化・終了
// =============================================================================

bool SpoutSender::Initialize(ID3D11Device* device, const std::string& senderName) {
    std::lock_guard<std::mutex> lock(m_impl->mutex);

    if (m_impl->initialized) {
        LOG_WARN("SpoutSender already initialized");
        return true;
    }

    if (!device) {
        LOG_ERROR("Device is null");
        return false;
    }

    if (senderName.empty()) {
        LOG_ERROR("Sender name cannot be empty");
        return false;
    }

    LOG_INFO("Initializing SpoutSender: '{}'", senderName);

    // デバイスを保存
    m_impl->device = device;

    // SpoutDirectXにデバイスを設定
    m_impl->spoutDX.SetDX11Device(device);

    // Sender名を設定
    m_impl->senderName = senderName;

    m_impl->initialized = true;
    m_impl->frameCount = 0;
    m_impl->lastFpsTime = std::chrono::steady_clock::now();
    m_impl->lastFpsFrameCount = 0;

    LOG_INFO("SpoutSender initialized successfully");
    return true;
}

void SpoutSender::Close() {
    std::lock_guard<std::mutex> lock(m_impl->mutex);

    if (!m_impl->initialized) {
        return;
    }

    LOG_INFO("Closing SpoutSender: '{}' (sent {} frames)", 
             m_impl->senderName, m_impl->frameCount);

    // Senderを解放
    m_impl->ReleaseSender();

    m_impl->device = nullptr;
    m_impl->initialized = false;
    m_impl->width = 0;
    m_impl->height = 0;
}

bool SpoutSender::IsInitialized() const {
    return m_impl->initialized;
}

// =============================================================================
// テクスチャ送信
// =============================================================================

bool SpoutSender::SendTexture(ID3D11Texture2D* texture) {
    if (!texture) {
        LOG_ERROR("Texture is null");
        return false;
    }

    // テクスチャサイズを取得
    D3D11_TEXTURE2D_DESC desc;
    texture->GetDesc(&desc);

    return SendTexture(texture, desc.Width, desc.Height);
}

bool SpoutSender::SendTexture(ID3D11Texture2D* texture, unsigned int width, unsigned int height) {
    std::lock_guard<std::mutex> lock(m_impl->mutex);

    if (!m_impl->initialized) {
        LOG_ERROR("SpoutSender not initialized");
        return false;
    }

    if (!texture) {
        LOG_ERROR("Texture is null");
        return false;
    }

    // サイズ変更をチェック - 再作成が必要
    bool needsRecreate = (width != m_impl->width || height != m_impl->height);
    
    if (needsRecreate) {
        LOG_INFO("Sender size changed: {}x{} -> {}x{}", 
                 m_impl->width, m_impl->height, width, height);
        
        // 既存のリソースを解放
        m_impl->ReleaseSender();
        
        m_impl->width = width;
        m_impl->height = height;
        
        // 共有テクスチャを作成
        DXGI_FORMAT format = DXGI_FORMAT_B8G8R8A8_UNORM;
        if (!m_impl->spoutDX.CreateSharedDX11Texture(
                m_impl->device,
                width, height,
                format,
                &m_impl->sharedTexture,
                m_impl->shareHandle)) {
            LOG_ERROR("Failed to create shared texture");
            return false;
        }
        
        // Senderを登録
        if (!m_impl->senderNames.CreateSender(
                m_impl->senderName.c_str(),
                width, height,
                m_impl->shareHandle,
                static_cast<DWORD>(format))) {
            LOG_ERROR("Failed to create sender");
            m_impl->sharedTexture->Release();
            m_impl->sharedTexture = nullptr;
            return false;
        }
        
        m_impl->senderCreated = true;
        LOG_INFO("Spout sender created: {}x{}", width, height);
    }

    // 入力テクスチャを共有テクスチャにコピー
    if (m_impl->sharedTexture && m_impl->device) {
        ID3D11DeviceContext* context = nullptr;
        m_impl->device->GetImmediateContext(&context);
        if (context) {
            context->CopyResource(m_impl->sharedTexture, texture);
            context->Flush();
            context->Release();
        }
    }
    
    m_impl->frameCount++;

    // FPS計算（1秒ごと）
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - m_impl->lastFpsTime).count();
    
    if (elapsed >= 1000) {
        uint64_t framesDelta = m_impl->frameCount - m_impl->lastFpsFrameCount;
        m_impl->currentFps = static_cast<double>(framesDelta) * 1000.0 / elapsed;
        m_impl->lastFpsTime = now;
        m_impl->lastFpsFrameCount = m_impl->frameCount;
    }

    return true;
}

// =============================================================================
// 設定
// =============================================================================

bool SpoutSender::SetSenderName(const std::string& name) {
    std::lock_guard<std::mutex> lock(m_impl->mutex);

    if (name.empty()) {
        LOG_ERROR("Sender name cannot be empty");
        return false;
    }

    if (name == m_impl->senderName) {
        return true;  // 変更なし
    }

    LOG_INFO("Changing sender name: '{}' -> '{}'", m_impl->senderName, name);

    // 既存のSenderを解放
    m_impl->ReleaseSender();
    
    m_impl->senderName = name;
    
    // サイズをリセットして次回送信時に再作成
    m_impl->width = 0;
    m_impl->height = 0;

    return true;
}

bool SpoutSender::SetSize(unsigned int width, unsigned int height) {
    std::lock_guard<std::mutex> lock(m_impl->mutex);

    if (width == 0 || height == 0) {
        LOG_ERROR("Invalid size: {}x{}", width, height);
        return false;
    }

    if (width == m_impl->width && height == m_impl->height) {
        return true;  // 変更なし
    }

    LOG_DEBUG("Setting sender size: {}x{}", width, height);
    // 次回送信時にサイズ変更を検出して再作成
    m_impl->width = 0;
    m_impl->height = 0;

    return true;
}

// =============================================================================
// 状態取得
// =============================================================================

std::string SpoutSender::GetSenderName() const {
    return m_impl->senderName;
}

unsigned int SpoutSender::GetWidth() const {
    return m_impl->width;
}

unsigned int SpoutSender::GetHeight() const {
    return m_impl->height;
}

uint64_t SpoutSender::GetFrameCount() const {
    return m_impl->frameCount;
}

double SpoutSender::GetFPS() const {
    return m_impl->currentFps;
}

} // namespace ytdlpspout
