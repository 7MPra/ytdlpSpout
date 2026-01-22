// =============================================================================
// M3U8Parser.cpp - HLS m3u8プレイリストパーサー実装
// =============================================================================

#include "hls/M3U8Parser.h"
#include "utils/Logger.h"
#include <sstream>
#include <algorithm>
#include <cctype>
#include <regex>

namespace ytdlpspout {
namespace hls {

// =============================================================================
// パース
// =============================================================================

std::optional<M3U8Playlist> M3U8Parser::Parse(
    const std::string& m3u8Content,
    const std::string& baseUrl)
{
    if (m3u8Content.empty()) {
        LOG_WARN("M3U8Parser: Empty content");
        return std::nullopt;
    }

    auto lines = SplitLines(m3u8Content);
    if (lines.empty()) {
        LOG_WARN("M3U8Parser: No lines found");
        return std::nullopt;
    }

    // 最初の行が#EXTM3Uであることを確認
    std::string firstLine = ToUpper(Trim(lines[0]));
    if (firstLine != "#EXTM3U") {
        LOG_WARN("M3U8Parser: Missing #EXTM3U tag");
        return std::nullopt;
    }

    M3U8Playlist playlist;
    double pendingDuration = 0.0;
    bool hasPendingDuration = false;
    int64_t pendingByteRangeLength = 0;
    int64_t pendingByteRangeOffset = -1;
    bool hasPendingByteRange = false;
    int64_t currentByteOffset = 0;
    int64_t segmentIndex = 0;

    for (size_t i = 1; i < lines.size(); ++i) {
        std::string line = Trim(lines[i]);
        if (line.empty()) {
            continue;
        }

        std::string upperLine = ToUpper(line);

        // #EXT-X-VERSION
        if (upperLine.find("#EXT-X-VERSION") == 0) {
            std::string value = GetTagValue(line, "#EXT-X-VERSION");
            if (!value.empty()) {
                try {
                    playlist.version = std::stoi(value);
                } catch (...) {
                    LOG_WARN("M3U8Parser: Invalid version value: {}", value);
                }
            }
            continue;
        }

        // #EXT-X-TARGETDURATION
        if (upperLine.find("#EXT-X-TARGETDURATION") == 0) {
            std::string value = GetTagValue(line, "#EXT-X-TARGETDURATION");
            if (!value.empty()) {
                try {
                    playlist.targetDuration = std::stod(value);
                } catch (...) {
                    LOG_WARN("M3U8Parser: Invalid target duration: {}", value);
                }
            }
            continue;
        }

        // #EXT-X-MEDIA-SEQUENCE
        if (upperLine.find("#EXT-X-MEDIA-SEQUENCE") == 0) {
            std::string value = GetTagValue(line, "#EXT-X-MEDIA-SEQUENCE");
            if (!value.empty()) {
                try {
                    playlist.mediaSequence = std::stoll(value);
                } catch (...) {
                    LOG_WARN("M3U8Parser: Invalid media sequence: {}", value);
                }
            }
            continue;
        }

        // #EXT-X-KEY
        if (upperLine.find("#EXT-X-KEY") == 0) {
            auto key = ParseKeyTag(line, baseUrl);
            if (key.has_value()) {
                if (key->method != "NONE") {
                    playlist.encryptionKey = key;
                } else {
                    // METHOD=NONEは暗号化なしを意味する
                    playlist.encryptionKey = std::nullopt;
                }
            }
            continue;
        }

        // #EXT-X-MAP
        if (upperLine.find("#EXT-X-MAP") == 0) {
            static const std::regex uriRegex("URI=\"([^\"]+)\"", std::regex::icase);
            static const std::regex byteRangeRegex("BYTERANGE=\"([^\"]+)\"", std::regex::icase);
            
            HlsMap mapInfo;
            bool hasUri = false;
            
            // URI属性を取得
            std::smatch uriMatch;
            if (std::regex_search(line, uriMatch, uriRegex)) {
                mapInfo.url = ResolveUrl(baseUrl, Trim(uriMatch[1].str()));
                hasUri = true;
            }
            
            // BYTERANGE属性を取得
            std::smatch byteRangeMatch;
            if (std::regex_search(line, byteRangeMatch, byteRangeRegex)) {
                int64_t length = 0;
                int64_t offset = -1;
                if (ParseByteRangeTag(byteRangeMatch[1].str(), length, offset)) {
                    mapInfo.byteRangeLength = length;
                    mapInfo.byteRangeStart = offset;
                }
            }
            
            if (hasUri) {
                playlist.map = mapInfo;
                LOG_DEBUG("M3U8Parser: Found Initialization Segment: {}", mapInfo.url);
            }
            continue;
        }

        // #EXT-X-BYTERANGE
        if (upperLine.find("#EXT-X-BYTERANGE") == 0) {
            std::string value = GetTagValue(line, "#EXT-X-BYTERANGE");
            int64_t length = 0;
            int64_t offset = -1;
            if (ParseByteRangeTag(value, length, offset)) {
                pendingByteRangeLength = length;
                if (offset >= 0) {
                    pendingByteRangeOffset = offset;
                    currentByteOffset = offset;
                } else {
                    // オフセットが省略された場合は前のセグメントの終端から
                    pendingByteRangeOffset = currentByteOffset;
                }
                hasPendingByteRange = true;
            }
            continue;
        }

        // #EXTINF
        if (upperLine.find("#EXTINF") == 0) {
            std::string value = GetTagValue(line, "#EXTINF");
            // カンマの前の数値部分のみを取得
            size_t commaPos = value.find(',');
            if (commaPos != std::string::npos) {
                value = value.substr(0, commaPos);
            }
            if (!value.empty()) {
                try {
                    pendingDuration = std::stod(value);
                    hasPendingDuration = true;
                } catch (...) {
                    LOG_WARN("M3U8Parser: Invalid EXTINF duration: {}", value);
                }
            }
            continue;
        }

        // #EXT-X-ENDLIST
        if (upperLine.find("#EXT-X-ENDLIST") == 0) {
            playlist.isEndList = true;
            continue;
        }

        // コメント行（#で始まる未知のタグ）
        if (line[0] == '#') {
            continue;
        }

        // セグメントURL
        if (hasPendingDuration) {
            HlsSegment segment;
            segment.index = segmentIndex++;
            segment.url = ResolveUrl(baseUrl, line);
            segment.duration = pendingDuration;
            segment.mediaSequence = playlist.mediaSequence + segment.index;

            if (hasPendingByteRange) {
                segment.byteRangeStart = pendingByteRangeOffset;
                segment.byteRangeLength = pendingByteRangeLength;
                currentByteOffset = pendingByteRangeOffset + pendingByteRangeLength;
                hasPendingByteRange = false;
            }

            playlist.segments.push_back(segment);
            playlist.totalDuration += pendingDuration;
            hasPendingDuration = false;
        }
    }

    // ライブ判定: ENDLISTがなければライブ
    playlist.isLive = !playlist.isEndList;

    LOG_DEBUG("M3U8Parser: Parsed {} segments, version={}, live={}, totalDuration={:.2f}s",
              playlist.segments.size(), playlist.version, playlist.isLive, playlist.totalDuration);

    return playlist;
}

// =============================================================================
// URL解決
// =============================================================================

std::string M3U8Parser::ResolveUrl(
    const std::string& baseUrl,
    const std::string& relativeUrl)
{
    if (relativeUrl.empty()) {
        return baseUrl;
    }

    // 既に絶対URLの場合
    std::string lowerUrl = relativeUrl;
    std::transform(lowerUrl.begin(), lowerUrl.end(), lowerUrl.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    if (lowerUrl.find("http://") == 0 || lowerUrl.find("https://") == 0) {
        return relativeUrl;
    }

    // ルート相対パス（/で始まる）
    if (relativeUrl[0] == '/') {
        return GetUrlOrigin(baseUrl) + relativeUrl;
    }

    // 相対パス
    std::string basePath = GetBasePath(baseUrl);
    std::string result = basePath + relativeUrl;

    // ../の解決
    // MIN_SCHEME_LENGTH: "https://" の長さ（スキーム部分の最小長）
    constexpr size_t MIN_SCHEME_LENGTH = 8;
    while (true) {
        size_t pos = result.find("/../");
        if (pos == std::string::npos) {
            break;
        }

        // /../の前のパス部分を見つける
        size_t slashPos = result.rfind('/', pos - 1);
        if (slashPos == std::string::npos || slashPos < MIN_SCHEME_LENGTH) {
            // URLのスキーム部分に到達した場合は停止
            break;
        }

        result = result.substr(0, slashPos) + result.substr(pos + 3);
    }

    return result;
}

// =============================================================================
// 内部ヘルパー
// =============================================================================

std::vector<std::string> M3U8Parser::SplitLines(const std::string& content) {
    std::vector<std::string> lines;
    std::istringstream stream(content);
    std::string line;

    while (std::getline(stream, line)) {
        // \r\nの\rを除去
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        lines.push_back(line);
    }

    return lines;
}

std::string M3U8Parser::ToUpper(const std::string& str) {
    std::string result = str;
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char c) { return std::toupper(c); });
    return result;
}

std::string M3U8Parser::Trim(const std::string& str) {
    size_t start = 0;
    while (start < str.size() && std::isspace(static_cast<unsigned char>(str[start]))) {
        ++start;
    }
    size_t end = str.size();
    while (end > start && std::isspace(static_cast<unsigned char>(str[end - 1]))) {
        --end;
    }
    return str.substr(start, end - start);
}

std::string M3U8Parser::GetTagValue(const std::string& line, const std::string& tagName) {
    // 大文字小文字を無視してタグを検索
    std::string upperLine = ToUpper(line);
    std::string upperTag = ToUpper(tagName);
    
    size_t pos = upperLine.find(upperTag);
    if (pos == std::string::npos) {
        return "";
    }

    // コロンの後の値を取得
    size_t colonPos = line.find(':', pos + tagName.length());
    if (colonPos == std::string::npos) {
        return "";
    }

    return Trim(line.substr(colonPos + 1));
}

std::optional<HlsEncryptionKey> M3U8Parser::ParseKeyTag(
    const std::string& line,
    const std::string& baseUrl)
{
    // 正規表現を静的化してコンパイルコストを削減
    static const std::regex methodRegex("METHOD=([^,]+)", std::regex::icase);
    static const std::regex uriRegex("URI=\"([^\"]+)\"", std::regex::icase);
    static const std::regex ivRegex("IV=(0x[0-9a-fA-F]+)", std::regex::icase);
    
    HlsEncryptionKey key;

    // METHOD属性を取得
    std::smatch methodMatch;
    if (std::regex_search(line, methodMatch, methodRegex)) {
        key.method = ToUpper(Trim(methodMatch[1].str()));
    } else {
        return std::nullopt;
    }

    // METHOD=NONEの場合は早期リターン
    if (key.method == "NONE") {
        return key;
    }

    // URI属性を取得
    std::smatch uriMatch;
    if (std::regex_search(line, uriMatch, uriRegex)) {
        key.keyUrl = ResolveUrl(baseUrl, Trim(uriMatch[1].str()));
    }

    // IV属性を取得
    std::smatch ivMatch;
    if (std::regex_search(line, ivMatch, ivRegex)) {
        key.iv = ParseHexString(ivMatch[1].str());
    }

    return key;
}

bool M3U8Parser::ParseByteRangeTag(
    const std::string& value,
    int64_t& outLength,
    int64_t& outOffset)
{
    if (value.empty()) {
        return false;
    }

    size_t atPos = value.find('@');
    if (atPos != std::string::npos) {
        // length@offset形式
        try {
            outLength = std::stoll(value.substr(0, atPos));
            outOffset = std::stoll(value.substr(atPos + 1));
            return true;
        } catch (...) {
            return false;
        }
    } else {
        // lengthのみ（オフセットは-1で省略を示す）
        try {
            outLength = std::stoll(value);
            outOffset = -1;
            return true;
        } catch (...) {
            return false;
        }
    }
}

std::vector<uint8_t> M3U8Parser::ParseHexString(const std::string& hexStr) {
    std::vector<uint8_t> result;

    std::string hex = hexStr;
    // 0xプレフィックスを除去
    if (hex.size() >= 2 && hex[0] == '0' && (hex[1] == 'x' || hex[1] == 'X')) {
        hex = hex.substr(2);
    }

    // 16バイト = 32文字のhex
    if (hex.size() != 32) {
        LOG_WARN("M3U8Parser: Invalid IV length (expected 32 hex chars): {}", hex.size());
        // パディングして16バイトにする
        while (hex.size() < 32) {
            hex = "0" + hex;
        }
    }

    for (size_t i = 0; i < hex.size(); i += 2) {
        try {
            uint8_t byte = static_cast<uint8_t>(std::stoul(hex.substr(i, 2), nullptr, 16));
            result.push_back(byte);
        } catch (...) {
            result.push_back(0);
        }
    }

    return result;
}

std::string M3U8Parser::GetBasePath(const std::string& url) {
    // 最後の/までのパスを取得
    size_t lastSlash = url.rfind('/');
    if (lastSlash != std::string::npos && lastSlash > 8) {
        return url.substr(0, lastSlash + 1);
    }
    return url;
}

std::string M3U8Parser::GetUrlOrigin(const std::string& url) {
    // スキーム（http:// または https://）の後の最初の/を探す
    size_t schemeEnd = url.find("://");
    if (schemeEnd == std::string::npos) {
        return "";
    }

    size_t pathStart = url.find('/', schemeEnd + 3);
    if (pathStart == std::string::npos) {
        return url;
    }

    return url.substr(0, pathStart);
}

// =============================================================================
// マスタープレイリスト対応
// =============================================================================

bool M3U8Parser::IsMasterPlaylist(const std::string& m3u8Content) {
    // #EXT-X-STREAM-INF があればマスタープレイリスト
    return m3u8Content.find("#EXT-X-STREAM-INF") != std::string::npos;
}

std::optional<MasterPlaylist> M3U8Parser::ParseMaster(
    const std::string& m3u8Content,
    const std::string& baseUrl)
{
    if (m3u8Content.empty()) {
        LOG_WARN("M3U8Parser::ParseMaster: Empty content");
        return std::nullopt;
    }

    auto lines = SplitLines(m3u8Content);
    if (lines.empty()) {
        LOG_WARN("M3U8Parser::ParseMaster: No lines found");
        return std::nullopt;
    }

    // 最初の行が#EXTM3Uであることを確認
    std::string firstLine = ToUpper(Trim(lines[0]));
    if (firstLine != "#EXTM3U") {
        LOG_WARN("M3U8Parser::ParseMaster: Missing #EXTM3U tag");
        return std::nullopt;
    }

    MasterPlaylist master;
    HlsVariant pendingVariant;
    bool hasPendingVariant = false;

    // 正規表現パターン（staticで再利用）
    static const std::regex bandwidthRegex("BANDWIDTH=([0-9]+)", std::regex::icase);
    static const std::regex resolutionRegex("RESOLUTION=([0-9]+)x([0-9]+)", std::regex::icase);
    static const std::regex codecsRegex("CODECS=\"([^\"]+)\"", std::regex::icase);
    static const std::regex nameRegex("NAME=\"([^\"]+)\"", std::regex::icase);

    for (size_t i = 1; i < lines.size(); ++i) {
        std::string line = Trim(lines[i]);
        if (line.empty()) {
            continue;
        }

        std::string upperLine = ToUpper(line);

        // #EXT-X-STREAM-INF
        if (upperLine.find("#EXT-X-STREAM-INF") == 0) {
            pendingVariant = HlsVariant{};
            hasPendingVariant = true;

            // BANDWIDTH
            std::smatch bandwidthMatch;
            if (std::regex_search(line, bandwidthMatch, bandwidthRegex)) {
                try {
                    pendingVariant.bandwidth = std::stoll(bandwidthMatch[1].str());
                } catch (...) {
                    pendingVariant.bandwidth = 0;
                }
            }

            // RESOLUTION
            std::smatch resolutionMatch;
            if (std::regex_search(line, resolutionMatch, resolutionRegex)) {
                try {
                    pendingVariant.width = std::stoi(resolutionMatch[1].str());
                    pendingVariant.height = std::stoi(resolutionMatch[2].str());
                } catch (...) {
                    pendingVariant.width = 0;
                    pendingVariant.height = 0;
                }
            }

            // CODECS
            std::smatch codecsMatch;
            if (std::regex_search(line, codecsMatch, codecsRegex)) {
                pendingVariant.codecs = codecsMatch[1].str();
            }

            // NAME
            std::smatch nameMatch;
            if (std::regex_search(line, nameMatch, nameRegex)) {
                pendingVariant.name = nameMatch[1].str();
            }

            continue;
        }

        // コメント行（#で始まる未知のタグ）
        if (line[0] == '#') {
            continue;
        }

        // バリアントURL（#EXT-X-STREAM-INFの直後の行）
        if (hasPendingVariant) {
            pendingVariant.url = ResolveUrl(baseUrl, line);
            master.variants.push_back(pendingVariant);
            hasPendingVariant = false;
        }
    }

    LOG_DEBUG("M3U8Parser::ParseMaster: Parsed {} variants", master.variants.size());
    for (const auto& v : master.variants) {
        LOG_DEBUG("  Variant: {}x{} @ {} bps", v.width, v.height, v.bandwidth);
    }

    return master;
}

std::optional<HlsVariant> M3U8Parser::SelectBestVariant(
    const MasterPlaylist& master,
    int64_t preferredBandwidth)
{
    if (master.variants.empty()) {
        LOG_WARN("M3U8Parser::SelectBestVariant: No variants available");
        return std::nullopt;
    }

    // バリアントを帯域幅でソート（降順）
    std::vector<HlsVariant> sorted = master.variants;
    std::sort(sorted.begin(), sorted.end(),
        [](const HlsVariant& a, const HlsVariant& b) {
            return a.bandwidth > b.bandwidth;
        });

    if (preferredBandwidth <= 0) {
        // 最高帯域幅を選択
        LOG_INFO("M3U8Parser::SelectBestVariant: Selected highest bandwidth variant: {}x{} @ {} bps",
                 sorted[0].width, sorted[0].height, sorted[0].bandwidth);
        return sorted[0];
    }

    // preferredBandwidth以下で最大のものを選択
    for (const auto& v : sorted) {
        if (v.bandwidth <= preferredBandwidth) {
            LOG_INFO("M3U8Parser::SelectBestVariant: Selected variant within budget: {}x{} @ {} bps",
                     v.width, v.height, v.bandwidth);
            return v;
        }
    }

    // 全てオーバーの場合は最小のものを選択
    LOG_WARN("M3U8Parser::SelectBestVariant: All variants exceed budget, selecting lowest: {}x{} @ {} bps",
             sorted.back().width, sorted.back().height, sorted.back().bandwidth);
    return sorted.back();
}

}  // namespace hls
}  // namespace ytdlpspout

