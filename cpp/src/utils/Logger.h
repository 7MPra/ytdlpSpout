// =============================================================================
// Logger.h - ログ出力ユーティリティ
// =============================================================================
//
// 機能:
//   - spdlogを使用した統一的なログ出力
//   - コンソールおよびファイルへの出力
//   - ログレベル（TRACE/DEBUG/INFO/WARN/ERROR/CRITICAL）対応
//
// =============================================================================

#pragma once

#include <string>
#include <memory>

// spdlog前方宣言
namespace spdlog {
    class logger;
}

namespace ytdlpspout {

// ログレベル列挙型
enum class LogLevel {
    Trace,
    Debug,
    Info,
    Warn,
    Error,
    Critical,
    Off
};

/// @brief ロガークラス
/// @details シングルトンパターンでアプリケーション全体で共有
class Logger {
public:
    /// @brief ロガーの初期化
    /// @param logToFile ファイル出力を有効にするか
    /// @param logFilePath ログファイルのパス（空の場合は "ytdlpspout.log"）
    /// @param level 初期ログレベル
    static void Initialize(
        bool logToFile = false,
        const std::string& logFilePath = "",
        LogLevel level = LogLevel::Info
    );

    /// @brief ロガーのシャットダウン
    static void Shutdown();

    /// @brief ログレベルの設定
    /// @param level 設定するログレベル
    static void SetLevel(LogLevel level);

    /// @brief 現在のログレベルを取得
    /// @return 現在のログレベル
    static LogLevel GetLevel();

    // ログ出力メソッド
    template<typename... Args>
    static void Trace(const char* fmt, Args&&... args);
    
    template<typename... Args>
    static void Debug(const char* fmt, Args&&... args);
    
    template<typename... Args>
    static void Info(const char* fmt, Args&&... args);
    
    template<typename... Args>
    static void Warn(const char* fmt, Args&&... args);
    
    template<typename... Args>
    static void Error(const char* fmt, Args&&... args);
    
    template<typename... Args>
    static void Critical(const char* fmt, Args&&... args);

    // 文字列版（フォーマットなし）
    static void TraceStr(const std::string& message);
    static void DebugStr(const std::string& message);
    static void InfoStr(const std::string& message);
    static void WarnStr(const std::string& message);
    static void ErrorStr(const std::string& message);
    static void CriticalStr(const std::string& message);

    /// @brief 内部ロガーインスタンスを取得
    /// @return spdlogロガーへの共有ポインタ
    static std::shared_ptr<spdlog::logger> GetLogger();

private:
    Logger() = default;
    ~Logger() = default;
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    static std::shared_ptr<spdlog::logger> s_logger;
    static bool s_initialized;
};

// =============================================================================
// 便利マクロ
// =============================================================================

#define LOG_TRACE(...)    ::ytdlpspout::Logger::Trace(__VA_ARGS__)
#define LOG_DEBUG(...)    ::ytdlpspout::Logger::Debug(__VA_ARGS__)
#define LOG_INFO(...)     ::ytdlpspout::Logger::Info(__VA_ARGS__)
#define LOG_WARN(...)     ::ytdlpspout::Logger::Warn(__VA_ARGS__)
#define LOG_ERROR(...)    ::ytdlpspout::Logger::Error(__VA_ARGS__)
#define LOG_CRITICAL(...) ::ytdlpspout::Logger::Critical(__VA_ARGS__)

} // namespace ytdlpspout

// =============================================================================
// テンプレート実装（ヘッダー内に必要）
// =============================================================================

#include <spdlog/spdlog.h>
#include <fmt/core.h>

namespace ytdlpspout {

template<typename... Args>
void Logger::Trace(const char* fmt, Args&&... args) {
    if (s_logger) {
        s_logger->trace(fmt::runtime(fmt), std::forward<Args>(args)...);
    }
}

template<typename... Args>
void Logger::Debug(const char* fmt, Args&&... args) {
    if (s_logger) {
        s_logger->debug(fmt::runtime(fmt), std::forward<Args>(args)...);
    }
}

template<typename... Args>
void Logger::Info(const char* fmt, Args&&... args) {
    if (s_logger) {
        s_logger->info(fmt::runtime(fmt), std::forward<Args>(args)...);
    }
}

template<typename... Args>
void Logger::Warn(const char* fmt, Args&&... args) {
    if (s_logger) {
        s_logger->warn(fmt::runtime(fmt), std::forward<Args>(args)...);
    }
}

template<typename... Args>
void Logger::Error(const char* fmt, Args&&... args) {
    if (s_logger) {
        s_logger->error(fmt::runtime(fmt), std::forward<Args>(args)...);
    }
}

template<typename... Args>
void Logger::Critical(const char* fmt, Args&&... args) {
    if (s_logger) {
        s_logger->critical(fmt::runtime(fmt), std::forward<Args>(args)...);
    }
}

} // namespace ytdlpspout
