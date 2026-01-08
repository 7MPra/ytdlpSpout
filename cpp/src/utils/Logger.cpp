// =============================================================================
// Logger.cpp - ログ出力ユーティリティ実装
// =============================================================================

#include "Logger.h"

#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/rotating_file_sink.h>

#include <vector>
#include <filesystem>

namespace ytdlpspout {

// 静的メンバ初期化
std::shared_ptr<spdlog::logger> Logger::s_logger = nullptr;
bool Logger::s_initialized = false;

// =============================================================================
// 初期化・シャットダウン
// =============================================================================

void Logger::Initialize(bool logToFile, const std::string& logFilePath, LogLevel level) {
    if (s_initialized) {
        return;
    }

    try {
        // シンクのリスト
        std::vector<spdlog::sink_ptr> sinks;

        // コンソール出力シンク（カラー対応）
        auto consoleSink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
        consoleSink->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] %v");
        sinks.push_back(consoleSink);

        // ファイル出力シンク（オプション）
        if (logToFile) {
            std::string filePath = logFilePath.empty() ? "ytdlpspout.log" : logFilePath;
            
            // ローテーティングファイルシンク（5MB x 3ファイル）
            auto fileSink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
                filePath, 
                5 * 1024 * 1024,  // 5MB
                3                  // 3ファイル保持
            );
            fileSink->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] [%s:%#] %v");
            sinks.push_back(fileSink);
        }

        // マルチシンクロガーを作成
        s_logger = std::make_shared<spdlog::logger>("ytdlpspout", sinks.begin(), sinks.end());
        
        // ログレベル設定
        SetLevel(level);

        // デフォルトロガーとして登録
        spdlog::register_logger(s_logger);
        spdlog::set_default_logger(s_logger);

        // フラッシュポリシー設定（エラー以上で即座にフラッシュ）
        s_logger->flush_on(spdlog::level::err);

        s_initialized = true;

        // 初期化完了メッセージ
        s_logger->info("ytdlpSpout C++ Backend - Logger initialized");

    } catch (const spdlog::spdlog_ex& ex) {
        // spdlog初期化失敗時のフォールバック
        fprintf(stderr, "Logger initialization failed: %s\n", ex.what());
    }
}

void Logger::Shutdown() {
    if (!s_initialized) {
        return;
    }

    if (s_logger) {
        s_logger->info("Logger shutting down");
        s_logger->flush();
    }

    spdlog::drop_all();
    s_logger.reset();
    s_initialized = false;
}

// =============================================================================
// ログレベル設定
// =============================================================================

void Logger::SetLevel(LogLevel level) {
    if (!s_logger) {
        return;
    }

    spdlog::level::level_enum spdlogLevel;
    switch (level) {
        case LogLevel::Trace:    spdlogLevel = spdlog::level::trace; break;
        case LogLevel::Debug:    spdlogLevel = spdlog::level::debug; break;
        case LogLevel::Info:     spdlogLevel = spdlog::level::info; break;
        case LogLevel::Warn:     spdlogLevel = spdlog::level::warn; break;
        case LogLevel::Error:    spdlogLevel = spdlog::level::err; break;
        case LogLevel::Critical: spdlogLevel = spdlog::level::critical; break;
        case LogLevel::Off:      spdlogLevel = spdlog::level::off; break;
        default:                 spdlogLevel = spdlog::level::info; break;
    }

    s_logger->set_level(spdlogLevel);
}

LogLevel Logger::GetLevel() {
    if (!s_logger) {
        return LogLevel::Off;
    }

    switch (s_logger->level()) {
        case spdlog::level::trace:    return LogLevel::Trace;
        case spdlog::level::debug:    return LogLevel::Debug;
        case spdlog::level::info:     return LogLevel::Info;
        case spdlog::level::warn:     return LogLevel::Warn;
        case spdlog::level::err:      return LogLevel::Error;
        case spdlog::level::critical: return LogLevel::Critical;
        case spdlog::level::off:      return LogLevel::Off;
        default:                      return LogLevel::Info;
    }
}

// =============================================================================
// 文字列版ログ出力
// =============================================================================

void Logger::TraceStr(const std::string& message) {
    if (s_logger) {
        s_logger->trace(message);
    }
}

void Logger::DebugStr(const std::string& message) {
    if (s_logger) {
        s_logger->debug(message);
    }
}

void Logger::InfoStr(const std::string& message) {
    if (s_logger) {
        s_logger->info(message);
    }
}

void Logger::WarnStr(const std::string& message) {
    if (s_logger) {
        s_logger->warn(message);
    }
}

void Logger::ErrorStr(const std::string& message) {
    if (s_logger) {
        s_logger->error(message);
    }
}

void Logger::CriticalStr(const std::string& message) {
    if (s_logger) {
        s_logger->critical(message);
    }
}

// =============================================================================
// 内部ロガー取得
// =============================================================================

std::shared_ptr<spdlog::logger> Logger::GetLogger() {
    // 未初期化の場合は自動初期化
    if (!s_initialized) {
        Initialize();
    }
    return s_logger;
}

} // namespace ytdlpspout
