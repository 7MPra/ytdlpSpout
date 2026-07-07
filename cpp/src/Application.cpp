// =============================================================================
// Application.cpp - アプリケーションメインロジック 実装
// =============================================================================

#include "Application.h"
#include "player/VideoPlayer.h"
#include "ytdlp/YtDlpResolver.h"
#include "audio/BeatMap.h"
#include "audio/BeatMapGenerator.h"
#include "utils/Logger.h"

#include <CLI/CLI.hpp>

#include <atomic>
#include <csignal>
#include <iostream>
#include <iomanip>
#include <filesystem>

namespace ytdlpspout {

// =============================================================================
// グローバルシグナルハンドラ用
// =============================================================================

static std::atomic<bool> g_stopRequested{ false };
static VideoPlayer* g_currentPlayer = nullptr;

// シグナルハンドラ - スレッドセーフのためフラグ設定のみ行う
// 注意: std::coutなどの非同期シグナル安全でない関数は使用しない
static void SignalHandler(int signal) {
    if (signal == SIGINT || signal == SIGTERM) {
        g_stopRequested.store(true, std::memory_order_release);
        if (g_currentPlayer) {
            g_currentPlayer->RequestStop();
        }
    }
}

// =============================================================================
// 内部実装クラス
// =============================================================================

/// @brief アプリケーション設定（内部用拡張）
struct AppConfigInternal : public AppConfig {
    // 解決後のパス（yt-dlpでストリームURLに変換後）
    std::string resolvedPath;
};

struct Application::Impl {
    AppConfigInternal config;
    std::unique_ptr<VideoPlayer> player;
    std::atomic<bool> stopRequested{ false };

    // 進捗表示
    double lastProgressTime = -1.0;
    
