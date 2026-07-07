// =============================================================================
// test_ytdlp_resolver.cpp - YtDlpResolver ユニットテスト
// =============================================================================
//
// テスト対象:
//   - URL判定 (IsSupportedUrl)
//   - yt-dlpパス設定
//   - メタデータ解析（モック応答）
//
// 注意:
//   実際のyt-dlp呼び出しテストはネットワーク/外部依存のため統合テストで行う
//
// =============================================================================

#include "ytdlp/YtDlpResolver.h"
#include "utils/Logger.h"

#include <iostream>
#include <cassert>
#include <string>

using namespace ytdlpspout;
using namespace ytdlpspout::ytdlp;

// =============================================================================
// ヘルパーマクロ
// =============================================================================

#define TEST_ASSERT(cond, msg) \
    do { \
        if (!(cond)) { \
            std::cerr << "FAILED: " << msg << std::endl; \
            std::cerr << "  at " << __FILE__ << ":" << __LINE__ << std::endl; \
            return false; \
        } \
    } while(0)

#define RUN_TEST(func) \
    do { \
        std::cout << "Running " #func "..." << std::endl; \
        if (func()) { \
            std::cout << "  PASSED" << std::endl; \
            passed++; \
        } else { \
            std::cout << "  FAILED" << std::endl; \
            failed++; \
        } \
    } while(0)

// =============================================================================
// テストケース: URL判定
// =============================================================================

bool TestIsSupportedUrl_YouTube() {
    // YouTube URLs
    TEST_ASSERT(YtDlpResolver::IsSupportedUrl("https://www.youtube.com/watch?v=dQw4w9WgXcQ"), 
                "YouTube full URL should be supported");
    TEST_ASSERT(YtDlpResolver::IsSupportedUrl("https://youtube.com/watch?v=dQw4w9WgXcQ"), 
                "YouTube URL without www should be supported");
    TEST_ASSERT(YtDlpResolver::IsSupportedUrl("https://youtu.be/dQw4w9WgXcQ"), 
                "YouTube short URL should be supported");
    TEST_ASSERT(YtDlpResolver::IsSupportedUrl("http://www.youtube.com/watch?v=dQw4w9WgXcQ"), 
                "YouTube HTTP URL should be supported");
    
    return true;
}

bool TestIsSupportedUrl_Twitch() {
    // Twitch URLs
    TEST_ASSERT(YtDlpResolver::IsSupportedUrl("https://www.twitch.tv/videos/123456789"), 
                "Twitch video URL should be supported");
    TEST_ASSERT(YtDlpResolver::IsSupportedUrl("https://twitch.tv/streamer"), 
                "Twitch channel URL should be supported");
    
    return true;
}

bool TestIsSupportedUrl_Vimeo() {
    // Vimeo URLs
    TEST_ASSERT(YtDlpResolver::IsSupportedUrl("https://vimeo.com/123456789"), 
                "Vimeo URL should be supported");
    TEST_ASSERT(YtDlpResolver::IsSupportedUrl("https://player.vimeo.com/video/123456789"), 
                "Vimeo player URL should be supported");
    
    return true;
}

bool TestIsSupportedUrl_NicoNico() {
    // NicoNico URLs
    TEST_ASSERT(YtDlpResolver::IsSupportedUrl("https://www.nicovideo.jp/watch/sm12345678"), 
                "NicoNico URL should be supported");
    TEST_ASSERT(YtDlpResolver::IsSupportedUrl("https://nicovideo.jp/watch/sm12345678"), 
                "NicoNico URL without www should be supported");
    
    return true;
}

bool TestIsSupportedUrl_NotSupported() {
    // Not supported URLs (direct file URLs, local paths)
    TEST_ASSERT(!YtDlpResolver::IsSupportedUrl("C:\\Videos\\test.mp4"),
                "Local path should NOT be supported");
    TEST_ASSERT(!YtDlpResolver::IsSupportedUrl("/home/user/video.mp4"),
                "Unix local path should NOT be supported");
    TEST_ASSERT(!YtDlpResolver::IsSupportedUrl("https://example.com/video.mp4"),
                "Direct video URL should NOT be supported");
    TEST_ASSERT(!YtDlpResolver::IsSupportedUrl("http://192.168.1.1/stream.m3u8"),
                "Direct stream URL should NOT be supported");
    TEST_ASSERT(!YtDlpResolver::IsSupportedUrl(""),
                "Empty string should NOT be supported");

    return true;
}

