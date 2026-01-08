// =============================================================================
// VideoDecoder.h - FFmpeg動画デコーダー
// =============================================================================
//
// 機能:
//   - FFmpeg libav* を使用した動画デコード
//   - ハードウェアアクセラレーション（D3D11VA）対応
//   - ファイル/ストリームからのデコード
//   - シーク対応
//
// =============================================================================

#pragma once

#include <string>
#include <memory>
#include <functional>
#include <cstdint>

// 前方宣言
struct ID3D11Device;
struct AVFrame;

namespace ytdlpspout {

// 前方宣言
namespace io { class CustomIOContext; }

/// @brief 動画情報構造体
struct VideoInfo {
    int width = 0;              // 幅（ピクセル）
    int height = 0;             // 高さ（ピクセル）
    double fps = 0.0;           // フレームレート
    double duration = 0.0;      // 再生時間（秒）
    int64_t totalFrames = 0;    // 総フレーム数
    int64_t bitrate = 0;        // ビットレート（bps）
    std::string codecName;      // コーデック名
    std::string pixelFormat;    // ピクセルフォーマット名
    bool hasAudio = false;      // 音声トラックがあるか
};

/// @brief 動画デコーダークラス
/// @details FFmpegを使用して動画をフレーム単位でデコード
class VideoDecoder {
public:
    VideoDecoder();
    ~VideoDecoder();

    // コピー禁止
    VideoDecoder(const VideoDecoder&) = delete;
    VideoDecoder& operator=(const VideoDecoder&) = delete;

    // ムーブ許可
    VideoDecoder(VideoDecoder&&) noexcept;
    VideoDecoder& operator=(VideoDecoder&&) noexcept;

    // =========================================================================
    // ファイル操作
    // =========================================================================

    /// @brief 動画ファイルを開く
    /// @param filePath ファイルパス
    /// @param d3dDevice D3D11デバイス（ハードウェアデコード用、nullptrでソフトウェア）
    /// @return 成功した場合true
    bool Open(const std::string& filePath, ID3D11Device* d3dDevice = nullptr);

    /// @brief カスタムIOContextで動画を開く
    /// @param ioContext 事前に初期化されたCustomIOContext（外部所有）
    /// @param d3dDevice D3D11デバイス（ハードウェアデコード用、nullptrでソフトウェア）
    /// @return 成功した場合true
    bool OpenWithCustomIO(io::CustomIOContext* ioContext, ID3D11Device* d3dDevice = nullptr);

    /// @brief 動画を閉じる
    void Close();

    /// @brief ファイルが開かれているか
    /// @return 開かれている場合true
    bool IsOpen() const;

    // =========================================================================
    // デコード
    // =========================================================================

    /// @brief 次のフレームをデコード
    /// @return デコード成功=true、EOF=false
    bool DecodeNextFrame();

    /// @brief 現在のデコード済みフレームを取得
    /// @return AVFrameポインタ（所有権は移動しない）
    AVFrame* GetCurrentFrame() const;

    /// @brief 現在のフレームのPTS（表示時刻）を取得
    /// @return 秒単位の表示時刻
    double GetCurrentPTS() const;

    /// @brief 現在のフレーム番号を取得
    /// @return フレーム番号（0始まり）
    int64_t GetCurrentFrameNumber() const;

    // =========================================================================
    // シーク
    // =========================================================================

    /// @brief 指定時刻にシーク
    /// @param seconds 秒単位のシーク位置
    /// @return 成功した場合true
    bool Seek(double seconds);

    /// @brief 指定フレームにシーク
    /// @param frameNumber フレーム番号
    /// @return 成功した場合true
    bool SeekToFrame(int64_t frameNumber);

    /// @brief 先頭にシーク
    /// @return 成功した場合true
    bool SeekToStart();

    // =========================================================================
    // 情報取得
    // =========================================================================

    /// @brief 動画情報を取得
    /// @return VideoInfo構造体
    VideoInfo GetVideoInfo() const;

    /// @brief ハードウェアアクセラレーションが有効か
    /// @return 有効な場合true
    bool IsHardwareAccelerated() const;

    /// @brief EOFに達したか
    /// @return EOFの場合true
    bool IsEOF() const;

    // =========================================================================
    // コールバック
    // =========================================================================

    /// @brief エラーコールバック型
    using ErrorCallback = std::function<void(const std::string& message)>;

    /// @brief エラーコールバックを設定
    void SetErrorCallback(ErrorCallback callback);

private:
    /// @brief 内部クローズ処理（mutex取得済みの状態で呼ぶ）
    void CloseInternal();

    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace ytdlpspout
