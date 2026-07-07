// =============================================================================
// TexturePool.h - テクスチャプール管理
// =============================================================================
//
// 機能:
//   - ID3D11Texture2D のプール管理
//   - テクスチャの再利用による効率的なメモリ使用
//   - デコード出力→Spout送信パイプラインの最適化
//   - スレッドセーフなテクスチャ貸し出し/返却
//
// =============================================================================

#pragma once

#include <d3d11.h>
#include <vector>
#include <queue>
#include <mutex>
#include <memory>
#include "utils/ComPtr.h"

namespace ytdlpspout {

// 前方宣言
class D3D11Context;

/// @brief プール管理されるテクスチャのハンドル
/// @details スコープ終了時に自動的にプールに返却
class PooledTexture {
public:
    PooledTexture() = default;
    ~PooledTexture();

    // コピー禁止
    PooledTexture(const PooledTexture&) = delete;
    PooledTexture& operator=(const PooledTexture&) = delete;

    // ムーブ許可
    PooledTexture(PooledTexture&& other) noexcept;
    PooledTexture& operator=(PooledTexture&& other) noexcept;

    /// @brief テクスチャへのアクセス
    ID3D11Texture2D* Get() const { return m_texture.Get(); }
    
    /// @brief テクスチャへのアクセス（演算子）
    ID3D11Texture2D* operator->() const { return m_texture.Get(); }
    
    /// @brief 有効かどうか
    bool IsValid() const { return m_texture != nullptr; }
    
    /// @brief 明示的にプールに返却
    void Release();

private:
    friend class TexturePool;

    PooledTexture(ComPtr<ID3D11Texture2D> texture, std::weak_ptr<class TexturePool> pool, size_t index);

    ComPtr<ID3D11Texture2D> m_texture;
    // Issue C: 生ポインタだとStop()等でTexturePoolが破棄された後にデストラクタが
    // 解放済みプールへアクセスしてuse-after-freeになるため、weak_ptrで安全に参照する。
    std::weak_ptr<class TexturePool> m_pool;
    size_t m_index = 0;
};

/// @brief テクスチャプールクラス
/// @details 固定サイズのテクスチャを事前確保し、効率的に再利用
/// @note Acquire()内でshared_from_this()を使用するため、本クラスは必ず
///       std::make_shared<TexturePool>() 等でshared_ptr管理下に生成すること
///       （スタック上や生ポインタでの生成はshared_from_this()呼び出し時に
///       std::bad_weak_ptrで落ちるため不可）。
class TexturePool : public std::enable_shared_from_this<TexturePool> {
public:
    /// @brief プール設定
    struct Config {
        UINT width = 1920;                              // テクスチャ幅
        UINT height = 1080;                             // テクスチャ高さ
        DXGI_FORMAT format = DXGI_FORMAT_B8G8R8A8_UNORM; // ピクセルフォーマット
        size_t poolSize = 3;                            // プールサイズ
        bool allowGrowth = true;                        // プールの動的拡張を許可
        size_t maxPoolSize = 8;                         // 最大プールサイズ
    };

    TexturePool();
    ~TexturePool();

    // コピー禁止
    TexturePool(const TexturePool&) = delete;
    TexturePool& operator=(const TexturePool&) = delete;

    // =========================================================================
    // 初期化
    // =========================================================================

    /// @brief プールを初期化
    /// @param context D3D11コンテキスト
    /// @param config プール設定
    /// @return 成功した場合true
    bool Initialize(D3D11Context* context, const Config& config);

    /// @brief プールをリセット（サイズ変更時など）
    /// @param config 新しい設定
    /// @return 成功した場合true
    bool Reset(const Config& config);

    /// @brief リソースを解放
    void Shutdown();

    // =========================================================================
    // テクスチャ管理
    // =========================================================================

    /// @brief プールからテクスチャを取得
    /// @return 利用可能なテクスチャ、なければ無効なハンドル
    PooledTexture Acquire();

    /// @brief テクスチャをプールに返却
    /// @param index テクスチャのインデックス
    void Release(size_t index);

    // =========================================================================
    // 状態取得
    // =========================================================================

    /// @brief プールサイズを取得
    size_t GetPoolSize() const { return m_config.poolSize; }

    /// @brief 利用可能なテクスチャ数を取得
    size_t GetAvailableCount() const;

    /// @brief 使用中のテクスチャ数を取得
    size_t GetInUseCount() const;

    /// @brief テクスチャ幅を取得
    UINT GetWidth() const { return m_config.width; }

    /// @brief テクスチャ高さを取得
    UINT GetHeight() const { return m_config.height; }

    /// @brief フォーマットを取得
    DXGI_FORMAT GetFormat() const { return m_config.format; }

private:
    friend class PooledTexture;

    D3D11Context* m_context = nullptr;
    Config m_config;

    std::vector<ComPtr<ID3D11Texture2D>> m_textures;  // テクスチャ配列
    std::vector<bool> m_inUse;                         // 使用中フラグ
    std::queue<size_t> m_available;                    // 利用可能なインデックス
    mutable std::mutex m_mutex;

    bool CreateTextures(size_t count);
    bool GrowPool();
};

} // namespace ytdlpspout