bool TestIsSupportedUrl_ExpandedDomains() {
    // RES-8: C++側の既知ドメインリストをpython/ytdlp_resolver.pyのYTDLP_DOMAINSに揃えた
    // ことの確認（従来欠落していたドメインを含む）
    TEST_ASSERT(YtDlpResolver::IsSupportedUrl("https://www.instagram.com/p/abc123/"),
                "Instagram URL should be supported");
    TEST_ASSERT(YtDlpResolver::IsSupportedUrl("https://www.tiktok.com/@user/video/123"),
                "TikTok URL should be supported");
    TEST_ASSERT(YtDlpResolver::IsSupportedUrl("https://www.facebook.com/watch/?v=123"),
                "Facebook URL should be supported");
    TEST_ASSERT(YtDlpResolver::IsSupportedUrl("https://fb.watch/abc123/"),
                "fb.watch URL should be supported");
    TEST_ASSERT(YtDlpResolver::IsSupportedUrl("https://soundcloud.com/artist/track"),
                "SoundCloud URL should be supported");
    TEST_ASSERT(YtDlpResolver::IsSupportedUrl("https://artist.bandcamp.com/track/song"),
                "Bandcamp URL should be supported");
    TEST_ASSERT(YtDlpResolver::IsSupportedUrl("https://www.reddit.com/r/videos/comments/abc"),
                "Reddit URL should be supported");
    TEST_ASSERT(YtDlpResolver::IsSupportedUrl("https://www.youtube-nocookie.com/embed/abc"),
                "youtube-nocookie.com URL should be supported");
    TEST_ASSERT(YtDlpResolver::IsSupportedUrl("https://live.nicovideo.jp/watch/lv12345"),
                "live.nicovideo.jp URL should be supported");
    TEST_ASSERT(YtDlpResolver::IsSupportedUrl("https://www.bilibili.tv/en/video/123"),
                "bilibili.tv URL should be supported");
    TEST_ASSERT(YtDlpResolver::IsSupportedUrl("https://x.com/user/status/123"),
                "x.com URL should be supported");
    TEST_ASSERT(YtDlpResolver::IsSupportedUrl("https://music.youtube.com/watch?v=abc"),
                "music.youtube.com (subdomain) URL should be supported");

    return true;
}

bool TestIsSupportedUrl_UnknownDomain() {
    // 未知ドメインでも、直接メディア拡張子でなければyt-dlp解決対象（ページURL想定）
    TEST_ASSERT(YtDlpResolver::IsSupportedUrl("https://example.com/watch/12345"),
                "Unknown domain page URL should be supported (yt-dlp fallback)");
    TEST_ASSERT(YtDlpResolver::IsSupportedUrl("https://some-video-site.example/videos/abc"),
                "Unknown video site page URL should be supported");
    TEST_ASSERT(YtDlpResolver::IsSupportedUrl("https://example.com/watch?v=abc&list=xyz"),
                "Unknown domain URL with query string but no media extension should be supported");

    // 未知ドメインでも直接メディア拡張子なら直リンクとして扱う（yt-dlp対象外）
    TEST_ASSERT(!YtDlpResolver::IsSupportedUrl("https://cdn.example.com/media/clip.webm"),
                "Unknown domain .webm direct link should NOT be supported");
    TEST_ASSERT(!YtDlpResolver::IsSupportedUrl("https://cdn.example.com/media/clip.mp4?token=abc"),
                "Unknown domain .mp4 direct link with query string should NOT be supported");

    return true;
}

// =============================================================================
// テストケース: YtDlpResolverコンストラクタとパス設定
// =============================================================================

bool TestYtDlpResolver_Constructor() {
    YtDlpResolver resolver;
    
    // デフォルトパスはyt-dlp
    TEST_ASSERT(resolver.GetYtDlpPath() == "yt-dlp", 
                "Default yt-dlp path should be 'yt-dlp'");
    
    return true;
}

