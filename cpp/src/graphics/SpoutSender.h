// =============================================================================
// SpoutSender.h - Spout送信ラッパー
// =============================================================================
//
// 機能:
//   - Spout2 SDK（SpoutDX）を使用したテクスチャ送信
//   - DirectX11テクスチャの共有
//   - Sender名の管理
//   - 動的なサイズ変更対応
//
// =============================================================================

#pragma once

#include <d3d11.h>
#include <string>
#include <memory>

namespace ytdlpspout {

/// @brief Spoutテクスチャ送信クラス
/// @details DirectX11テクスチャをSpout経由で他のアプリケーションに共有
class SpoutSender {
public:
    SpoutSender();
    ~SpoutSender();

    // コピー禁止
    SpoutSender(const SpoutSender&) = delete;
    SpoutSender& operator=(const SpoutSender&) = delete;

    // ムーブ許可
    SpoutSender(SpoutSender&&) noexcept;
    SpoutSender& operator=(SpoutSender&&) noexcept;

    // =========================================================================
    // 初期化・終了
    // =========================================================================

    /// @brief Spout Senderの初期化
    /// @param device DirectX11デバイス
    /// @param senderName Sender名（他のアプリからこの名前で受信）
    /// @return 成功した場合true
    bool Initialize(ID3D11Device* device, const std::string& senderName);

    /// @brief リソースの解放
    void Close();

    /// @brief 初期化済みかどうか
    /// @return 初期化済みの場合true
    bool IsInitialized() const;

    // =========================================================================
    // テクスチャ送信
    // =========================================================================

    /// @brief DirectX11テクスチャを送信
    /// @param texture 送信するテクスチャ
    /// @return 成功した場合true
    bool SendTexture(ID3D11Texture2D* texture);

    /// @brief テクスチャを送信（明示的なサイズ指定）
    /// @param texture 送信するテクスチャ
    /// @param width テクスチャ幅
    /// @param height テクスチャ高さ
    /// @return 成功した場合true
    bool SendTexture(ID3D11Texture2D* texture, unsigned int width, unsigned int height);

    // =========================================================================
    // 設定
    // =========================================================================

    /// @brief Sender名を変更
    /// @param name 新しいSender名
    /// @return 成功した場合true
    bool SetSenderName(const std::string& name);

    /// @brief 送信サイズを変更
    /// @param width 新しい幅
    /// @param height 新しい高さ
    /// @return 成功した場合true
    bool SetSize(unsigned int width, unsigned int height);

    // =========================================================================
    // 状態取得
    // =========================================================================

    /// @brief 現在のSender名を取得
    /// @return Sender名
    std::string GetSenderName() const;

    /// @brief 現在の送信幅を取得
    /// @return 幅（ピクセル）
    unsigned int GetWidth() const;

    /// @brief 現在の送信高さを取得
    /// @return 高さ（ピクセル）
    unsigned int GetHeight() const;

    /// @brief フレームカウントを取得
    /// @return 送信したフレーム数
    uint64_t GetFrameCount() const;

    /// @brief FPSを取得（直近1秒間の平均）
    /// @return フレームレート
    double GetFPS() const;

private:
    // Pimpl パターン（Spout2 SDKへの依存を隠蔽）
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace ytdlpspout
