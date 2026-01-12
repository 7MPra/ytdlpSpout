// =============================================================================
// test_decoder.cpp - デコーダーモジュールのテスト
// =============================================================================
//
// 基本的なユニットテスト（Google Testなしの簡易版）
//
// テスト項目:
//   - D3D11Contextの初期化
//   - テクスチャ作成
//   - FFmpegの初期化確認
//
// =============================================================================

#include <iostream>
#include <cassert>
#include <string>

// Windows
#include <Windows.h>
#include <objbase.h>
#include <d3d11.h>
#include <wrl/client.h>

// spdlog
#include <spdlog/spdlog.h>

// FFmpeg（利用可能か確認）
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/pixdesc.h>
}

// =============================================================================
// テストユーティリティ
// =============================================================================

#define TEST_ASSERT(condition, message)                           \
    do {                                                          \
        if (!(condition)) {                                       \
            std::cerr << "FAILED: " << (message) << std::endl;    \
            return false;                                          \
        }                                                          \
        std::cout << "PASSED: " << (message) << std::endl;        \
    } while (0)

#define RUN_TEST(func)                                            \
    do {                                                          \
        std::cout << "\n=== " #func " ===" << std::endl;          \
        if (func()) {                                             \
            passedTests++;                                         \
        } else {                                                   \
            failedTests++;                                         \
        }                                                          \
        totalTests++;                                              \
    } while (0)

// =============================================================================
// テストケース: D3D11デバイス作成
// =============================================================================

bool Test_D3D11DeviceCreation() {
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
    D3D_FEATURE_LEVEL featureLevel;

    UINT createFlags = 0;
#ifdef _DEBUG
    createFlags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    D3D_FEATURE_LEVEL featureLevels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0
    };

    HRESULT hr = D3D11CreateDevice(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        createFlags,
        featureLevels,
        _countof(featureLevels),
        D3D11_SDK_VERSION,
        device.GetAddressOf(),
        &featureLevel,
        context.GetAddressOf()
    );

    TEST_ASSERT(SUCCEEDED(hr), "D3D11CreateDevice should succeed");
    TEST_ASSERT(device != nullptr, "Device should not be null");
    TEST_ASSERT(context != nullptr, "Context should not be null");

    std::cout << "  Feature Level: 0x" << std::hex << featureLevel << std::dec << std::endl;

    return true;
}

// =============================================================================
// テストケース: テクスチャ作成
// =============================================================================

bool Test_TextureCreation() {
    // デバイス作成
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;

    D3D_FEATURE_LEVEL featureLevels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0
    };

    HRESULT hr = D3D11CreateDevice(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        0,
        featureLevels,
        _countof(featureLevels),
        D3D11_SDK_VERSION,
        device.GetAddressOf(),
        nullptr,
        context.GetAddressOf()
    );

    TEST_ASSERT(SUCCEEDED(hr), "Device creation should succeed");

    // テクスチャ作成
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = 1920;
    desc.Height = 1080;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    hr = device->CreateTexture2D(&desc, nullptr, texture.GetAddressOf());

    TEST_ASSERT(SUCCEEDED(hr), "Texture2D creation should succeed");
    TEST_ASSERT(texture != nullptr, "Texture should not be null");

    // テクスチャ情報確認
    D3D11_TEXTURE2D_DESC createdDesc;
    texture->GetDesc(&createdDesc);

    TEST_ASSERT(createdDesc.Width == 1920, "Width should be 1920");
    TEST_ASSERT(createdDesc.Height == 1080, "Height should be 1080");
    TEST_ASSERT(createdDesc.Format == DXGI_FORMAT_B8G8R8A8_UNORM, "Format should be BGRA");

    return true;
}

// =============================================================================
// テストケース: FFmpeg初期化
// =============================================================================

