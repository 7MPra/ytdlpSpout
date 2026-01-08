// =============================================================================
// test_decoder_custom_io.cpp - VideoDecoder + CustomIOContext 統合テスト
// =============================================================================
//
// VideoDecoderのOpenWithCustomIO()メソッドをテスト
// 外部CustomIOContextを使用した動画読み込みの統合テスト
//
// =============================================================================

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include "decoder/VideoDecoder.h"
#include "io/CustomIOContext.h"

using namespace ytdlpspout;

// =============================================================================
// 基本テスト
// =============================================================================

TEST_CASE("VideoDecoder::OpenWithCustomIO with nullptr") {
    VideoDecoder decoder;
    
    // nullptrを渡すとfalseを返すべき
    CHECK_FALSE(decoder.OpenWithCustomIO(nullptr));
    CHECK_FALSE(decoder.IsOpen());
}

TEST_CASE("VideoDecoder::OpenWithCustomIO with uninitialized CustomIOContext") {
    VideoDecoder decoder;
    io::CustomIOContext ioContext;
    
    // 未初期化のCustomIOContextを渡すとfalseを返すべき
    CHECK_FALSE(ioContext.IsInitialized());
    CHECK_FALSE(decoder.OpenWithCustomIO(&ioContext));
    CHECK_FALSE(decoder.IsOpen());
}

// =============================================================================
// ローカルファイル統合テスト
// =============================================================================

TEST_CASE("VideoDecoder::OpenWithCustomIO with local file") {
    // テスト用の動画ファイルパス（環境に依存）
    // 存在しない場合はスキップ
    const char* testFile = "D:\\Downloads\\0001-0600.mp4";
    
    // ファイル存在チェック
    FILE* f = fopen(testFile, "rb");
    if (!f) {
        MESSAGE("Test file not found, skipping: ", testFile);
        return;
    }
    fclose(f);
    
    // CustomIOContextを初期化
    io::CustomIOContext ioContext;
    REQUIRE(ioContext.Initialize(testFile));
    REQUIRE(ioContext.IsInitialized());
    
    // VideoDecoderでオープン
    VideoDecoder decoder;
    CHECK(decoder.OpenWithCustomIO(&ioContext));
    CHECK(decoder.IsOpen());
    
    // 動画情報を取得
    auto info = decoder.GetVideoInfo();
    CHECK(info.width > 0);
    CHECK(info.height > 0);
    CHECK(info.fps > 0.0);
    
    MESSAGE("Opened video: ", info.width, "x", info.height, " @ ", info.fps, " fps");
    
    // デコードテスト
    CHECK(decoder.DecodeNextFrame());
    AVFrame* frame = decoder.GetCurrentFrame();
    CHECK(frame != nullptr);
    
    // クローズ
    decoder.Close();
    CHECK_FALSE(decoder.IsOpen());
    
    // CustomIOContextは外部所有なので、まだ有効（明示的にClose必要）
    ioContext.Close();
}

TEST_CASE("VideoDecoder::OpenWithCustomIO multiple frames decoding") {
    const char* testFile = "D:\\Downloads\\0001-0600.mp4";
    
    FILE* f = fopen(testFile, "rb");
    if (!f) {
        MESSAGE("Test file not found, skipping: ", testFile);
        return;
    }
    fclose(f);
    
    io::CustomIOContext ioContext;
    REQUIRE(ioContext.Initialize(testFile));
    
    VideoDecoder decoder;
    REQUIRE(decoder.OpenWithCustomIO(&ioContext));
    
    // 複数フレームをデコード
    int frameCount = 0;
    for (int i = 0; i < 30; ++i) {
        if (decoder.DecodeNextFrame()) {
            frameCount++;
            CHECK(decoder.GetCurrentPTS() >= 0.0);
        }
    }
    
    CHECK(frameCount > 0);
    MESSAGE("Decoded ", frameCount, " frames");
    
    decoder.Close();
    ioContext.Close();
}

TEST_CASE("VideoDecoder::OpenWithCustomIO seek functionality") {
    const char* testFile = "D:\\Downloads\\0001-0600.mp4";
    
    FILE* f = fopen(testFile, "rb");
    if (!f) {
        MESSAGE("Test file not found, skipping: ", testFile);
        return;
    }
    fclose(f);
    
    io::CustomIOContext ioContext;
    REQUIRE(ioContext.Initialize(testFile));
    
    VideoDecoder decoder;
    REQUIRE(decoder.OpenWithCustomIO(&ioContext));
    
    // 先にいくつかフレームをデコード
    for (int i = 0; i < 10; ++i) {
        decoder.DecodeNextFrame();
    }
    
    double ptsBeforeSeek = decoder.GetCurrentPTS();
    
    // 先頭にシーク
    CHECK(decoder.SeekToStart());
    CHECK(decoder.DecodeNextFrame());
    
    double ptsAfterSeek = decoder.GetCurrentPTS();
    CHECK(ptsAfterSeek < ptsBeforeSeek);
    
    decoder.Close();
    ioContext.Close();
}

// =============================================================================
// エッジケーステスト
// =============================================================================

TEST_CASE("VideoDecoder::OpenWithCustomIO double open") {
    const char* testFile = "D:\\Downloads\\0001-0600.mp4";
    
    FILE* f = fopen(testFile, "rb");
    if (!f) {
        MESSAGE("Test file not found, skipping: ", testFile);
        return;
    }
    fclose(f);
    
    io::CustomIOContext ioContext1;
    io::CustomIOContext ioContext2;
    REQUIRE(ioContext1.Initialize(testFile));
    REQUIRE(ioContext2.Initialize(testFile));
    
    VideoDecoder decoder;
    
    // 最初のオープン
    CHECK(decoder.OpenWithCustomIO(&ioContext1));
    CHECK(decoder.IsOpen());
    
    // 二回目のオープン（既存を閉じて新しいものを開く）
    CHECK(decoder.OpenWithCustomIO(&ioContext2));
    CHECK(decoder.IsOpen());
    
    decoder.Close();
    ioContext1.Close();
    ioContext2.Close();
}

TEST_CASE("VideoDecoder::OpenWithCustomIO compatibility with regular Open") {
    const char* testFile = "D:\\Downloads\\0001-0600.mp4";
    
    FILE* f = fopen(testFile, "rb");
    if (!f) {
        MESSAGE("Test file not found, skipping: ", testFile);
        return;
    }
    fclose(f);
    
    VideoDecoder decoder;
    
    // 通常のOpen
    CHECK(decoder.Open(testFile));
    CHECK(decoder.IsOpen());
    CHECK(decoder.DecodeNextFrame());
    decoder.Close();
    
    // CustomIOContextを使用したOpen
    io::CustomIOContext ioContext;
    REQUIRE(ioContext.Initialize(testFile));
    CHECK(decoder.OpenWithCustomIO(&ioContext));
    CHECK(decoder.IsOpen());
    CHECK(decoder.DecodeNextFrame());
    decoder.Close();
    ioContext.Close();
    
    // 再度通常のOpen
    CHECK(decoder.Open(testFile));
    CHECK(decoder.IsOpen());
    decoder.Close();
}
