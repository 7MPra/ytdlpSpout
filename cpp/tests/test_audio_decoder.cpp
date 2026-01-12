// =============================================================================
// test_audio_decoder.cpp - AudioDecoderのユニットテスト
// =============================================================================

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "audio/AudioDecoder.h"
#include <vector>
#include <cmath>

using namespace ytdlpspout;

// =============================================================================
// AudioDecoder 基本テスト
// =============================================================================

TEST_SUITE("AudioDecoder") {
    
    TEST_CASE("Default construction") {
        AudioDecoder decoder;
        
        CHECK(decoder.GetSampleRate() == 0);
        CHECK(decoder.GetChannels() == 0);
        CHECK(decoder.GetDuration() == 0.0);
        CHECK(decoder.IsEOF() == false);
    }
    
    TEST_CASE("Open non-existent file returns false") {
        AudioDecoder decoder;
        
        bool result = decoder.Open("non_existent_file.mp3");
        CHECK(result == false);
    }
    
    TEST_CASE("Close on unopened decoder is safe") {
        AudioDecoder decoder;
        
        // Should not crash
        decoder.Close();
        CHECK(decoder.GetSampleRate() == 0);
    }
    
    TEST_CASE("GetSamples on unopened decoder returns 0") {
        AudioDecoder decoder;
        
        std::vector<float> buffer(1024);
        int samples = decoder.GetSamples(buffer.data(), static_cast<int>(buffer.size()));
        CHECK(samples == 0);
    }
    
    TEST_CASE("Seek on unopened decoder returns false") {
        AudioDecoder decoder;
        
        bool result = decoder.Seek(1.0);
        CHECK(result == false);
    }
    
    TEST_CASE("Move constructor works") {
        AudioDecoder decoder1;
        AudioDecoder decoder2(std::move(decoder1));
        
        CHECK(decoder2.GetSampleRate() == 0);
    }
    
    TEST_CASE("Move assignment works") {
        AudioDecoder decoder1;
        AudioDecoder decoder2;
        
        decoder2 = std::move(decoder1);
        CHECK(decoder2.GetSampleRate() == 0);
    }
}

// =============================================================================
// AudioDecoder URLテスト（静的メソッド）
// =============================================================================

TEST_SUITE("AudioDecoder URL Detection") {
    
    TEST_CASE("Detect local file path") {
        CHECK(AudioDecoder::IsUrl("C:\\music\\song.mp3") == false);
        CHECK(AudioDecoder::IsUrl("/home/user/song.mp3") == false);
        CHECK(AudioDecoder::IsUrl("song.mp3") == false);
    }
    
    TEST_CASE("Detect HTTP URL") {
        CHECK(AudioDecoder::IsUrl("http://example.com/audio.mp3") == true);
        CHECK(AudioDecoder::IsUrl("https://example.com/audio.mp3") == true);
    }
}