bool Test_FFmpegInitialization() {
    // FFmpegバージョン確認
    unsigned int avcodecVersion = avcodec_version();
    unsigned int avformatVersion = avformat_version();
    unsigned int avutilVersion = avutil_version();

    std::cout << "  avcodec version: " << AV_VERSION_MAJOR(avcodecVersion) << "."
              << AV_VERSION_MINOR(avcodecVersion) << "."
              << AV_VERSION_MICRO(avcodecVersion) << std::endl;

    std::cout << "  avformat version: " << AV_VERSION_MAJOR(avformatVersion) << "."
              << AV_VERSION_MINOR(avformatVersion) << "."
              << AV_VERSION_MICRO(avformatVersion) << std::endl;

    std::cout << "  avutil version: " << AV_VERSION_MAJOR(avutilVersion) << "."
              << AV_VERSION_MINOR(avutilVersion) << "."
              << AV_VERSION_MICRO(avutilVersion) << std::endl;

    TEST_ASSERT(avcodecVersion > 0, "avcodec should be available");
    TEST_ASSERT(avformatVersion > 0, "avformat should be available");

    // H.264デコーダーの確認
    const AVCodec* h264Decoder = avcodec_find_decoder(AV_CODEC_ID_H264);
    TEST_ASSERT(h264Decoder != nullptr, "H.264 decoder should be available");
    std::cout << "  H.264 decoder: " << h264Decoder->name << std::endl;

    // HEVCデコーダーの確認
    const AVCodec* hevcDecoder = avcodec_find_decoder(AV_CODEC_ID_HEVC);
    TEST_ASSERT(hevcDecoder != nullptr, "HEVC decoder should be available");
    std::cout << "  HEVC decoder: " << hevcDecoder->name << std::endl;

    return true;
}

// =============================================================================
// テストケース: ハードウェアデコーダー確認
// =============================================================================

bool Test_HardwareDecoders() {
    // D3D11VAの確認
    enum AVHWDeviceType hwType = av_hwdevice_find_type_by_name("d3d11va");
    TEST_ASSERT(hwType != AV_HWDEVICE_TYPE_NONE, "D3D11VA should be available");
    std::cout << "  D3D11VA device type: " << static_cast<int>(hwType) << std::endl;

    // 利用可能なHWデバイスタイプを列挙
    std::cout << "  Available HW device types:" << std::endl;
    AVHWDeviceType type = AV_HWDEVICE_TYPE_NONE;
    while ((type = av_hwdevice_iterate_types(type)) != AV_HWDEVICE_TYPE_NONE) {
        std::cout << "    - " << av_hwdevice_get_type_name(type) << std::endl;
    }

    return true;
}

// =============================================================================
// テストケース: NV12フォーマット確認
// =============================================================================

bool Test_NV12Format() {
    // NV12ピクセルフォーマットの確認
    const AVPixFmtDescriptor* nv12Desc = av_pix_fmt_desc_get(AV_PIX_FMT_NV12);
    TEST_ASSERT(nv12Desc != nullptr, "NV12 format should be available");
    std::cout << "  NV12 name: " << nv12Desc->name << std::endl;
    std::cout << "  NV12 components: " << static_cast<int>(nv12Desc->nb_components) << std::endl;

    // D3D11フォーマットの確認
    const AVPixFmtDescriptor* d3d11Desc = av_pix_fmt_desc_get(AV_PIX_FMT_D3D11);
    TEST_ASSERT(d3d11Desc != nullptr, "D3D11 format should be available");
    std::cout << "  D3D11 name: " << d3d11Desc->name << std::endl;

    return true;
}

// =============================================================================
// メイン
// =============================================================================

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "ytdlpSpout C++ Backend - Unit Tests" << std::endl;
    std::cout << "========================================" << std::endl;

    // COM初期化
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr)) {
        std::cerr << "Failed to initialize COM" << std::endl;
        return 1;
    }

    int totalTests = 0;
    int passedTests = 0;
    int failedTests = 0;

    // テスト実行
    RUN_TEST(Test_D3D11DeviceCreation);
    RUN_TEST(Test_TextureCreation);
    RUN_TEST(Test_FFmpegInitialization);
    RUN_TEST(Test_HardwareDecoders);
    RUN_TEST(Test_NV12Format);

    // 結果表示
    std::cout << "\n========================================" << std::endl;
    std::cout << "Test Results: " << passedTests << "/" << totalTests << " passed";
    if (failedTests > 0) {
        std::cout << " (" << failedTests << " failed)";
    }
    std::cout << std::endl;
    std::cout << "========================================" << std::endl;

    // COM終了
    CoUninitialize();

    return failedTests > 0 ? 1 : 0;
}
