// =============================================================================
// ErrorHandling.h - エラーハンドリングマクロ
// =============================================================================
//
// 機能:
//   - HRESULT (DirectX/COM) エラーチェック
//   - FFmpeg エラーコードチェック
//   - 汎用的な条件チェックとログ出力
//   - 例外安全なリソース管理
//
// =============================================================================

#pragma once

#include <Windows.h>
#include <string>
#include <stdexcept>
#include <format>
#include "Logger.h"

// FFmpeg エラーコード変換用
#ifdef __cplusplus
extern "C" {
#endif
#include <libavutil/error.h>
#ifdef __cplusplus
}
#endif

namespace ytdlpspout {

// =============================================================================
// HRESULT ヘルパー関数
// =============================================================================

/// @brief HRESULTを文字列に変換
/// @param hr HRESULT値
/// @return エラーメッセージ文字列
inline std::string HResultToString(HRESULT hr) {
    char* msgBuf = nullptr;
    DWORD size = FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        hr,
        MAKELANGID(LANG_ENGLISH, SUBLANG_DEFAULT),
        reinterpret_cast<LPSTR>(&msgBuf),
        0,
        nullptr
    );

    std::string result;
    if (size > 0 && msgBuf) {
        result = std::string(msgBuf, size);
        // 末尾の改行を削除
        while (!result.empty() && (result.back() == '\n' || result.back() == '\r')) {
            result.pop_back();
        }
        LocalFree(msgBuf);
    } else {
        result = std::format("Unknown error 0x{:08X}", static_cast<unsigned int>(hr));
    }

    return result;
}

/// @brief HRESULTが成功かどうかをチェック
/// @param hr チェックするHRESULT
/// @return 成功の場合true
inline bool Succeeded(HRESULT hr) {
    return SUCCEEDED(hr);
}

/// @brief HRESULTが失敗かどうかをチェック
/// @param hr チェックするHRESULT
/// @return 失敗の場合true
inline bool Failed(HRESULT hr) {
    return FAILED(hr);
}

// =============================================================================
// FFmpeg エラーヘルパー
// =============================================================================

/// @brief FFmpegエラーコードを文字列に変換
/// @param errnum FFmpegエラーコード
/// @return エラーメッセージ文字列
inline std::string FFmpegErrorToString(int errnum) {
    char errbuf[AV_ERROR_MAX_STRING_SIZE] = {0};
    av_strerror(errnum, errbuf, sizeof(errbuf));
    return std::string(errbuf);
}

// =============================================================================
// エラーチェックマクロ
// =============================================================================

/// @brief HRESULT チェック - 失敗時にログ出力してfalseを返す
#define HR_CHECK(hr, msg)                                                     \
    do {                                                                       \
        HRESULT _hr = (hr);                                                   \
        if (FAILED(_hr)) {                                                    \
            LOG_ERROR("{}: {}", (msg),                                        \
                      ::ytdlpspout::HResultToString(_hr));                    \
            return false;                                                      \
        }                                                                      \
    } while (0)

/// @brief HRESULT チェック - 失敗時にログ出力して例外をスロー
#define HR_THROW(hr, msg)                                                     \
    do {                                                                       \
        HRESULT _hr = (hr);                                                   \
        if (FAILED(_hr)) {                                                    \
            std::string _errMsg = std::format("{}: {}",                       \
                (msg), ::ytdlpspout::HResultToString(_hr));                   \
            LOG_ERROR("{}", _errMsg);                                         \
            throw std::runtime_error(_errMsg);                                \
        }                                                                      \
    } while (0)

/// @brief HRESULT チェック - 失敗時にログ出力のみ（処理は続行）
#define HR_LOG(hr, msg)                                                       \
    do {                                                                       \
        HRESULT _hr = (hr);                                                   \
        if (FAILED(_hr)) {                                                    \
            LOG_WARN("{}: {}", (msg),                                         \
                     ::ytdlpspout::HResultToString(_hr));                     \
        }                                                                      \
    } while (0)

// =============================================================================
// FFmpeg エラーチェックマクロ
// =============================================================================

#ifdef AVERROR