bool TestYtDlpResolver_SetPath() {
    YtDlpResolver resolver;
    
    resolver.SetYtDlpPath("C:\\Tools\\yt-dlp.exe");
    TEST_ASSERT(resolver.GetYtDlpPath() == "C:\\Tools\\yt-dlp.exe", 
                "Custom yt-dlp path should be set correctly");
    
    // 空パスの場合はデフォルトに戻す
    resolver.SetYtDlpPath("");
    TEST_ASSERT(resolver.GetYtDlpPath() == "yt-dlp", 
                "Empty path should reset to default");
    
    return true;
}

// =============================================================================
// テストケース: QuoteWinArg（RES-1: コマンド/引数インジェクション対策）
// =============================================================================

bool TestQuoteWinArg_Simple() {
    TEST_ASSERT(YtDlpResolver::QuoteWinArg("simple") == "\"simple\"",
                "Simple argument should be wrapped in quotes");
    TEST_ASSERT(YtDlpResolver::QuoteWinArg("") == "\"\"",
                "Empty argument should become an empty quoted string");

    return true;
}

bool TestQuoteWinArg_Spaces() {
    TEST_ASSERT(YtDlpResolver::QuoteWinArg("hello world") == "\"hello world\"",
                "Spaces should be preserved inside quotes");

    return true;
}

bool TestQuoteWinArg_Ampersand() {
    // '&' はクオート内では特別な意味を持たず、そのまま渡る
    TEST_ASSERT(YtDlpResolver::QuoteWinArg("a&b") == "\"a&b\"",
                "Ampersand should be inert inside quotes");
    TEST_ASSERT(YtDlpResolver::QuoteWinArg("https://example.com/watch?v=abc&list=xyz") ==
                "\"https://example.com/watch?v=abc&list=xyz\"",
                "URL with ampersand should be safely quoted as a single argument");

    return true;
}

bool TestQuoteWinArg_DoubleQuote() {
    // 内部の " は \" にエスケープされる
    TEST_ASSERT(YtDlpResolver::QuoteWinArg("say \"hi\"") == "\"say \\\"hi\\\"\"",
                "Embedded double quotes should be escaped as \\\"");

    return true;
}

bool TestQuoteWinArg_Backslashes() {
    // 通常のパス中のバックスラッシュは、"の直前や末尾でなければそのまま
    TEST_ASSERT(YtDlpResolver::QuoteWinArg("C:\\Tools\\yt-dlp.exe") ==
                "\"C:\\Tools\\yt-dlp.exe\"",
                "Backslashes not adjacent to a quote should be unchanged");

    // 末尾のバックスラッシュは閉じる"の直前で2倍化される
    TEST_ASSERT(YtDlpResolver::QuoteWinArg("C:\\path\\") == "\"C:\\path\\\\\"",
                "Trailing backslash before closing quote should be doubled");

    // バックスラッシュの直後に"が続く場合、2n+1本の\+エスケープされた"になる
    TEST_ASSERT(YtDlpResolver::QuoteWinArg("a\\\"b") == "\"a\\\\\\\"b\"",
                "Backslash immediately preceding an embedded quote should be doubled plus one escape");

    return true;
}

bool TestQuoteWinArg_CaretPercent() {
    // ^ と % はダブルクオート内では特殊文字ではなく、そのまま渡る
    TEST_ASSERT(YtDlpResolver::QuoteWinArg("100%^done") == "\"100%^done\"",
                "Caret and percent should pass through unchanged inside quotes");

    return true;
}

// =============================================================================
// テストケース: JSON解析
// =============================================================================

