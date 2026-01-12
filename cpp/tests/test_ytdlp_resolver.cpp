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
    
    // コンストラクタ・パステスト
    RUN_TEST(TestYtDlpResolver_Constructor);
    RUN_TEST(TestYtDlpResolver_SetPath);
    
    // JSON解析テスト
    RUN_TEST(TestParseMetadataJson_ValidYouTube);
    RUN_TEST(TestParseMetadataJson_LiveStream);
    RUN_TEST(TestParseMetadataJson_Invalid);
    
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