    // yt-dlp resolver
    std::unique_ptr<ytdlp::YtDlpResolver> ytdlpResolver;
};

// =============================================================================
// コンストラクタ・デストラクタ
// =============================================================================

Application::Application() : m_impl(std::make_unique<Impl>()) {
    m_impl->ytdlpResolver = std::make_unique<ytdlp::YtDlpResolver>();
}

Application::~Application() {
    Stop();
}

// =============================================================================
// 引数解析
// =============================================================================

bool Application::ParseArguments(int argc, char* argv[]) {
    CLI::App app{"ytdlpSpout C++ Backend - Video to Spout streaming"};

    // 位置引数: 入力ファイルまたはURL
    app.add_option("input", m_impl->config.inputFile, "Input video file path or URL")
        ->required();
    // 注意: URLの場合はExistingFileチェックを外す

    // オプション引数
    app.add_option("-n,--name", m_impl->config.senderName, 
                   "Spout sender name (default: ytdlpSpout)")
        ->default_val("ytdlpSpout");

    app.add_flag("-l,--loop", m_impl->config.loop,
                 "Loop playback");

    app.add_flag("-v,--verbose", m_impl->config.verbose,
                 "Enable verbose logging");

    app.add_flag("--no-hwaccel", [this](int64_t count) {
        if (count > 0) {
            m_impl->config.useHardwareAccel = false;
        }
    }, "Disable hardware acceleration");

    app.add_flag("--no-progress", [this](int64_t count) {
        if (count > 0) {
            m_impl->config.showProgress = false;
        }
    }, "Disable progress display");

    // yt-dlp関連オプション
    app.add_option("--ytdlp-path", m_impl->config.ytdlpPath,
                   "Path to yt-dlp executable (default: yt-dlp)")
        ->default_val("yt-dlp");

    app.add_option("-f,--format", m_impl->config.format,
                   "Format selection (e.g., 'best', '1080p', '720p')")
        ->default_val("best");

    app.add_option("--height", m_impl->config.preferredHeight,
                   "Preferred video height in pixels (default: 1080)")
        ->default_val(1080);

    // ビートマップ関連オプション
    app.add_flag("--analyze-bpm", m_impl->config.analyzeBpm,
                 "Analyze BPM before playback");

    app.add_option("--load-beatmap", m_impl->config.loadBeatmapPath,
                   "Load beatmap from file");

    app.add_option("--save-beatmap", m_impl->config.saveBeatmapPath,
                   "Save analyzed beatmap to file");

    // ヘルプ・バージョン
    app.set_help_flag("-h,--help", "Show this help message");
    app.set_version_flag("--version", "ytdlpSpout C++ Backend v0.1.0");

    try {
        app.parse(argc, argv);
    } catch (const CLI::ParseError& e) {
        app.exit(e);
        return false;
    }

    // フォーマット文字列から高さを解析
    // RES-5: --format ""（空文字）が渡された場合、fmt.back()はUBを引き起こすため
    // 空文字列は"best"相当として扱いスキップする
    if (m_impl->config.format != "best" && !m_impl->config.format.empty()) {
        // "1080p" -> 1080
        std::string fmt = m_impl->config.format;
        if (fmt.back() == 'p' || fmt.back() == 'P') {
            fmt.pop_back();
        }
        try {
            int height = std::stoi(fmt);
            if (height > 0) {
                m_impl->config.preferredHeight = height;
            }
        } catch (...) {
            // 変換失敗は無視
        }
    }

    return true;
}

// =============================================================================
// 実行
// =============================================================================

int Application::Run() {
    // ロガー初期化
    LogLevel logLevel = m_impl->config.verbose ? LogLevel::Debug : LogLevel::Info;
    Logger::Initialize(false, "", logLevel);

    LOG_INFO("ytdlpSpout C++ Backend starting...");
    LOG_INFO("Input: {}", m_impl->config.inputFile);
    LOG_INFO("Sender: {}", m_impl->config.senderName);
    LOG_INFO("Loop: {}", m_impl->config.loop ? "yes" : "no");
    LOG_INFO("Hardware Accel: {}", m_impl->config.useHardwareAccel ? "yes" : "no");

    // RES-6: シグナルハンドラ設定は、ブロッキングするURL解決（yt-dlp呼び出し、最大30秒×2回）
    // より前に登録する。ハンドラはg_currentPlayerのnullチェックを行うため、
    // プレイヤー生成前のこの時点で登録しても安全。これにより解決中のCtrl-Cで
    // 子プロセス（yt-dlp）が回収されずリークすることを防ぐ。
    std::signal(SIGINT, SignalHandler);
    std::signal(SIGTERM, SignalHandler);

    // =========================================================================
    // URLタイプに応じた処理分岐
    // =========================================================================

    auto sourceType = ytdlp::YtDlpResolver::GetSourceType(m_impl->config.inputFile);
    
    switch (sourceType) {
        case ytdlp::SourceType::LocalFile: {
            // ローカルファイル: 直接再生
            if (!std::filesystem::exists(m_impl->config.inputFile)) {
                LOG_ERROR("File not found: {}", m_impl->config.inputFile);
                return 1;
            }
            m_impl->config.resolvedPath = m_impl->config.inputFile;
            LOG_INFO("Source type: Local file");
            break;
        }
        
        case ytdlp::SourceType::HttpUrl: {
            // 直接HTTP URL: CustomIOContext経由で再生
            m_impl->config.resolvedPath = m_impl->config.inputFile;
            LOG_INFO("Source type: HTTP URL (direct)");
            break;
        }
        
        case ytdlp::SourceType::YtDlpUrl: {
            // yt-dlp対応URL: ストリームURL取得
            LOG_INFO("Source type: yt-dlp supported URL");
            LOG_INFO("Resolving stream URL with yt-dlp...");
            
            // yt-dlpパス設定
            m_impl->ytdlpResolver->SetYtDlpPath(m_impl->config.ytdlpPath);
            
            // ストリームURL取得
            auto streamUrl = m_impl->ytdlpResolver->GetStreamUrl(
                m_impl->config.inputFile, 
                m_impl->config.preferredHeight
            );
            
            if (!streamUrl) {
                LOG_ERROR("Failed to resolve stream URL for: {}", m_impl->config.inputFile);
                return 1;
            }
            
            m_impl->config.resolvedPath = *streamUrl;
            LOG_INFO("Stream URL resolved successfully");
            LOG_DEBUG("Resolved URL: {}", m_impl->config.resolvedPath);
            break;
        }
        
        default: {
            LOG_ERROR("Unknown source type for: {}", m_impl->config.inputFile);
            return 1;
        }
    }

    // プレイヤー作成
    m_impl->player = std::make_unique<VideoPlayer>();
    g_currentPlayer = m_impl->player.get();

    // コールバック設定
    if (m_impl->config.showProgress) {
        m_impl->player->SetProgressCallback([this](double current, double duration) {
            // 1秒ごとに進捗表示
            if (current - m_impl->lastProgressTime >= 1.0 || m_impl->lastProgressTime < 0) {
                m_impl->lastProgressTime = current;
                
                int curMin = static_cast<int>(current) / 60;
                int curSec = static_cast<int>(current) % 60;
                int durMin = static_cast<int>(duration) / 60;
                int durSec = static_cast<int>(duration) % 60;
                
                std::cout << "\rPlayback: " 
                          << std::setfill('0') << std::setw(2) << curMin << ":"
                          << std::setfill('0') << std::setw(2) << curSec << " / "
                          << std::setfill('0') << std::setw(2) << durMin << ":"
                          << std::setfill('0') << std::setw(2) << durSec
                          << " (" << std::fixed << std::setprecision(1) 
                          << (current / duration * 100.0) << "%)"
                          << "    " << std::flush;
            }
        });
    }

    m_impl->player->SetErrorCallback([](const std::string& error) {
        LOG_ERROR("Player error: {}", error);
    });

    m_impl->player->SetCompletionCallback([]() {
        std::cout << std::endl;
        LOG_INFO("Playback completed");
    });

    // =========================================================================
    // ビートマップ処理
    // =========================================================================
    std::shared_ptr<BeatMap> beatMap;
    
    // ビートマップファイルの読み込み
    if (!m_impl->config.loadBeatmapPath.empty()) {
        LOG_INFO("Loading beatmap from: {}", m_impl->config.loadBeatmapPath);
        auto loadedMap = BeatMap::LoadFromFile(m_impl->config.loadBeatmapPath);
        if (loadedMap) {
            beatMap = std::make_shared<BeatMap>(std::move(*loadedMap));
            LOG_INFO("Beatmap loaded: BPM={:.1f}, {} beats", 
                     beatMap->bpm, beatMap->beats.size());
        } else {
            LOG_WARN("Failed to load beatmap, continuing without it");
        }
    }
    
    // BPM解析（ビートマップが読み込まれていない場合のみ）
    if (m_impl->config.analyzeBpm && !beatMap) {
        LOG_INFO("Analyzing BPM from: {}", m_impl->config.resolvedPath);
        
        BeatMapGenerator generator;
        try {
            auto generatedMap = generator.Generate(m_impl->config.resolvedPath,
                [](double progress) {
                    std::cout << "\rBPM Analysis: " << std::fixed << std::setprecision(1) 
                              << (progress * 100.0) << "%    " << std::flush;
                });
            std::cout << std::endl;
            
            if (!generatedMap.beats.empty()) {
                beatMap = std::make_shared<BeatMap>(std::move(generatedMap));
                LOG_INFO("BPM analysis complete: BPM={:.1f}, {} beats", 
                         beatMap->bpm, beatMap->beats.size());
                
                // ビートマップの保存
                if (!m_impl->config.saveBeatmapPath.empty()) {
                    if (beatMap->SaveToFile(m_impl->config.saveBeatmapPath)) {
                        LOG_INFO("Beatmap saved to: {}", m_impl->config.saveBeatmapPath);
                    } else {
                        LOG_WARN("Failed to save beatmap to: {}", m_impl->config.saveBeatmapPath);
                    }
                }
            } else {
                LOG_WARN("BPM analysis produced no beats");
            }
        } catch (const std::exception& e) {
            LOG_WARN("BPM analysis failed: {}", e.what());
        }
    }
    
    // ビートマップをプレイヤーに設定
    if (beatMap) {
        m_impl->player->SetBeatMap(beatMap);
        
        // ビートイベントのログ出力コールバックを設定
        m_impl->player->SetBeatCallback([](const BeatInfo& beat) {
            LOG_DEBUG("♪ Beat #{}: {:.3f}s (confidence: {:.2f}{})",
                      beat.beatNumber, beat.timestamp, beat.confidence,
                      beat.isDownbeat ? ", DOWNBEAT" : "");
        });
    }

    // 再生設定（解決後のパスを使用）
    PlayerConfig playerConfig;
    playerConfig.filePath = m_impl->config.resolvedPath;
    playerConfig.senderName = m_impl->config.senderName;
    playerConfig.loop = m_impl->config.loop;
    playerConfig.useHardwareAccel = m_impl->config.useHardwareAccel;
    playerConfig.verbose = m_impl->config.verbose;

    // 再生開始
    if (!m_impl->player->Start(playerConfig)) {
        LOG_ERROR("Failed to start playback");
        return 1;
    }

    LOG_INFO("Playback started. Press Ctrl+C to stop.");

    // 再生ループを実行
    int result = m_impl->player->RunLoop();
    
    // 停止リクエストがあった場合のチェック
    while (!result && g_stopRequested) {
        break;
    }

    // クリーンアップ
    std::cout << std::endl;
    g_currentPlayer = nullptr;
    m_impl->player->Stop();
    m_impl->player.reset();

    LOG_INFO("ytdlpSpout C++ Backend stopped");
    Logger::Shutdown();

    return result;
}

const AppConfig& Application::GetConfig() const {
    return m_impl->config;
}

// =============================================================================
// 制御
// =============================================================================

void Application::Stop() {
    m_impl->stopRequested = true;
    if (m_impl->player) {
        m_impl->player->Stop();
    }
}

bool Application::IsStopRequested() const {
    return m_impl->stopRequested || g_stopRequested;
}

} // namespace ytdlpspout