bool TestParseMetadataJson_ValidYouTube() {
    YtDlpResolver resolver;
    
    // yt-dlp -j の典型的な出力をシミュレート
    std::string json = R"({
        "id": "dQw4w9WgXcQ",
        "title": "Test Video",
        "uploader": "Test Channel",
        "duration": 212.0,
        "is_live": false,
        "formats": [
            {
                "format_id": "137",
                "url": "https://example.com/video1080p.mp4",
                "width": 1920,
                "height": 1080,
                "fps": 30,
                "vcodec": "avc1.640028",
                "acodec": "none",
                "filesize": 50000000
            },
            {
                "format_id": "140",
                "url": "https://example.com/audio.m4a",
                "width": null,
                "height": null,
                "fps": null,
                "vcodec": "none",
                "acodec": "mp4a.40.2",
                "filesize": 5000000
            },
            {
                "format_id": "22",
                "url": "https://example.com/video720p.mp4",
                "width": 1280,
                "height": 720,
                "fps": 30,
                "vcodec": "avc1.64001F",
                "acodec": "mp4a.40.2",
                "filesize": 30000000
            }
        ]
    })";
    
    auto metadata = resolver.ParseMetadataJson(json);
    TEST_ASSERT(metadata.has_value(), "Should parse valid JSON successfully");
    TEST_ASSERT(metadata->id == "dQw4w9WgXcQ", "Video ID should match");
    TEST_ASSERT(metadata->title == "Test Video", "Title should match");
    TEST_ASSERT(metadata->uploader == "Test Channel", "Uploader should match");
    TEST_ASSERT(std::abs(metadata->duration - 212.0) < 0.1, "Duration should match");
    TEST_ASSERT(!metadata->isLive, "Should not be live");
    TEST_ASSERT(metadata->formats.size() == 3, "Should have 3 formats");
    
    // 最初のフォーマットをチェック
    auto& fmt1 = metadata->formats[0];
    TEST_ASSERT(fmt1.formatId == "137", "Format ID should match");
    TEST_ASSERT(fmt1.width == 1920, "Width should match");
    TEST_ASSERT(fmt1.height == 1080, "Height should match");
    TEST_ASSERT(fmt1.fps == 30, "FPS should match");
    
    return true;
}

bool TestParseMetadataJson_LiveStream() {
    YtDlpResolver resolver;
    
    std::string json = R"({
        "id": "live123",
        "title": "Live Stream",
        "uploader": "Streamer",
        "duration": null,
        "is_live": true,
        "formats": []
    })";
    
    auto metadata = resolver.ParseMetadataJson(json);
    TEST_ASSERT(metadata.has_value(), "Should parse live stream JSON");
    TEST_ASSERT(metadata->isLive, "Should be live");
    TEST_ASSERT(metadata->duration == 0.0, "Duration should be 0 for live");
    
    return true;
}

bool TestParseMetadataJson_Invalid() {
    YtDlpResolver resolver;
    
    // 不正なJSON
    auto result1 = resolver.ParseMetadataJson("not json");
    TEST_ASSERT(!result1.has_value(), "Should fail for invalid JSON");
    
    // 空JSON
    auto result2 = resolver.ParseMetadataJson("{}");
    TEST_ASSERT(!result2.has_value(), "Should fail for empty JSON (no id)");
    
    // 空文字列
    auto result3 = resolver.ParseMetadataJson("");
    TEST_ASSERT(!result3.has_value(), "Should fail for empty string");

    return true;
}

bool TestParseMetadataJson_NullFields() {
    YtDlpResolver resolver;

    // RES-3: title/uploader/is_live/thumbnailがnullでも解析全体が失敗しないこと
    // （nlohmann::json の j.value(key, default) はキーが存在しても値がnullの場合に
    //   type_error(302)を送出するため、null安全なヘルパーで既定値にフォールバックする）
    std::string json = R"({
        "id": "abc123",
        "title": null,
        "uploader": null,
        "duration": null,
        "is_live": null,
        "thumbnail": null,
        "formats": [
            {
                "format_id": "137",
                "url": "https://example.com/video.mp4",
                "width": 1920,
                "height": 1080,
                "vcodec": null,
                "acodec": null
            }
        ]
    })";

    auto metadata = resolver.ParseMetadataJson(json);
    TEST_ASSERT(metadata.has_value(), "Should parse successfully even with null fields");
    TEST_ASSERT(metadata->id == "abc123", "ID should still be parsed correctly");
    TEST_ASSERT(metadata->title.empty(), "Null title should fall back to default (empty string)");
    TEST_ASSERT(metadata->uploader.empty(), "Null uploader should fall back to default (empty string)");
    TEST_ASSERT(metadata->duration == 0.0, "Null duration should fall back to default (0.0)");
    TEST_ASSERT(!metadata->isLive, "Null is_live should fall back to default (false)");
    TEST_ASSERT(metadata->thumbnailUrl.empty(), "Null thumbnail should fall back to default (empty string)");
    TEST_ASSERT(metadata->formats.size() == 1, "Should still parse the single format entry");
    TEST_ASSERT(metadata->formats[0].vcodec == "none", "Null vcodec should fall back to default \"none\"");
    TEST_ASSERT(metadata->formats[0].acodec == "none", "Null acodec should fall back to default \"none\"");

    return true;
}

