// =============================================================================
// test_m3u8_parser.cpp - M3U8パーサーのユニットテスト
// =============================================================================

#include <iostream>
#include <cassert>
#include <cmath>
#include "hls/M3U8Parser.h"
#include "utils/Logger.h"

using namespace ytdlpspout;
using namespace ytdlpspout::hls;

// =============================================================================
// テストユーティリティ
// =============================================================================

static int s_testsPassed = 0;
static int s_testsFailed = 0;

#define TEST_CASE(name) \
    std::cout << "  Testing: " << name << "..." << std::flush; \
    try {

#define TEST_END() \
        std::cout << " PASSED" << std::endl; \
        s_testsPassed++; \
    } catch (const std::exception& e) { \
        std::cout << " FAILED: " << e.what() << std::endl; \
        s_testsFailed++; \
    }

#define ASSERT_TRUE(expr) \
    if (!(expr)) throw std::runtime_error("Assertion failed: " #expr)

#define ASSERT_FALSE(expr) \
    if (expr) throw std::runtime_error("Assertion failed: !" #expr)

#define ASSERT_EQ(a, b) \
    if ((a) != (b)) throw std::runtime_error("Assertion failed: " #a " != " #b)

#define ASSERT_NE(a, b) \
    if ((a) == (b)) throw std::runtime_error("Assertion failed: " #a " == " #b)

#define ASSERT_NEAR(a, b, epsilon) \
    if (std::abs((a) - (b)) > (epsilon)) throw std::runtime_error("Assertion failed: " #a " ~= " #b)

// =============================================================================
// テストデータ
// =============================================================================

// シンプルなVODプレイリスト
const char* SIMPLE_M3U8 = R"(#EXTM3U
#EXT-X-VERSION:3
#EXT-X-TARGETDURATION:10
#EXT-X-MEDIA-SEQUENCE:0
#EXTINF:9.009,
segment0.ts
#EXTINF:9.009,
segment1.ts
#EXTINF:3.003,
segment2.ts
#EXT-X-ENDLIST
)";

// AES-128暗号化付きプレイリスト
const char* ENCRYPTED_M3U8 = R"(#EXTM3U
#EXT-X-VERSION:3
#EXT-X-TARGETDURATION:10
#EXT-X-MEDIA-SEQUENCE:100
#EXT-X-KEY:METHOD=AES-128,URI="https://example.com/key.bin",IV=0x00000000000000000000000000000001
#EXTINF:10.0,
https://example.com/video/seg100.ts
#EXTINF:10.0,
https://example.com/video/seg101.ts
#EXT-X-ENDLIST
)";

// バイト範囲指定プレイリスト
const char* BYTERANGE_M3U8 = R"(#EXTM3U
#EXT-X-VERSION:4
#EXT-X-TARGETDURATION:10
#EXT-X-MEDIA-SEQUENCE:0
#EXTINF:10.0,
#EXT-X-BYTERANGE:500000@0
video.mp4
#EXTINF:10.0,
#EXT-X-BYTERANGE:600000@500000
video.mp4
#EXTINF:5.0,
#EXT-X-BYTERANGE:250000@1100000
video.mp4
#EXT-X-ENDLIST
)";

// ライブストリームプレイリスト（ENDLISTなし）
const char* LIVE_M3U8 = R"(#EXTM3U
#EXT-X-VERSION:3
#EXT-X-TARGETDURATION:6
#EXT-X-MEDIA-SEQUENCE:2680
#EXTINF:5.960,
https://live.example.com/seg2680.ts
#EXTINF:5.960,
https://live.example.com/seg2681.ts
#EXTINF:5.960,
https://live.example.com/seg2682.ts
)";

// 相対URLプレイリスト
const char* RELATIVE_URL_M3U8 = R"(#EXTM3U
#EXT-X-VERSION:3
#EXT-X-TARGETDURATION:10
#EXT-X-MEDIA-SEQUENCE:0
#EXTINF:10.0,
segments/seg0.ts
#EXTINF:10.0,
../other/seg1.ts
#EXTINF:10.0,
/absolute/seg2.ts
#EXT-X-ENDLIST
)";

// 不正なプレイリスト（#EXTMINFなし）
const char* INVALID_M3U8 = R"(This is not a valid m3u8
segment0.ts
segment1.ts
)";

