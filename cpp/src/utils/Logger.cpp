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
#include <iostream>

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

        // ファイル出力設定
        if (logToFile && !logFilePath.empty()) {
            try {
                // ファイルシンクを作成 (truncate=true: 毎回上書き)
                auto file_sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(logFilePath, true);
                file_sink->set_level(spdlog::level::trace);
                sinks.push_back(file_sink);
            } catch (const spdlog::spdlog_ex& ex) {
                 std::cerr << "Log initialization failed: " << ex.what() << std::endl;
            }
        }

        // ロガー作成
        auto logger = std::make_shared<spdlog::logger>("ytdlpspout", sinks.begin(), sinks.end());
        
        // パターン設定: 日時 [スレッドID] [レベル] メッセージ
        logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%t] [%l] %v");
        
        // ログレベル設定
        logger->set_level(static_cast<spdlog::level::level_enum>(level));
        
        // 即時フラッシュ設定（デバッグレベル以上でフラッシュ）
        // これによりクラッシュ時でもログが保存される可能性が高まる
        logger->flush_on(spdlog::level::debug);
        
        // グローバルロガーとして登録
        spdlog::set_default_logger(logger);

        // 静的メンバに設定
        s_logger = logger;
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
