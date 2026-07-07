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

namespace {

// nlohmann::json の j.value(key, default) は、キーが存在しても値が null の場合に
// type_error(302) を送出してしまう（is_null()のガードが必要）。
// yt-dlpはuploader/thumbnail/duration等をしばしばnullで返すため、
// 以下のヘルパーでnull安全・型不一致安全にフィールドを取得する。

std::string JsonStringOr(const json& j, const std::string& key, const std::string& def) {
    if (!j.contains(key) || j.at(key).is_null()) {
        return def;
    }
    try {
        return j.at(key).get<std::string>();
    } catch (const json::type_error&) {
        return def;
    }
}

bool JsonBoolOr(const json& j, const std::string& key, bool def) {
    if (!j.contains(key) || j.at(key).is_null()) {
        return def;
    }
    try {
        return j.at(key).get<bool>();
    } catch (const json::type_error&) {
        return def;
    }
}

double JsonNumberOr(const json& j, const std::string& key, double def) {
    if (!j.contains(key) || j.at(key).is_null()) {
        return def;
    }
    try {
        return j.at(key).get<double>();
    } catch (const json::type_error&) {
        return def;
    }
}

} // namespace

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
        metadata.id = JsonStringOr(j, "id", "");
        metadata.title = JsonStringOr(j, "title", "");
        metadata.uploader = JsonStringOr(j, "uploader", "");
        metadata.isLive = JsonBoolOr(j, "is_live", false);
        metadata.thumbnailUrl = JsonStringOr(j, "thumbnail", "");

        // durationはnullの場合がある（ライブ配信など）
        metadata.duration = JsonNumberOr(j, "duration", 0.0);

        // フォーマット解析
        if (j.contains("formats") && j["formats"].is_array()) {
            for (const auto& fmtJson : j["formats"]) {
                FormatInfo fmt;
                fmt.formatId = JsonStringOr(fmtJson, "format_id", "");
                fmt.url = JsonStringOr(fmtJson, "url", "");
                fmt.ext = JsonStringOr(fmtJson, "ext", "");

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
                fmt.vcodec = JsonStringOr(fmtJson, "vcodec", "none");
                fmt.acodec = JsonStringOr(fmtJson, "acodec", "none");

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

namespace {

// 直接再生可能なファイル拡張子（python/ytdlp_resolver.py の DIRECT_EXTENSIONS と同期）
const std::vector<std::string>& DirectMediaExtensions() {
    static const std::vector<std::string> exts = {
        ".mp4", ".webm", ".mkv", ".avi", ".mov", ".flv", ".wmv",
        ".m3u8", ".mpd", ".ts",
        ".mp3", ".ogg", ".wav", ".flac", ".m4a",
    };
    return exts;
}

// http(s) URLのパス部分（クエリ・フラグメント除去、小文字化）を取り出す
// ※ 呼び出し前にhttp(s)スキームであることを確認しておくこと
std::string ExtractUrlPathLower(const std::string& url) {
    size_t schemeEnd = url.find("://");
    size_t pathStart = (schemeEnd == std::string::npos) ? 0 : schemeEnd + 3;
    size_t slashPos = url.find('/', pathStart);
    std::string path = (slashPos == std::string::npos) ? std::string() : url.substr(slashPos);
    size_t queryPos = path.find_first_of("?#");
    if (queryPos != std::string::npos) {
        path = path.substr(0, queryPos);
    }
    std::transform(path.begin(), path.end(), path.begin(), ::tolower);
    return path;
}

// URLのパス末尾が既知の直接メディア拡張子かどうか
bool HasDirectMediaExtension(const std::string& url) {
    std::string path = ExtractUrlPathLower(url);
    for (const auto& ext : DirectMediaExtensions()) {
        if (path.size() >= ext.size() &&
            path.compare(path.size() - ext.size(), ext.size(), ext) == 0) {
            return true;
        }
    }
    return false;
}

// http(s) URLのホスト部分（ユーザー情報・ポート除去、小文字化、www.除去）を取り出す
// ※ 呼び出し前にhttp(s)スキームであることを確認しておくこと
std::string ExtractUrlHostLower(const std::string& url) {
    size_t schemeEnd = url.find("://");
    size_t hostStart = (schemeEnd == std::string::npos) ? 0 : schemeEnd + 3;
    size_t hostEnd = url.find_first_of("/?#", hostStart);
    std::string host = (hostEnd == std::string::npos) ? url.substr(hostStart) : url.substr(hostStart, hostEnd - hostStart);

    // userinfo除去 (user:pass@host)
    size_t atPos = host.find('@');
    if (atPos != std::string::npos) {
        host = host.substr(atPos + 1);
    }
    // ポート除去
    size_t colonPos = host.find(':');
    if (colonPos != std::string::npos) {
        host = host.substr(0, colonPos);
    }

    std::transform(host.begin(), host.end(), host.begin(), ::tolower);
    if (host.compare(0, 4, "www.") == 0) {
        host = host.substr(4);
    }
    return host;
}

// 既知のyt-dlp対応ドメイン一覧
// ※ python/ytdlp_resolver.py の YtDlpAsyncResolver.YTDLP_DOMAINS と必ず同期させること
//    （Python側を正とする）
const std::vector<std::string>& KnownYtdlpDomains() {
    static const std::vector<std::string> domains = {
        // 動画サイト
        "youtube.com", "youtu.be", "youtube-nocookie.com",
        "twitch.tv",
        "nicovideo.jp", "nico.ms", "live.nicovideo.jp",
        "vimeo.com",
        "dailymotion.com",
        "bilibili.com", "bilibili.tv",

        // SNS動画
        "twitter.com", "x.com",
        "instagram.com",
        "tiktok.com",
        "facebook.com", "fb.watch",

        // その他
        "soundcloud.com",
        "bandcamp.com",
        "reddit.com",
        "pornhub.com", "xvideos.com",
    };
    return domains;
}

// hostが既知のyt-dlp対応ドメイン（またはそのサブドメイン）かどうか判定
// （python/ytdlp_resolver.py の _is_known_ytdlp_domain と同じ判定ロジック）
bool IsKnownYtdlpDomain(const std::string& host) {
    if (host.empty()) {
        return false;
    }
    for (const auto& domain : KnownYtdlpDomains()) {
        if (host == domain) {
            return true;
        }
        if (host.size() > domain.size() &&
            host.compare(host.size() - domain.size(), domain.size(), domain) == 0 &&
            host[host.size() - domain.size() - 1] == '.') {
            return true;
        }
    }
    return false;
}

} // namespace

bool YtDlpResolver::IsSupportedUrl(const std::string& url) {
    if (url.empty()) {
        return false;
    }

    // http(s) URL以外（ローカルパス等）はyt-dlp解決の対象外
    static const std::regex schemePattern(R"(^https?://)", std::regex::icase);
    if (!std::regex_search(url, schemePattern)) {
        return false;
    }

    // 既知サイトのドメインは拡張子の有無に関わらずyt-dlp解決対象
    // （python/ytdlp_resolver.py の YTDLP_DOMAINS と同一内容に揃えている）
    std::string host = ExtractUrlHostLower(url);
    if (IsKnownYtdlpDomain(host)) {
        return true;
    }

    // 未知サイトでも、直接メディア拡張子でなければyt-dlp解決を試みる
    // （python/ytdlp_resolver.py の is_ytdlp_url と反転条件を揃える）
    return !HasDirectMediaExtension(url);
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
// コマンドライン引数クオート（RES-1: コマンド/引数インジェクション対策）
// =============================================================================

std::string YtDlpResolver::QuoteWinArg(const std::string& arg) {
    // MSDN「Parsing C++ Command-Line Arguments」/ CommandLineToArgvW互換規則:
    //   - 引数は常に " で囲む
    //   - " の直前に連続する \ は2倍にし、" 自体は \" にエスケープする
    //   - 閉じる " の直前に来る連続する \ も2倍にする（エスケープと解釈されないように）
    // これにより引数中のスペース/"/&/^/%等が単一の引数としてそのまま子プロセスに渡る。
    std::string result;
    result.push_back('"');

    size_t backslashes = 0;
    for (char c : arg) {
        if (c == '\\') {
            ++backslashes;
            continue;
        }
        if (c == '"') {
            // 直前の\をすべて2倍にし、"自体をエスケープする\を1つ追加
            result.append(backslashes * 2 + 1, '\\');
            backslashes = 0;
            result.push_back('"');
        } else {
            // \はここでは特殊文字ではないためそのまま出力
            result.append(backslashes, '\\');
            backslashes = 0;
            result.push_back(c);
        }
    }
    // 末尾に残った\は、後続の閉じる"のために2倍にする
    result.append(backslashes * 2, '\\');
    result.push_back('"');
    return result;
}

// =============================================================================
// プロセス実行
// =============================================================================

namespace {

// UTF-8文字列をUTF-16 (std::wstring) に変換する
// コードベースの文字列はUTF-8だが、CreateProcessWにはUTF-16が必要なため変換する
// （RES-2: CreateProcessAはANSI/システムロケール解釈のため非ASCIIパス/URLが破損する）
std::wstring Utf8ToWide(const std::string& utf8) {
    if (utf8.empty()) {
        return std::wstring();
    }
    int sizeNeeded = MultiByteToWideChar(
        CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (sizeNeeded <= 0) {
        return std::wstring();
    }
    std::wstring wide(static_cast<size_t>(sizeNeeded), L'\0');
    MultiByteToWideChar(
        CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), wide.data(), sizeNeeded);
    return wide;
}

} // namespace

bool YtDlpResolver::ExecuteYtDlp(const std::vector<std::string>& args, std::string& output) {
    // コマンドライン構築（全引数・実行ファイルパスをWindows規則で正しくクオート・エスケープ）
    std::string cmdLine = QuoteWinArg(m_ytdlpPath);
    for (const auto& arg : args) {
        cmdLine += " ";
        cmdLine += QuoteWinArg(arg);
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

    // プロセス起動（UTF-8→UTF-16変換の上でCreateProcessWを使用し、非ASCIIパス/URLの破損を防ぐ）
    STARTUPINFOW si = {};
    si.cb = sizeof(STARTUPINFOW);
    si.hStdOutput = hStdoutWrite;
    si.hStdError = hStderrWrite;
    si.dwFlags = STARTF_USESTDHANDLES;

    PROCESS_INFORMATION pi = {};

    std::wstring wCmdLine = Utf8ToWide(cmdLine);
    std::vector<wchar_t> cmdLineBuf(wCmdLine.begin(), wCmdLine.end());
    cmdLineBuf.push_back(L'\0');

    BOOL success = CreateProcessW(
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