// Windows形式の改行（CRLF）
const char* CRLF_M3U8 = "#EXTM3U\r\n"
"#EXT-X-VERSION:3\r\n"
"#EXT-X-TARGETDURATION:10\r\n"
"#EXT-X-MEDIA-SEQUENCE:0\r\n"
"#EXTINF:10.0,\r\n"
"segment0.ts\r\n"
"#EXT-X-ENDLIST\r\n";

// =============================================================================
// パース基本テスト
// =============================================================================

void TestSimpleM3U8Parse() {
    TEST_CASE("Simple VOD m3u8 parsing")
    {
        auto result = M3U8Parser::Parse(SIMPLE_M3U8, "https://example.com/video/");
        ASSERT_TRUE(result.has_value());
        
        const auto& playlist = result.value();
        ASSERT_EQ(playlist.version, 3);
        ASSERT_EQ(playlist.targetDuration, 10.0);
        ASSERT_EQ(playlist.mediaSequence, 0);
        ASSERT_TRUE(playlist.isEndList);
        ASSERT_FALSE(playlist.isLive);
        ASSERT_EQ(playlist.segments.size(), 3);
        
        // セグメント0の検証
        ASSERT_NEAR(playlist.segments[0].duration, 9.009, 0.001);
        ASSERT_EQ(playlist.segments[0].url, "https://example.com/video/segment0.ts");
        ASSERT_EQ(playlist.segments[0].mediaSequence, 0);
        ASSERT_EQ(playlist.segments[0].index, 0);
        
        // セグメント1の検証
        ASSERT_NEAR(playlist.segments[1].duration, 9.009, 0.001);
        ASSERT_EQ(playlist.segments[1].url, "https://example.com/video/segment1.ts");
        ASSERT_EQ(playlist.segments[1].mediaSequence, 1);
        
        // セグメント2の検証
        ASSERT_NEAR(playlist.segments[2].duration, 3.003, 0.001);
        ASSERT_EQ(playlist.segments[2].url, "https://example.com/video/segment2.ts");
        ASSERT_EQ(playlist.segments[2].mediaSequence, 2);
        
        // 合計時間の検証
        ASSERT_NEAR(playlist.totalDuration, 9.009 + 9.009 + 3.003, 0.001);
    }
    TEST_END()
}

void TestEncryptedM3U8Parse() {
    TEST_CASE("AES-128 encrypted m3u8 parsing")
    {
        auto result = M3U8Parser::Parse(ENCRYPTED_M3U8, "https://example.com/");
        ASSERT_TRUE(result.has_value());
        
        const auto& playlist = result.value();
        ASSERT_EQ(playlist.mediaSequence, 100);
        ASSERT_TRUE(playlist.isEndList);
        ASSERT_FALSE(playlist.isLive);
        ASSERT_EQ(playlist.segments.size(), 2);
        
        // 暗号化キーの検証
        ASSERT_TRUE(playlist.encryptionKey.has_value());
        const auto& key = playlist.encryptionKey.value();
        ASSERT_EQ(key.method, "AES-128");
        ASSERT_EQ(key.keyUrl, "https://example.com/key.bin");
        ASSERT_EQ(key.iv.size(), 16);
        // IV = 0x00000000000000000000000000000001
        ASSERT_EQ(key.iv[15], 0x01);
        for (int i = 0; i < 15; ++i) {
            ASSERT_EQ(key.iv[i], 0x00);
        }
        
        // セグメントの検証（絶対URL）
        ASSERT_EQ(playlist.segments[0].url, "https://example.com/video/seg100.ts");
        ASSERT_EQ(playlist.segments[0].mediaSequence, 100);
        ASSERT_EQ(playlist.segments[1].url, "https://example.com/video/seg101.ts");
        ASSERT_EQ(playlist.segments[1].mediaSequence, 101);
    }
    TEST_END()
}

void TestByteRangeM3U8Parse() {
    TEST_CASE("Byte range m3u8 parsing")
    {
        auto result = M3U8Parser::Parse(BYTERANGE_M3U8, "https://example.com/video/");
        ASSERT_TRUE(result.has_value());
        
        const auto& playlist = result.value();
        ASSERT_EQ(playlist.version, 4);
        ASSERT_EQ(playlist.segments.size(), 3);
        
        // セグメント0: 0から500000バイト
        ASSERT_EQ(playlist.segments[0].byteRangeStart, 0);
        ASSERT_EQ(playlist.segments[0].byteRangeLength, 500000);
        
        // セグメント1: 500000から600000バイト
        ASSERT_EQ(playlist.segments[1].byteRangeStart, 500000);
        ASSERT_EQ(playlist.segments[1].byteRangeLength, 600000);
        
        // セグメント2: 1100000から250000バイト
        ASSERT_EQ(playlist.segments[2].byteRangeStart, 1100000);
        ASSERT_EQ(playlist.segments[2].byteRangeLength, 250000);
        
        // 全セグメントが同じファイルを指す
        ASSERT_EQ(playlist.segments[0].url, "https://example.com/video/video.mp4");
        ASSERT_EQ(playlist.segments[1].url, "https://example.com/video/video.mp4");
        ASSERT_EQ(playlist.segments[2].url, "https://example.com/video/video.mp4");
    }
    TEST_END()
}

