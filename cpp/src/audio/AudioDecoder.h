// =============================================================================
// AudioDecoder.h - 音声デコーダー
// =============================================================================
//
// 機能:
//   - FFmpeg libavcodec/libswresample を使用した音声デコード
//   - 出力形式: float32、モノラル、設定可能なサンプルレート
//   - ファイル/URLからのデコード対応
//
// =============================================================================

#pragma once

#include <string>
#include <memory>
#include <cstdint>

namespace ytdlpspout {

/// @brief 音声デコーダークラス
/// @details FFmpegを使用して音声をfloat32モノラルにデコード
class AudioDecoder {
public:
    /// @brief デフォルト出力サンプルレート
    static constexpr int DEFAULT_SAMPLE_RATE = 44100;

    AudioDecoder();
    ~AudioDecoder();

    // コピー禁止
    AudioDecoder(const AudioDecoder&) = delete;
    AudioDecoder& operator=(const AudioDecoder&) = delete;

    // ムーブ許可
    AudioDecoder(AudioDecoder&&) noexcept;
    AudioDecoder& operator=(AudioDecoder&&) noexcept;

    // =========================================================================
    // ファイル操作
    // =========================================================================

    /// @brief 音声ファイルまたはURLを開く
    /// @param path ファイルパスまたはURL
    /// @param targetSampleRate 出力サンプルレート（0でオリジナル維持）
    /// @return 成功した場合true
    bool Open(const std::string& path, int targetSampleRate = DEFAULT_SAMPLE_RATE);

    /// @brief 閉じる
    void Close();

    /// @brief 開いているか確認
    /// @return 開いている場合true
    bool IsOpen() const;

    // =========================================================================
    // デコード
    // =========================================================================

    /// @brief サンプルを取得（float32、モノラル）
    /// @param buffer 出力バッファ
    /// @param maxSamples 最大サンプル数
    /// @return 実際に取得したサンプル数（EOFで0）
    int GetSamples(float* buffer, int maxSamples);

    // =========================================================================
    // 情報取得
    // =========================================================================

    /// @brief 出力サンプルレートを取得
    /// @return サンプルレート（Hz）
    int GetSampleRate() const;

    /// @brief 元のチャンネル数を取得
    /// @return チャンネル数
    int GetChannels() const;

    /// @brief 再生時間を取得
    /// @return 秒単位の再生時間
    double GetDuration() const;

    /// @brief EOFに達したか確認
    /// @return EOFの場合true
    bool IsEOF() const;

    // =========================================================================
    // シーク
    // =========================================================================

    /// @brief 指定時刻にシーク
    /// @param seconds 秒単位のシーク位置
    /// @return 成功した場合true
    bool Seek(double seconds);

    // =========================================================================
    // ユーティリティ
    // =========================================================================

    /// @brief パスがURLかどうか判定
    /// @param path パス文字列
    /// @return URLの場合true
    static bool IsUrl(const std::string& path);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace ytdlpspout