/// @brief FFmpegエラーチェック - 失敗時にログ出力してfalseを返す
#define FF_CHECK(ret, msg)                                                    \
    do {                                                                       \
        int _ret = (ret);                                                     \
        if (_ret < 0) {                                                       \
            LOG_ERROR("{}: {}", (msg),                                        \
                      ::ytdlpspout::FFmpegErrorToString(_ret));               \
            return false;                                                      \
        }                                                                      \
    } while (0)

/// @brief FFmpegエラーチェック - 失敗時にログ出力して例外をスロー
#define FF_THROW(ret, msg)                                                    \
    do {                                                                       \
        int _ret = (ret);                                                     \
        if (_ret < 0) {                                                       \
            std::string _errMsg = std::format("{}: {}",                       \
                (msg), ::ytdlpspout::FFmpegErrorToString(_ret));              \
            LOG_ERROR("{}", _errMsg);                                         \
            throw std::runtime_error(_errMsg);                                \
        }                                                                      \
    } while (0)

/// @brief FFmpegエラーチェック - 失敗時にログ出力のみ
#define FF_LOG(ret, msg)                                                      \
    do {                                                                       \
        int _ret = (ret);                                                     \
        if (_ret < 0) {                                                       \
            LOG_WARN("{}: {}", (msg),                                         \
                     ::ytdlpspout::FFmpegErrorToString(_ret));                \
        }                                                                      \
    } while (0)

#endif // AVERROR

// =============================================================================
// 汎用チェックマクロ
// =============================================================================

/// @brief 条件チェック - 失敗時にログ出力してfalseを返す
#define CHECK(condition, msg)                                                 \
    do {                                                                       \
        if (!(condition)) {                                                   \
            LOG_ERROR("Check failed: {}", (msg));                             \
            return false;                                                      \
        }                                                                      \
    } while (0)

/// @brief 条件チェック - 失敗時にログ出力して例外をスロー
#define CHECK_THROW(condition, msg)                                           \
    do {                                                                       \
        if (!(condition)) {                                                   \
            LOG_ERROR("Check failed: {}", (msg));                             \
            throw std::runtime_error(msg);                                    \
        }                                                                      \
    } while (0)

/// @brief NULLポインタチェック
#define CHECK_NULL(ptr, msg)                                                  \
    do {                                                                       \
        if ((ptr) == nullptr) {                                               \
            LOG_ERROR("Null pointer: {}", (msg));                             \
            return false;                                                      \
        }                                                                      \
    } while (0)

/// @brief NULLポインタチェック - 例外スロー
#define CHECK_NULL_THROW(ptr, msg)                                            \
    do {                                                                       \
        if ((ptr) == nullptr) {                                               \
            LOG_ERROR("Null pointer: {}", (msg));                             \
            throw std::runtime_error(msg);                                    \
        }                                                                      \
    } while (0)

// =============================================================================
// スコープガード（RAII）
// =============================================================================

/// @brief スコープ終了時に処理を実行するガード
template<typename Func>
class ScopeGuard {
public:
    explicit ScopeGuard(Func&& func) : m_func(std::forward<Func>(func)), m_active(true) {}
    ~ScopeGuard() { if (m_active) m_func(); }
    
    void Dismiss() { m_active = false; }
    
    // コピー禁止
    ScopeGuard(const ScopeGuard&) = delete;
    ScopeGuard& operator=(const ScopeGuard&) = delete;
    
    // ムーブ許可
    ScopeGuard(ScopeGuard&& other) noexcept 
        : m_func(std::move(other.m_func)), m_active(other.m_active) {
        other.m_active = false;
    }

private:
    Func m_func;
    bool m_active;
};

/// @brief スコープガード作成ヘルパー
template<typename Func>
ScopeGuard<Func> MakeScopeGuard(Func&& func) {
    return ScopeGuard<Func>(std::forward<Func>(func));
}

/// @brief スコープ終了時に処理を実行（マクロ版）
#define SCOPE_EXIT(code) \
    auto CONCATENATE(_scope_guard_, __LINE__) = ::ytdlpspout::MakeScopeGuard([&]() { code; })

// マクロ連結ヘルパー
#define CONCATENATE_IMPL(x, y) x##y
#define CONCATENATE(x, y) CONCATENATE_IMPL(x, y)

} // namespace ytdlpspout