void TestLiveM3U8Parse() {
    TEST_CASE("Live stream m3u8 parsing (no ENDLIST)")
    {
        auto result = M3U8Parser::Parse(LIVE_M3U8, "https://example.com/");
        ASSERT_TRUE(result.has_value());
        
        const auto& playlist = result.value();
        ASSERT_FALSE(playlist.isEndList);
        ASSERT_TRUE(playlist.isLive);
        ASSERT_EQ(playlist.mediaSequence, 2680);
        ASSERT_EQ(playlist.segments.size(), 3);
        
        // シーケンス番号の検証
        ASSERT_EQ(playlist.segments[0].mediaSequence, 2680);
        ASSERT_EQ(playlist.segments[1].mediaSequence, 2681);
        ASSERT_EQ(playlist.segments[2].mediaSequence, 2682);
    }
    TEST_END()
}

void TestCRLFLineEndings() {
    TEST_CASE("CRLF line endings handling")
    {
        auto result = M3U8Parser::Parse(CRLF_M3U8, "https://example.com/");
        ASSERT_TRUE(result.has_value());
        
        const auto& playlist = result.value();
        ASSERT_EQ(playlist.version, 3);
        ASSERT_EQ(playlist.segments.size(), 1);
        ASSERT_TRUE(playlist.isEndList);
    }
    TEST_END()
}

void TestInvalidM3U8() {
    TEST_CASE("Invalid m3u8 handling (missing EXTM3U)")
    {
        auto result = M3U8Parser::Parse(INVALID_M3U8, "https://example.com/");
        ASSERT_FALSE(result.has_value());
    }
    TEST_END()
}

void TestEmptyM3U8() {
    TEST_CASE("Empty m3u8 handling")
    {
        auto result = M3U8Parser::Parse("", "https://example.com/");
        ASSERT_FALSE(result.has_value());
        
        result = M3U8Parser::Parse("#EXTM3U\n", "https://example.com/");
        ASSERT_TRUE(result.has_value());
        ASSERT_EQ(result->segments.size(), 0);
    }
    TEST_END()
}

// =============================================================================
// URL解決テスト
// =============================================================================

void TestResolveUrl_RelativePath() {
    TEST_CASE("ResolveUrl - relative path")
    {
        std::string base = "https://example.com/video/playlist.m3u8";
        
        // 同じディレクトリの相対パス
        std::string resolved = M3U8Parser::ResolveUrl(base, "segment0.ts");
        ASSERT_EQ(resolved, "https://example.com/video/segment0.ts");
        
        // サブディレクトリ
        resolved = M3U8Parser::ResolveUrl(base, "segments/seg0.ts");
        ASSERT_EQ(resolved, "https://example.com/video/segments/seg0.ts");
    }
    TEST_END()
}

void TestResolveUrl_ParentDirectory() {
    TEST_CASE("ResolveUrl - parent directory (../)")
    {
        std::string base = "https://example.com/video/hls/playlist.m3u8";
        
        std::string resolved = M3U8Parser::ResolveUrl(base, "../segment0.ts");
        ASSERT_EQ(resolved, "https://example.com/video/segment0.ts");
        
        resolved = M3U8Parser::ResolveUrl(base, "../../other/seg.ts");
        ASSERT_EQ(resolved, "https://example.com/other/seg.ts");
    }
    TEST_END()
}

void TestResolveUrl_AbsolutePath() {
    TEST_CASE("ResolveUrl - absolute path (/)")
    {
        std::string base = "https://example.com/video/playlist.m3u8";
        
        std::string resolved = M3U8Parser::ResolveUrl(base, "/absolute/segment0.ts");
        ASSERT_EQ(resolved, "https://example.com/absolute/segment0.ts");
    }
    TEST_END()
}

