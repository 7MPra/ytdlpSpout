// =============================================================================
// TexturePool.cpp - テクスチャプール管理実装
// =============================================================================

#include "TexturePool.h"
#include "D3D11Context.h"
#include "utils/Logger.h"
#include "utils/ErrorHandling.h"

namespace ytdlpspout {

// =============================================================================
// PooledTexture 実装
// =============================================================================

PooledTexture::PooledTexture(ComPtr<ID3D11Texture2D> texture, TexturePool* pool, size_t index)
    : m_texture(std::move(texture))
    , m_pool(pool)
    , m_index(index)
{
}

PooledTexture::~PooledTexture() {
    Release();
}

PooledTexture::PooledTexture(PooledTexture&& other) noexcept
    : m_texture(std::move(other.m_texture))
    , m_pool(other.m_pool)
    , m_index(other.m_index)
{
    other.m_pool = nullptr;
}

PooledTexture& PooledTexture::operator=(PooledTexture&& other) noexcept {
    if (this != &other) {
        Release();
        m_texture = std::move(other.m_texture);
        m_pool = other.m_pool;
        m_index = other.m_index;
        other.m_pool = nullptr;
    }
    return *this;
}

void PooledTexture::Release() {
    if (m_pool && m_texture) {
        m_pool->Release(m_index);
        m_pool = nullptr;
    }
    m_texture.Reset();
}

// =============================================================================
// TexturePool 実装
// =============================================================================

TexturePool::TexturePool() = default;

TexturePool::~TexturePool() {
    Shutdown();
}

bool TexturePool::Initialize(D3D11Context* context, const Config& config) {
    std::lock_guard<std::mutex> lock(m_mutex);

    if (!context || !context->IsInitialized()) {
        LOG_ERROR("D3D11Context is not initialized");
        return false;
    }

    m_context = context;
    m_config = config;

    LOG_INFO("Initializing TexturePool: {}x{}, size={}, format={}",
             config.width, config.height, config.poolSize, static_cast<int>(config.format));

    if (!CreateTextures(config.poolSize)) {
        LOG_ERROR("Failed to create initial texture pool");
        return false;
    }

    LOG_INFO("TexturePool initialized with {} textures", m_textures.size());
    return true;
}

bool TexturePool::Reset(const Config& config) {
    std::lock_guard<std::mutex> lock(m_mutex);

    LOG_INFO("Resetting TexturePool: {}x{} -> {}x{}",
             m_config.width, m_config.height, config.width, config.height);

    // 既存のテクスチャをクリア
    m_textures.clear();
    m_inUse.clear();
    while (!m_available.empty()) {
        m_available.pop();
    }

    m_config = config;

    if (!CreateTextures(config.poolSize)) {
        LOG_ERROR("Failed to recreate texture pool");
        return false;
    }

    return true;
}

void TexturePool::Shutdown() {
    std::lock_guard<std::mutex> lock(m_mutex);

    LOG_DEBUG("Shutting down TexturePool");

    // すべてのテクスチャを解放
    m_textures.clear();
    m_inUse.clear();
    while (!m_available.empty()) {
        m_available.pop();
    }

    m_context = nullptr;
}

bool TexturePool::CreateTextures(size_t count) {
    if (!m_context) {
        return false;
    }

    m_textures.reserve(count);
    m_inUse.reserve(count);

    for (size_t i = 0; i < count; ++i) {
        auto texture = m_context->CreateRenderTargetTexture(
            m_config.width,
            m_config.height,
            m_config.format
        );

        if (!texture) {
            LOG_ERROR("Failed to create texture {} of {}", i + 1, count);
            return false;
        }

        m_textures.push_back(std::move(texture));
        m_inUse.push_back(false);
        m_available.push(i);
    }

    return true;
}

bool TexturePool::GrowPool() {
    if (!m_config.allowGrowth) {
        return false;
    }

    if (m_textures.size() >= m_config.maxPoolSize) {
        LOG_WARN("TexturePool reached maximum size: {}", m_config.maxPoolSize);
        return false;
    }

    LOG_DEBUG("Growing TexturePool: {} -> {}", m_textures.size(), m_textures.size() + 1);

    auto texture = m_context->CreateRenderTargetTexture(
        m_config.width,
        m_config.height,
        m_config.format
    );

    if (!texture) {
        LOG_ERROR("Failed to grow texture pool");
        return false;
    }

    size_t newIndex = m_textures.size();
    m_textures.push_back(std::move(texture));
    m_inUse.push_back(false);
    m_available.push(newIndex);

    return true;
}

PooledTexture TexturePool::Acquire() {
    std::lock_guard<std::mutex> lock(m_mutex);

    // 利用可能なテクスチャがない場合、プールを拡張
    if (m_available.empty()) {
        if (!GrowPool()) {
            LOG_WARN("No available textures in pool");
            return PooledTexture();
        }
    }

    size_t index = m_available.front();
    m_available.pop();
    m_inUse[index] = true;

    LOG_TRACE("Acquired texture {} (available: {})", index, m_available.size());

    return PooledTexture(m_textures[index], this, index);
}

void TexturePool::Release(size_t index) {
    std::lock_guard<std::mutex> lock(m_mutex);

    if (index >= m_inUse.size()) {
        LOG_ERROR("Invalid texture index: {}", index);
        return;
    }

    if (!m_inUse[index]) {
        LOG_WARN("Texture {} already released", index);
        return;
    }

    m_inUse[index] = false;
    m_available.push(index);

    LOG_TRACE("Released texture {} (available: {})", index, m_available.size());
}

size_t TexturePool::GetAvailableCount() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_available.size();
}

size_t TexturePool::GetInUseCount() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    size_t count = 0;
    for (bool inUse : m_inUse) {
        if (inUse) ++count;
    }
    return count;
}

} // namespace ytdlpspout