// =============================================================================
// テストケース: フォーマット選択
// =============================================================================

bool TestSelectBestFormat_PreferHeight() {
    YtDlpResolver resolver;
    
    VideoMetadata metadata;
    metadata.id = "test";
    metadata.formats = {
        {"360p", "url1", 640, 360, 30, "avc1", "aac", 10000000},
        {"720p", "url2", 1280, 720, 30, "avc1", "aac", 20000000},
        {"1080p", "url3", 1920, 1080, 30, "avc1", "aac", 40000000},
        {"1440p", "url4", 2560, 1440, 30, "avc1", "aac", 80000000},
    };
    
    // 1080p希望 -> 1080pを選択
    auto fmt1 = resolver.SelectBestFormat(metadata, 1080);
    TEST_ASSERT(fmt1.has_value(), "Should find format for 1080p");
    TEST_ASSERT(fmt1->height == 1080, "Should select 1080p format");
    
    // 720p希望 -> 720pを選択
    auto fmt2 = resolver.SelectBestFormat(metadata, 720);
    TEST_ASSERT(fmt2.has_value(), "Should find format for 720p");
    TEST_ASSERT(fmt2->height == 720, "Should select 720p format");
    
    // 480p希望（存在しない）-> 360pを選択（下回る最大）
    auto fmt3 = resolver.SelectBestFormat(metadata, 480);
    TEST_ASSERT(fmt3.has_value(), "Should find fallback format");
    TEST_ASSERT(fmt3->height == 360, "Should select 360p as fallback");
    
    // 4K希望 -> 1440pを選択（超えない最大）
    auto fmt4 = resolver.SelectBestFormat(metadata, 2160);
    TEST_ASSERT(fmt4.has_value(), "Should find format for 4K request");
    TEST_ASSERT(fmt4->height == 1440, "Should select 1440p for 4K request");
    
    return true;
}

bool TestSelectBestFormat_PreferCodec() {
    YtDlpResolver resolver;
    
    VideoMetadata metadata;
    metadata.id = "test";
    metadata.formats = {
        {"vp9-1080", "url1", 1920, 1080, 30, "vp9", "opus", 40000000},
        {"h264-1080", "url2", 1920, 1080, 30, "avc1.640028", "aac", 40000000},
        {"av1-1080", "url3", 1920, 1080, 30, "av01", "opus", 35000000},
    };
    
    // h264/hevcを優先
    auto fmt = resolver.SelectBestFormat(metadata, 1080);
    TEST_ASSERT(fmt.has_value(), "Should find format");
    TEST_ASSERT(fmt->formatId == "h264-1080", "Should prefer h264 codec");
    
    return true;
}

bool TestSelectBestFormat_NoFormats() {
    YtDlpResolver resolver;
    
    VideoMetadata metadata;
    metadata.id = "test";
    metadata.formats = {};
    
    auto fmt = resolver.SelectBestFormat(metadata, 1080);
    TEST_ASSERT(!fmt.has_value(), "Should return empty for no formats");
    
    return true;
}

// =============================================================================
// テストケース: URL判定詳細
// =============================================================================