void TestResolveUrl_FullUrl() {
    TEST_CASE("ResolveUrl - full URL (already absolute)")
    {
        std::string base = "https://example.com/video/playlist.m3u8";
        
        // httpsで始まる絶対URL
        std::string resolved = M3U8Parser::ResolveUrl(base, "https://cdn.example.com/seg0.ts");
        ASSERT_EQ(resolved, "https://cdn.example.com/seg0.ts");
        
        // httpで始まる絶対URL
        resolved = M3U8Parser::ResolveUrl(base, "http://cdn.example.com/seg0.ts");
        ASSERT_EQ(resolved, "http://cdn.example.com/seg0.ts");
    }
    TEST_END()
}

void TestResolveUrl_TrailingSlash() {
    TEST_CASE("ResolveUrl - base URL with trailing slash")
    {
        // ディレクトリとして扱われるベースURL
        std::string base = "https://example.com/video/";
        
        std::string resolved = M3U8Parser::ResolveUrl(base, "segment0.ts");
        ASSERT_EQ(resolved, "https://example.com/video/segment0.ts");
    }
    TEST_END()
}

void TestResolveUrl_QueryStringWithSlash() {
    TEST_CASE("ResolveUrl - base URL with slash inside query string (issue L-1b)")
    {
        // クエリ文字列内に'/'を含むベースURL（署名トークン等でよくあるパターン）
        std::string base = "https://example.com/video/playlist.m3u8?token=abc/def&sig=xyz";

        std::string resolved = M3U8Parser::ResolveUrl(base, "segment0.ts");
        // クエリ内の'/'に惑わされず、正しく"video/"までがベースパスになること
        ASSERT_EQ(resolved, "https://example.com/video/segment0.ts");
    }
    TEST_END()
}

void TestResolveUrl_SchemeRelative() {
    TEST_CASE("ResolveUrl - scheme-relative URL (//host/path) (issue L-1a)")
    {
        std::string base = "https://example.com/video/playlist.m3u8";

        std::string resolved = M3U8Parser::ResolveUrl(base, "//cdn.example.com/seg0.ts");
        ASSERT_EQ(resolved, "https://cdn.example.com/seg0.ts");

        // httpベースの場合はhttpスキームが付与されること
        std::string httpBase = "http://example.com/video/playlist.m3u8";
        resolved = M3U8Parser::ResolveUrl(httpBase, "//cdn.example.com/seg0.ts");
        ASSERT_EQ(resolved, "http://cdn.example.com/seg0.ts");
    }
    TEST_END()
}

void TestRelativeUrlM3U8Parse() {
    TEST_CASE("Relative URL resolution in m3u8")
    {
        auto result = M3U8Parser::Parse(RELATIVE_URL_M3U8, "https://example.com/video/hls/playlist.m3u8");
        ASSERT_TRUE(result.has_value());
        
        const auto& playlist = result.value();
        ASSERT_EQ(playlist.segments.size(), 3);
        
        // 相対パス: segments/seg0.ts
        ASSERT_EQ(playlist.segments[0].url, "https://example.com/video/hls/segments/seg0.ts");
        
        // 親ディレクトリ: ../other/seg1.ts
        ASSERT_EQ(playlist.segments[1].url, "https://example.com/video/other/seg1.ts");
        
        // 絶対パス: /absolute/seg2.ts
        ASSERT_EQ(playlist.segments[2].url, "https://example.com/absolute/seg2.ts");
    }
    TEST_END()
}

// =============================================================================
// エッジケーステスト
// =============================================================================

void TestCaseInsensitiveTags() {
    TEST_CASE("Case insensitive tag parsing")
    {
        const char* mixedCase = R"(#extm3u
#ext-x-version:3
#Ext-X-TargetDuration:10
#ext-x-media-sequence:0
#EXTINF:10.0,
segment0.ts
#ext-x-endlist
)";
        auto result = M3U8Parser::Parse(mixedCase, "https://example.com/");
        ASSERT_TRUE(result.has_value());
        ASSERT_EQ(result->version, 3);
        ASSERT_TRUE(result->isEndList);
    }
    TEST_END()
}

void TestExtinfWithTitle() {
    TEST_CASE("EXTINF with title text")
    {
        const char* withTitle = R"(#EXTM3U
#EXT-X-VERSION:3
#EXT-X-TARGETDURATION:10
#EXT-X-MEDIA-SEQUENCE:0
#EXTINF:10.0,Segment Title Here
segment0.ts
#EXT-X-ENDLIST
)";
        auto result = M3U8Parser::Parse(withTitle, "https://example.com/");
        ASSERT_TRUE(result.has_value());
        ASSERT_EQ(result->segments.size(), 1);
        ASSERT_NEAR(result->segments[0].duration, 10.0, 0.001);
    }
    TEST_END()
}

