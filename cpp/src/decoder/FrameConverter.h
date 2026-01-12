// =============================================================================
// FrameConverter.h - フレーム形式変換
// =============================================================================
//
// 機能:
//   - AVFrame から ID3D11Texture2D への変換
//   - NV12 → RGBA GPU変換
//   - ソフトウェアフォールバック（sws_scale）
//   - HWフレームからのテクスチャ抽出
//
// =============================================================================

#pragma once

#include <d3d11.h>
#include <memory>
#include "utils/ComPtr.h"

// 前方宣言
struct AVFrame;

namespace ytdlpspout {

// 前方宣言
class D3D11Context;

/// @brief フレーム変換クラス
/// @details AVFrameをDirectX11テクスチャに変換
class FrameConverter {
public:
    FrameConverter();
    ~FrameConverter();

    // コピー禁止
    FrameConverter(const FrameConverter&) = delete;
    FrameConverter& operator=(const FrameConverter&) = delete;

    // =========================================================================
    // 初期化
    // =========================================================================

    /// @brief 変換器の初期化
    /// @param d3dContext D3D11コンテキスト
    /// @return 成功した場合true
    bool Initialize(D3D11Context* d3dContext);

    /// @brief リソースの解放
    void Shutdown();

    /// @brief 初期化済みかどうか
    /// @return 初期化済みの場合true
    bool IsInitialized() const;

    // =========================================================================
    // 変換
    // =========================================================================

    /// @brief AVFrameをRGBAテクスチャに変換
    /// @param frame ソースAVFrame（SW/HW両対応）
    /// @param dstTexture 出力テクスチャ（RGBA）
    /// @return 成功した場合true
    bool Convert(AVFrame* frame, ID3D11Texture2D* dstTexture);

    /// @brief HWフレーム（D3D11）から直接テクスチャを取得
    /// @param frame HWデコードされたAVFrame
    /// @param outTexture 出力テクスチャポインタ
    /// @param outIndex 出力テクスチャ配列インデックス
    /// @return 成功した場合true
    bool GetHWTexture(AVFrame* frame, ID3D11Texture2D** outTexture, UINT* outIndex);

    // =========================================================================
    // 設定
    // =========================================================================

    /// @brief 出力サイズを設定（スケーリング用）
    /// @param width 出力幅
    /// @param height 出力高さ
    void SetOutputSize(int width, int height);

    /// @brief ソフトウェア変換を強制するか
    /// @param force trueでソフトウェア変換を強制
    void ForceSwConversion(bool force);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;

    bool ConvertSoftware(AVFrame* frame, ID3D11Texture2D* dstTexture);
    bool ConvertHardware(AVFrame* frame, ID3D11Texture2D* dstTexture);
};

} // namespace ytdlpspout
