// =============================================================================
// HWAccelContext.h - ハードウェアアクセラレーション管理
// =============================================================================
//
// 機能:
//   - D3D11VA ハードウェアアクセラレーションの初期化・管理
//   - FFmpeg AVHWDeviceContext の設定
//   - GPUデコード用のコンテキスト管理
//
// =============================================================================

#pragma once

#include <memory>
#include <cstdint>

// 前方宣言
struct ID3D11Device;
struct AVCodecContext;
struct AVBufferRef;
enum AVPixelFormat : int;

namespace ytdlpspout {

/// @brief ハードウェアアクセラレーションコンテキストクラス
/// @details D3D11VAを使用したGPUデコードを管理
class HWAccelContext {
public:
    HWAccelContext();
    ~HWAccelContext();

    // コピー禁止
    HWAccelContext(const HWAccelContext&) = delete;
    HWAccelContext& operator=(const HWAccelContext&) = delete;

    // ムーブ許可
    HWAccelContext(HWAccelContext&&) noexcept;
    HWAccelContext& operator=(HWAccelContext&&) noexcept;

    // =========================================================================
    // 初期化
    // =========================================================================

    /// @brief ハードウェアアクセラレーションの初期化
    /// @param device DirectX11デバイス
    /// @param codecCtx FFmpegコーデックコンテキスト
    /// @return 成功した場合true
    bool Initialize(ID3D11Device* device, AVCodecContext* codecCtx);

    /// @brief リソースの解放
    void Shutdown();

    /// @brief 初期化済みかどうか
    /// @return 初期化済みの場合true
    bool IsInitialized() const;

    // =========================================================================
    // 情報取得
    // =========================================================================

    /// @brief ハードウェアピクセルフォーマットを取得
    /// @return AV_PIX_FMT_D3D11 など
    AVPixelFormat GetHWPixelFormat() const;

    /// @brief HWデバイスコンテキストを取得
    /// @return AVBufferRefポインタ（所有権は移動しない）
    AVBufferRef* GetHWDeviceContext() const;

    /// @brief HWフレームコンテキストを取得
    /// @return AVBufferRefポインタ（所有権は移動しない）
    AVBufferRef* GetHWFrameContext() const;

    /// @brief D3D11デバイスを取得
    /// @return デバイスポインタ
    ID3D11Device* GetDevice() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;

    // FFmpegコールバック（staticメンバ）
    static AVPixelFormat GetHWFormat(AVCodecContext* ctx, const AVPixelFormat* pixFmts);
};

} // namespace ytdlpspout