void TestKeyMethodNone() {
    TEST_CASE("EXT-X-KEY with METHOD=NONE")
    {
        const char* keyNone = R"(#EXTM3U
#EXT-X-VERSION:3
#EXT-X-TARGETDURATION:10
#EXT-X-MEDIA-SEQUENCE:0
#EXT-X-KEY:METHOD=NONE
#EXTINF:10.0,
segment0.ts
#EXT-X-ENDLIST
)";
        auto result = M3U8Parser::Parse(keyNone, "https://example.com/");
        ASSERT_TRUE(result.has_value());
        // METHOD=NONEの場合は暗号化なし
        ASSERT_FALSE(result->encryptionKey.has_value());
    }
    TEST_END()
}

void TestMultipleKeys() {
    TEST_CASE("Multiple EXT-X-KEY tags (last one wins)")
    {
        const char* multiKey = R"(#EXTM3U
#EXT-X-VERSION:3
#EXT-X-TARGETDURATION:10
#EXT-X-MEDIA-SEQUENCE:0
#EXT-X-KEY:METHOD=AES-128,URI="https://example.com/key1.bin",IV=0x00000000000000000000000000000001
#EXTINF:10.0,
segment0.ts
#EXT-X-KEY:METHOD=AES-128,URI="https://example.com/key2.bin",IV=0x00000000000000000000000000000002
#EXTINF:10.0,
segment1.ts
#EXT-X-ENDLIST
)";
        auto result = M3U8Parser::Parse(multiKey, "https://example.com/");
        ASSERT_TRUE(result.has_value());
        ASSERT_TRUE(result->encryptionKey.has_value());
        // 最後のキーが使用される
        ASSERT_EQ(result->encryptionKey->keyUrl, "https://example.com/key2.bin");
        ASSERT_EQ(result->encryptionKey->iv[15], 0x02);
    }
    TEST_END()
}

void TestSampleAesMethodParse() {
    TEST_CASE("EXT-X-KEY with METHOD=SAMPLE-AES is preserved (issue L-2b)")
    {
        const char* sampleAes = R"(#EXTM3U
#EXT-X-VERSION:5
#EXT-X-TARGETDURATION:10
#EXT-X-MEDIA-SEQUENCE:0
#EXT-X-KEY:METHOD=SAMPLE-AES,URI="https://example.com/key.bin",IV=0x00000000000000000000000000000001
#EXTINF:10.0,
segment0.ts
#EXT-X-ENDLIST
)";
        auto result = M3U8Parser::Parse(sampleAes, "https://example.com/");
        ASSERT_TRUE(result.has_value());
        ASSERT_TRUE(result->encryptionKey.has_value());
        // SAMPLE-AESはAES-128とは異なる方式として保持される（呼び出し側でフォールバック判断に使う）
        ASSERT_EQ(result->encryptionKey->method, "SAMPLE-AES");
        ASSERT_FALSE(result->hasKeyRotation);
    }
    TEST_END()
}

void TestKeyRotationDetected() {
    TEST_CASE("Key rotation is detected when EXT-X-KEY changes mid-playlist (issue L-2c)")
    {
        // 途中でURIが変わる（ローテーションする）プレイリスト
        auto result = M3U8Parser::Parse(R"(#EXTM3U
#EXT-X-VERSION:3
#EXT-X-TARGETDURATION:10
#EXT-X-MEDIA-SEQUENCE:0
#EXT-X-KEY:METHOD=AES-128,URI="https://example.com/key1.bin",IV=0x00000000000000000000000000000001
#EXTINF:10.0,
segment0.ts
#EXT-X-KEY:METHOD=AES-128,URI="https://example.com/key2.bin",IV=0x00000000000000000000000000000002
#EXTINF:10.0,
segment1.ts
#EXT-X-ENDLIST
)", "https://example.com/");
        ASSERT_TRUE(result.has_value());
        ASSERT_TRUE(result->hasKeyRotation);
    }
    TEST_END()
}

