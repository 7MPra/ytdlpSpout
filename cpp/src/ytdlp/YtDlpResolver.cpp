// =============================================================================
// YtDlpResolver.cpp - yt-dlp連携モジュール 実装
// =============================================================================

#include "YtDlpResolver.h"
#include "utils/Logger.h"

#include <nlohmann/json.hpp>

#include <Windows.h>
#include <algorithm>
#include <chrono>
#include <regex>
#include <sstream>

using json = nlohmann::json;

namespace ytdlpspout {
namespace ytdlp {

// =============================================================================
// コンストラクタ・デストラクタ
// =============================================================================

YtDlpResolver::YtDlpResolver() = default;
YtDlpResolver::~YtDlpResolver() = default;

// =============================================================================
// 設定
// =============================================================================

void YtDlpResolver::SetYtDlpPath(const std::string& path) {
    if (path.empty()) {
        m_ytdlpPath = "yt-dlp";
    } else {
        m_ytdlpPath = path;
    }
}

const std::string& YtDlpResolver::GetYtDlpPath() const {
    return m_ytdlpPath;
}

void YtDlpResolver::SetTimeout(int timeoutMs) {
    m_timeoutMs = timeoutMs;
}

// =============================================================================
// URL解決
// =============================================================================

std::optional<VideoMetadata> YtDlpResolver::ResolveUrl(const std::string& url) {
    LOG_INFO("Resolving URL with yt-dlp: {}", url);

    std::vector<std::string> args = {
        "-j",           // JSON出力
        "--no-download", // ダウンロードしない
        "--no-warnings", // 警告を抑制
        url
    };

    std::string output;
    if (!ExecuteYtDlp(args, output)) {
        LOG_ERROR("yt-dlp execution failed");
        return std::nullopt;
    }

    return ParseMetadataJson(output);
}

std::optional<std::string> YtDlpResolver::GetStreamUrl(const std::string& url, int preferredHeight) {
    LOG_INFO("Getting stream URL for: {} (preferred height: {})", url, preferredHeight);

    // まずメタデータを取得してベストフォーマットを選択
    auto metadata = ResolveUrl(url);
    if (!metadata) {
        return std::nullopt;
    }

    auto format = SelectBestFormat(*metadata, preferredHeight);
    if (!format) {
        LOG_ERROR("No suitable format found");
        return std::nullopt;
    }

    // 選択されたフォーマットのURLが既にあればそれを返す
    if (!format->url.empty()) {
        LOG_INFO("Using format {} ({}x{}, {})", format->formatId, format->width, format->height, format->vcodec);
        return format->url;
    }

    // URLがない場合は yt-dlp -g で取得
    LOG_INFO("Getting direct URL for format: {}", format->formatId);

    std::vector<std::string> args = {
        "-f", format->formatId,
        "-g",           // URL出力のみ
        "--no-warnings",
        url
    };

    std::string output;
    if (!ExecuteYtDlp(args, output)) {
        LOG_ERROR("Failed to get stream URL");
        return std::nullopt;
    }

    // 末尾の改行を削除
    while (!output.empty() && (output.back() == '\n' || output.back() == '\r')) {
        output.pop_back();
    }

    if (output.empty()) {
        LOG_ERROR("Empty stream URL returned");
        return std::nullopt;
    }

    LOG_INFO("Stream URL obtained successfully");
    return output;
}

// =============================================================================
// フォーマット選択
// =============================================================================

std::optional<FormatInfo> YtDlpResolver::SelectBestFormat(const VideoMetadata& metadata, int preferredHeight) {
    if (metadata.formats.empty()) {
        return std::nullopt;
    }

    // ビデオトラックを持つフォーマットのみフィルタ
    std::vector<const FormatInfo*> videoFormats;
    for (const auto& fmt : metadata.formats) {
        if (fmt.height > 0 && (fmt.hasVideo || fmt.vcodec != "none")) {
            videoFormats.push_back(&fmt);
        }
    }

    if (videoFormats.empty()) {
        // ビデオフォーマットがない場合は最初のフォーマットを返す
        return metadata.formats[0];
    }

    // preferredHeight以下で最大の高さを持つフォーマットを探す
    const FormatInfo* bestFormat = nullptr;
    int bestScore = -1;

    for (const auto* fmt : videoFormats) {
        if (fmt->height > preferredHeight) {
            continue; // 希望より大きいものはスキップ
        }

        // スコア計算: 高さ優先、h264/hevc優先
        int score = fmt->height * 100;  // 基本スコア

        // コーデック優先度
        if (IsHwDecodableCodec(fmt->vcodec)) {
            score += 50;  // h264/hevcボーナス
        }

        // 音声付きを優先
        if (fmt->hasAudio || fmt->acodec != "none") {
            score += 10;
        }

        if (score > bestScore) {
            bestScore = score;
            bestFormat = fmt;
        }
    }

    // preferredHeight以下が見つからない場合は最も小さいものを返す
    if (!bestFormat) {
        const FormatInfo* smallest = nullptr;
        for (const auto* fmt : videoFormats) {
            if (!smallest || fmt->height < smallest->height) {
                smallest = fmt;
            }
        }
        bestFormat = smallest;
    }

    if (bestFormat) {
        return *bestFormat;
    }

    return std::nullopt;
}

// =============================================================================
// JSON解析
// =============================================================================

std::optional<VideoMetadata> YtDlpResolver::ParseMetadataJson(const std::string& jsonStr) {
    if (jsonStr.empty()) {
        return std::nullopt;
    }

    try {
        json j = json::parse(jsonStr);

        // 必須フィールドチェック
        if (!j.contains("id")) {
            LOG_ERROR("JSON missing required field: id");
            return std::nullopt;
        }

        VideoMetadata metadata;
        metadata.id = j.value("id", "");
        metadata.title = j.value("title", "");
        metadata.uploader = j.value("uploader", "");
        metadata.isLive = j.value("is_live", false);
        metadata.thumbnailUrl = j.value("thumbnail", "");

        // durationはnullの場合がある（ライブ配信など）
        if (j.contains("duration") && !j["duration"].is_null()) {
            metadata.duration = j["duration"].get<double>();
        }

        // フォーマット解析
        if (j.contains("formats") && j["formats"].is_array()) {
            for (const auto& fmtJson : j["formats"]) {
                FormatInfo fmt;
                fmt.formatId = fmtJson.value("format_id", "");
                fmt.url = fmtJson.value("url", "");
                fmt.ext = fmtJson.value("ext", "");

                // 数値フィールド（nullの場合がある）
                if (fmtJson.contains("width") && !fmtJson["width"].is_null()) {
                    fmt.width = fmtJson["width"].get<int>();
                }
                if (fmtJson.contains("height") && !fmtJson["height"].is_null()) {
                    fmt.height = fmtJson["height"].get<int>();
                }
                if (fmtJson.contains("fps") && !fmtJson["fps"].is_null()) {
                    fmt.fps = static_cast<int>(fmtJson["fps"].get<double>());
                }
                if (fmtJson.contains("filesize") && !fmtJson["filesize"].is_null()) {
                    fmt.filesize = fmtJson["filesize"].get<int64_t>();
                }
                if (fmtJson.contains("tbr") && !fmtJson["tbr"].is_null()) {
                    fmt.tbr = static_cast<int>(fmtJson["tbr"].get<double>());
                }

                // コーデック
                fmt.vcodec = fmtJson.value("vcodec", "none");
                fmt.acodec = fmtJson.value("acodec", "none");

                // ビデオ/オーディオ判定
                fmt.hasVideo = (fmt.vcodec != "none" && !fmt.vcodec.empty());
                fmt.hasAudio = (fmt.acodec != "none" && !fmt.acodec.empty());

                metadata.formats.push_back(std::move(fmt));
            }
        }

        return metadata;

    } catch (const json::parse_error& e) {
        LOG_ERROR("JSON parse error: {}", e.what());
        return std::nullopt;
    } catch (const json::type_error& e) {
        LOG_ERROR("JSON type error: {}", e.what());
        return std::nullopt;
    }
}

// =============================================================================
// 静的ユーティリティ
// =============================================================================

bool YtDlpResolver::IsSupportedUrl(const std::string& url) {
    if (url.empty()) {
        return false;
    }

    // サポートするサイトのパターン
    static const std::vector<std::regex> patterns = {
        // YouTube
        std::regex(R"(https?://(www\.)?youtube\.com/)", std::regex::icase),
        std::regex(R"(https?://youtu\.be/)", std::regex::icase),
        std::regex(R"(https?://music\.youtube\.com/)", std::regex::icase),
        
        // Twitch
        std::regex(R"(https?://(www\.)?twitch\.tv/)", std::regex::icase),
        
        // Vimeo
        std::regex(R"(https?://(www\.)?(player\.)?vimeo\.com/)", std::regex::icase),
        
        // NicoNico
        std::regex(R"(https?://(www\.)?nicovideo\.jp/)", std::regex::icase),
        std::regex(R"(https?://nico\.ms/)", std::regex::icase),
        
        // Twitter/X
        std::regex(R"(https?://(www\.)?(twitter|x)\.com/)", std::regex::icase),
        
        // Dailymotion
        std::regex(R"(https?://(www\.)?dailymotion\.com/)", std::regex::icase),
        
        // Bilibili
        std::regex(R"(https?://(www\.)?bilibili\.com/)", std::regex::icase),
    };

    for (const auto& pattern : patterns) {
        if (std::regex_search(url, pattern)) {
            return true;
        }
    }

    return false;
}

SourceType YtDlpResolver::GetSourceType(const std::string& pathOrUrl) {
    if (pathOrUrl.empty()) {
        return SourceType::Unknown;
    }

    // yt-dlp対応URLかチェック
    if (IsSupportedUrl(pathOrUrl)) {
        return SourceType::YtDlpUrl;
    }

    // HTTP/HTTPS URLかチェック
    if (pathOrUrl.substr(0, 7) == "http://" || pathOrUrl.substr(0, 8) == "https://") {
        return SourceType::HttpUrl;
    }

    // ローカルファイルパス判定
    // Windowsパス: C:\..., D:/...
    // Unixパス: /...
    if ((pathOrUrl.length() >= 2 && pathOrUrl[1] == ':') ||  // Windows drive letter
        (pathOrUrl.length() >= 3 && std::isalpha(pathOrUrl[0]) && pathOrUrl[1] == ':') ||
        (pathOrUrl[0] == '/') ||
        (pathOrUrl[0] == '\\')) {
        return SourceType::LocalFile;
    }

    // 相対パスもローカルファイルとして扱う
    return SourceType::LocalFile;
}

bool YtDlpResolver::IsHwDecodableCodec(const std::string& vcodec) {
    if (vcodec.empty() || vcodec == "none") {
        return false;
    }

    std::string lower = vcodec;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

    // h264/AVC
    if (lower.find("avc") != std::string::npos ||
        lower.find("h264") != std::string::npos ||
        lower.find("h.264") != std::string::npos) {
        return true;
    }

    // h265/HEVC
    if (lower.find("hevc") != std::string::npos ||
        lower.find("h265") != std::string::npos ||
        lower.find("h.265") != std::string::npos) {
        return true;
    }

    return false;
}

// =============================================================================
// プロセス実行
// =============================================================================

bool YtDlpResolver::ExecuteYtDlp(const std::vector<std::string>& args, std::string& output) {
    // コマンドライン構築
    std::string cmdLine = "\"" + m_ytdlpPath + "\"";
    for (const auto& arg : args) {
        cmdLine += " ";
        // 引数にスペースや特殊文字が含まれる場合はクォート
        if (arg.find(' ') != std::string::npos || arg.find('&') != std::string::npos) {
            cmdLine += "\"" + arg + "\"";
        } else {
            cmdLine += arg;
        }
    }

    LOG_DEBUG("Executing: {}", cmdLine);

    // パイプ作成
    SECURITY_ATTRIBUTES saAttr = {};
    saAttr.nLength = sizeof(SECURITY_ATTRIBUTES);
    saAttr.bInheritHandle = TRUE;
    saAttr.lpSecurityDescriptor = nullptr;

    HANDLE hStdoutRead = nullptr;
    HANDLE hStdoutWrite = nullptr;
    HANDLE hStderrRead = nullptr;
    HANDLE hStderrWrite = nullptr;

    if (!CreatePipe(&hStdoutRead, &hStdoutWrite, &saAttr, 0)) {
        LOG_ERROR("Failed to create stdout pipe");
        return false;
    }

    if (!CreatePipe(&hStderrRead, &hStderrWrite, &saAttr, 0)) {
        CloseHandle(hStdoutRead);
        CloseHandle(hStdoutWrite);
        LOG_ERROR("Failed to create stderr pipe");
        return false;
    }

    // 親プロセス側のハンドルを継承不可に
    SetHandleInformation(hStdoutRead, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(hStderrRead, HANDLE_FLAG_INHERIT, 0);

    // プロセス起動
    STARTUPINFOA si = {};
    si.cb = sizeof(STARTUPINFOA);
    si.hStdOutput = hStdoutWrite;
    si.hStdError = hStderrWrite;
    si.dwFlags = STARTF_USESTDHANDLES;

    PROCESS_INFORMATION pi = {};

    std::vector<char> cmdLineBuf(cmdLine.begin(), cmdLine.end());
    cmdLineBuf.push_back('\0');

    BOOL success = CreateProcessA(
        nullptr,
        cmdLineBuf.data(),
        nullptr,
        nullptr,
        TRUE,
        CREATE_NO_WINDOW,
        nullptr,
        nullptr,
        &si,
        &pi
    );

    // 書き込み側ハンドルを閉じる（子プロセスにのみ必要）
    CloseHandle(hStdoutWrite);
    CloseHandle(hStderrWrite);

    if (!success) {
        DWORD error = GetLastError();
        LOG_ERROR("Failed to create process: error {}", error);
        CloseHandle(hStdoutRead);
        CloseHandle(hStderrRead);
        return false;
    }

    // 出力を読み取り（プロセス実行中にパイプを読む - デッドロック回避）
    output.clear();
    std::string stderrOutput;
    char buffer[4096];
    DWORD bytesRead;
    auto startTime = std::chrono::steady_clock::now();

    // 非ブロッキングで読み取りながらプロセス終了を待つ
    while (true) {
        // タイムアウトチェック
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - startTime).count();
        if (elapsed > m_timeoutMs) {
            LOG_ERROR("yt-dlp process timed out after {}ms", elapsed);
            TerminateProcess(pi.hProcess, 1);
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
            CloseHandle(hStdoutRead);
            CloseHandle(hStderrRead);
            return false;
        }

        // プロセス終了チェック
        DWORD waitResult = WaitForSingleObject(pi.hProcess, 0);
        bool processEnded = (waitResult == WAIT_OBJECT_0);

        // stdoutから読み取り（PeekNamedPipeで非ブロッキングチェック）
        DWORD available = 0;
        while (PeekNamedPipe(hStdoutRead, nullptr, 0, nullptr, &available, nullptr) && available > 0) {
            if (ReadFile(hStdoutRead, buffer, sizeof(buffer) - 1, &bytesRead, nullptr) && bytesRead > 0) {
                buffer[bytesRead] = '\0';
                output += buffer;
            }
        }

        // stderrから読み取り
        available = 0;
        while (PeekNamedPipe(hStderrRead, nullptr, 0, nullptr, &available, nullptr) && available > 0) {
            if (ReadFile(hStderrRead, buffer, sizeof(buffer) - 1, &bytesRead, nullptr) && bytesRead > 0) {
                buffer[bytesRead] = '\0';
                stderrOutput += buffer;
            }
        }

        if (processEnded) {
            break;
        }

        // CPU負荷軽減のため少し待機
        Sleep(10);
    }

    // 残りの出力を読み取り
    while (ReadFile(hStdoutRead, buffer, sizeof(buffer) - 1, &bytesRead, nullptr) && bytesRead > 0) {
        buffer[bytesRead] = '\0';
        output += buffer;
    }
    while (ReadFile(hStderrRead, buffer, sizeof(buffer) - 1, &bytesRead, nullptr) && bytesRead > 0) {
        buffer[bytesRead] = '\0';
        stderrOutput += buffer;
    }

    // 終了コード取得
    DWORD exitCode = 0;
    GetExitCodeProcess(pi.hProcess, &exitCode);

    // クリーンアップ
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    CloseHandle(hStdoutRead);
    CloseHandle(hStderrRead);

    if (exitCode != 0) {
        LOG_ERROR("yt-dlp exited with code {}", exitCode);
        if (!stderrOutput.empty()) {
            LOG_ERROR("yt-dlp stderr: {}", stderrOutput);
        }
        return false;
    }

    return true;
}

} // namespace ytdlp
} // namespace ytdlpspout