bool TestGetSourceType() {
    TEST_ASSERT(YtDlpResolver::GetSourceType("C:\\Videos\\test.mp4") == SourceType::LocalFile,
                "Windows path should be LocalFile");
    TEST_ASSERT(YtDlpResolver::GetSourceType("D:/Videos/test.mp4") == SourceType::LocalFile,
                "Windows path with forward slash should be LocalFile");
    TEST_ASSERT(YtDlpResolver::GetSourceType("/home/user/video.mp4") == SourceType::LocalFile,
                "Unix path should be LocalFile");
    TEST_ASSERT(YtDlpResolver::GetSourceType("https://example.com/video.mp4") == SourceType::HttpUrl,
                "Direct HTTPS URL should be HttpUrl");
    TEST_ASSERT(YtDlpResolver::GetSourceType("http://192.168.1.1/stream.m3u8") == SourceType::HttpUrl,
                "Direct HTTP URL should be HttpUrl");
    TEST_ASSERT(YtDlpResolver::GetSourceType("https://www.youtube.com/watch?v=xxx") == SourceType::YtDlpUrl,
                "YouTube URL should be YtDlpUrl");
    TEST_ASSERT(YtDlpResolver::GetSourceType("https://youtu.be/xxx") == SourceType::YtDlpUrl,
                "YouTube short URL should be YtDlpUrl");
    TEST_ASSERT(YtDlpResolver::GetSourceType("https://vimeo.com/123") == SourceType::YtDlpUrl,
                "Vimeo URL should be YtDlpUrl");
    TEST_ASSERT(YtDlpResolver::GetSourceType("https://example.com/watch/12345") == SourceType::YtDlpUrl,
                "Unknown domain page URL (no media extension) should be YtDlpUrl");
    TEST_ASSERT(YtDlpResolver::GetSourceType("https://cdn.example.com/media/clip.mp4") == SourceType::HttpUrl,
                "Unknown domain .mp4 direct link should be HttpUrl");

    return true;
}

// =============================================================================
// メイン
// =============================================================================

int main() {
    Logger::Initialize(false, "", LogLevel::Error); // テスト時はエラーのみ
    
    int passed = 0;
    int failed = 0;
    
    std::cout << "========================================" << std::endl;
    std::cout << "YtDlpResolver Unit Tests" << std::endl;
    std::cout << "========================================" << std::endl;
    
    // URL判定テスト
    RUN_TEST(TestIsSupportedUrl_YouTube);
    RUN_TEST(TestIsSupportedUrl_Twitch);
    RUN_TEST(TestIsSupportedUrl_Vimeo);
    RUN_TEST(TestIsSupportedUrl_NicoNico);
    RUN_TEST(TestIsSupportedUrl_NotSupported);
    RUN_TEST(TestIsSupportedUrl_UnknownDomain);
    RUN_TEST(TestIsSupportedUrl_ExpandedDomains);

    // コンストラクタ・パステスト
    RUN_TEST(TestYtDlpResolver_Constructor);
    RUN_TEST(TestYtDlpResolver_SetPath);

    // QuoteWinArgテスト（コマンド/引数インジェクション対策）
    RUN_TEST(TestQuoteWinArg_Simple);
    RUN_TEST(TestQuoteWinArg_Spaces);
    RUN_TEST(TestQuoteWinArg_Ampersand);
    RUN_TEST(TestQuoteWinArg_DoubleQuote);
    RUN_TEST(TestQuoteWinArg_Backslashes);
    RUN_TEST(TestQuoteWinArg_CaretPercent);

    // JSON解析テスト
    RUN_TEST(TestParseMetadataJson_ValidYouTube);
    RUN_TEST(TestParseMetadataJson_LiveStream);
    RUN_TEST(TestParseMetadataJson_Invalid);
    RUN_TEST(TestParseMetadataJson_NullFields);

    // フォーマット選択テスト
    RUN_TEST(TestSelectBestFormat_PreferHeight);
    RUN_TEST(TestSelectBestFormat_PreferCodec);
    RUN_TEST(TestSelectBestFormat_NoFormats);
    
    // ソースタイプ判定テスト
    RUN_TEST(TestGetSourceType);
    
    std::cout << "========================================" << std::endl;
    std::cout << "Results: " << passed << " passed, " << failed << " failed" << std::endl;
    std::cout << "========================================" << std::endl;
    
    Logger::Shutdown();
    
    return failed > 0 ? 1 : 0;
}