void TestKeyRotationNotDetectedWhenSame() {
    TEST_CASE("Key rotation is NOT flagged when repeated EXT-X-KEY is identical")
    {
        // 同じURI/IVの#EXT-X-KEYが繰り返し出現しても、ローテーションとは見なさない
        auto result = M3U8Parser::Parse(R"(#EXTM3U
#EXT-X-VERSION:3
#EXT-X-TARGETDURATION:10
#EXT-X-MEDIA-SEQUENCE:0
#EXT-X-KEY:METHOD=AES-128,URI="https://example.com/key1.bin",IV=0x00000000000000000000000000000001
#EXTINF:10.0,
segment0.ts
#EXT-X-KEY:METHOD=AES-128,URI="https://example.com/key1.bin",IV=0x00000000000000000000000000000001
#EXTINF:10.0,
segment1.ts
#EXT-X-ENDLIST
)", "https://example.com/");
        ASSERT_TRUE(result.has_value());
        ASSERT_FALSE(result->hasKeyRotation);
    }
    TEST_END()
}

void TestLiveM3U8HasNoKeyRotationByDefault() {
    TEST_CASE("Live playlist without ENDLIST is flagged isLive (issue L-2a, parser-level check)")
    {
        auto result = M3U8Parser::Parse(LIVE_M3U8, "https://example.com/");
        ASSERT_TRUE(result.has_value());
        // ライブ判定（#EXT-X-ENDLISTがない）はHlsSliceLoadingManager::Open()の
        // フォールバック判断に使われる
        ASSERT_TRUE(result->isLive);
        ASSERT_FALSE(result->hasKeyRotation);
    }
    TEST_END()
}

void TestByteRangeWithoutOffset() {
    TEST_CASE("EXT-X-BYTERANGE without offset (sequential)")
    {
        const char* byteRangeSeq = R"(#EXTM3U
#EXT-X-VERSION:4
#EXT-X-TARGETDURATION:10
#EXT-X-MEDIA-SEQUENCE:0
#EXTINF:10.0,
#EXT-X-BYTERANGE:500000@0
video.mp4
#EXTINF:10.0,
#EXT-X-BYTERANGE:600000
video.mp4
#EXT-X-ENDLIST
)";
        auto result = M3U8Parser::Parse(byteRangeSeq, "https://example.com/");
        ASSERT_TRUE(result.has_value());
        ASSERT_EQ(result->segments.size(), 2);
        
        // 最初のセグメント: 明示的なオフセット
        ASSERT_EQ(result->segments[0].byteRangeStart, 0);
        ASSERT_EQ(result->segments[0].byteRangeLength, 500000);
        
        // 2番目のセグメント: 前のセグメントの終端から継続
        ASSERT_EQ(result->segments[1].byteRangeStart, 500000);
        ASSERT_EQ(result->segments[1].byteRangeLength, 600000);
    }
    TEST_END()
}

// =============================================================================
// メイン関数
// =============================================================================

int main(int argc, char* argv[]) {
    std::cout << "=== M3U8 Parser Tests ===" << std::endl;
    
    // ロガー初期化（コンソール出力のみ、Warningレベル）
    ytdlpspout::Logger::Initialize(false, "", ytdlpspout::LogLevel::Warn);
    
    std::cout << "\n[Basic Parsing Tests]" << std::endl;
    TestSimpleM3U8Parse();
    TestEncryptedM3U8Parse();
    TestByteRangeM3U8Parse();
    TestLiveM3U8Parse();
    TestCRLFLineEndings();
    TestInvalidM3U8();
    TestEmptyM3U8();
    
    std::cout << "\n[URL Resolution Tests]" << std::endl;
    TestResolveUrl_RelativePath();
    TestResolveUrl_ParentDirectory();
    TestResolveUrl_AbsolutePath();
    TestResolveUrl_FullUrl();
    TestResolveUrl_TrailingSlash();
    TestResolveUrl_QueryStringWithSlash();
    TestResolveUrl_SchemeRelative();
    TestRelativeUrlM3U8Parse();

    std::cout << "\n[Edge Case Tests]" << std::endl;
    TestCaseInsensitiveTags();
    TestExtinfWithTitle();
    TestKeyMethodNone();
    TestMultipleKeys();
    TestSampleAesMethodParse();
    TestKeyRotationDetected();
    TestKeyRotationNotDetectedWhenSame();
    TestLiveM3U8HasNoKeyRotationByDefault();
    TestByteRangeWithoutOffset();

    std::cout << "\n=== Test Summary ===" << std::endl;
    std::cout << "  Passed: " << s_testsPassed << std::endl;
    std::cout << "  Failed: " << s_testsFailed << std::endl;
    
    return s_testsFailed > 0 ? 1 : 0;
}
