// =============================================================================
// test_nv12_srv_pool.cpp - NV12 SRVテクスチャプールのテスト
// =============================================================================
//
// テスト項目:
//   - NV12Pool_AcquireReturnsValidTexture
//   - NV12Pool_ReusesSameTexture
//   - NV12Pool_DifferentSizeCreatesNew
//   - NV12Pool_ReleaseMakesAvailable
//   - NV12Pool_TextureHasSRVBindFlag
//   - ClearNV12Pool_WorksAfterClear
//
// =============================================================================

#include <iostream>
#include <cassert>
#include <string>
#include <vector>
#include <Windows.h>
#include <objbase.h>
#include <d3d11.h>

#include "graphics/D3D11Context.h"
#include "utils/Logger.h"

using namespace ytdlpspout;

// =============================================================================
// テストユーティリティ
// =============================================================================

#define TEST_ASSERT(condition, message)                           \
    do {                                                          \
        if (!(condition)) {                                       \
            std::cerr << "FAILED: " << (message) << std::endl;    \
            return false;                                         \
        }                                                         \
        std::cout << "PASSED: " << (message) << std::endl;        \
    } while (0)

#define RUN_TEST(func)                                            \
    do {                                                          \
        std::cout << "\n=== " #func " ===" << std::endl;          \
        if (func()) {                                             \
            passedTests++;                                        \
        } else {                                                  \
            failedTests++;                                        \
        }                                                         \
        totalTests++;                                             \
    } while (0)

// =============================================================================
// テストケース: NV12Pool_AcquireReturnsValidTexture
// =============================================================================

bool Test_NV12Pool_AcquireReturnsValidTexture() {
    D3D11Context ctx;
    TEST_ASSERT(ctx.Initialize(true), "D3D11Context initialization should succeed");

    // NV12 SRVテクスチャを取得
    auto texture = ctx.AcquireNV12SRVTexture(1920, 1080);
    TEST_ASSERT(texture != nullptr, "AcquireNV12SRVTexture should return valid texture");

    // テクスチャの属性を確認
    D3D11_TEXTURE2D_DESC desc;
    texture->GetDesc(&desc);
    TEST_ASSERT(desc.Width == 1920, "Texture width should be 1920");
    TEST_ASSERT(desc.Height == 1080, "Texture height should be 1080");
    TEST_ASSERT(desc.Format == DXGI_FORMAT_NV12, "Texture format should be NV12");
    TEST_ASSERT(desc.Usage == D3D11_USAGE_DEFAULT, "Texture should be default usage");

    ctx.ReleaseNV12SRVTexture(texture.Get());
    ctx.Shutdown();
    return true;
}

// =============================================================================
// テストケース: NV12Pool_TextureHasSRVBindFlag
// =============================================================================

bool Test_NV12Pool_TextureHasSRVBindFlag() {
    D3D11Context ctx;
    TEST_ASSERT(ctx.Initialize(true), "D3D11Context initialization should succeed");

    // NV12 SRVテクスチャを取得
    auto texture = ctx.AcquireNV12SRVTexture(1920, 1080);
    TEST_ASSERT(texture != nullptr, "AcquireNV12SRVTexture should return valid texture");

    // テクスチャにD3D11_BIND_SHADER_RESOURCEフラグがあることを確認
    D3D11_TEXTURE2D_DESC desc;
    texture->GetDesc(&desc);
    TEST_ASSERT((desc.BindFlags & D3D11_BIND_SHADER_RESOURCE) != 0, 
        "Texture should have D3D11_BIND_SHADER_RESOURCE flag");

    // SRVが実際に作成できることを確認
    D3D11_SHADER_RESOURCE_VIEW_DESC srvDescY = {};
    srvDescY.Format = DXGI_FORMAT_R8_UNORM;
    srvDescY.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDescY.Texture2D.MostDetailedMip = 0;
    srvDescY.Texture2D.MipLevels = 1;

    ComPtr<ID3D11ShaderResourceView> srvY;
    HRESULT hr = ctx.GetDevice()->CreateShaderResourceView(texture.Get(), &srvDescY, srvY.GetAddressOf());
    TEST_ASSERT(SUCCEEDED(hr), "Y plane SRV creation should succeed");

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDescUV = {};
    srvDescUV.Format = DXGI_FORMAT_R8G8_UNORM;
    srvDescUV.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDescUV.Texture2D.MostDetailedMip = 0;
    srvDescUV.Texture2D.MipLevels = 1;

    ComPtr<ID3D11ShaderResourceView> srvUV;
    hr = ctx.GetDevice()->CreateShaderResourceView(texture.Get(), &srvDescUV, srvUV.GetAddressOf());
    TEST_ASSERT(SUCCEEDED(hr), "UV plane SRV creation should succeed");

    ctx.ReleaseNV12SRVTexture(texture.Get());
    ctx.Shutdown();
    return true;
}

// =============================================================================
// テストケース: NV12Pool_ReusesSameTexture
// =============================================================================

bool Test_NV12Pool_ReusesSameTexture() {
    D3D11Context ctx;
    TEST_ASSERT(ctx.Initialize(true), "D3D11Context initialization should succeed");

    // 同じサイズのテクスチャを2回取得
    auto texture1 = ctx.AcquireNV12SRVTexture(1920, 1080);
    TEST_ASSERT(texture1 != nullptr, "First AcquireNV12SRVTexture should return valid texture");

    // 解放する
    ctx.ReleaseNV12SRVTexture(texture1.Get());

    // 同じサイズで再度取得 - 再利用されるべき
    auto texture2 = ctx.AcquireNV12SRVTexture(1920, 1080);
    TEST_ASSERT(texture2 != nullptr, "Second AcquireNV12SRVTexture should return valid texture");

    // 同じテクスチャポインタが返されるべき
    TEST_ASSERT(texture1.Get() == texture2.Get(), 
        "Same texture should be reused after release");

    ctx.ReleaseNV12SRVTexture(texture2.Get());
    ctx.Shutdown();
    return true;
}

// =============================================================================
// テストケース: NV12Pool_DifferentSizeCreatesNew
// =============================================================================

bool Test_NV12Pool_DifferentSizeCreatesNew() {
    D3D11Context ctx;
    TEST_ASSERT(ctx.Initialize(true), "D3D11Context initialization should succeed");

    // 1920x1080テクスチャを取得
    auto texture1 = ctx.AcquireNV12SRVTexture(1920, 1080);
    TEST_ASSERT(texture1 != nullptr, "First AcquireNV12SRVTexture should return valid texture");

    // 異なるサイズのテクスチャを取得（解放せずに）
    auto texture2 = ctx.AcquireNV12SRVTexture(1280, 720);
    TEST_ASSERT(texture2 != nullptr, "Second AcquireNV12SRVTexture should return valid texture");

    // 異なるテクスチャポインタが返されるべき
    TEST_ASSERT(texture1.Get() != texture2.Get(), 
        "Different size should create new texture");

    // サイズを確認
    D3D11_TEXTURE2D_DESC desc1, desc2;
    texture1->GetDesc(&desc1);
    texture2->GetDesc(&desc2);
    TEST_ASSERT(desc1.Width == 1920 && desc1.Height == 1080, "First texture dimensions should be 1920x1080");
    TEST_ASSERT(desc2.Width == 1280 && desc2.Height == 720, "Second texture dimensions should be 1280x720");

    ctx.ReleaseNV12SRVTexture(texture1.Get());
    ctx.ReleaseNV12SRVTexture(texture2.Get());
    ctx.Shutdown();
    return true;
}

// =============================================================================
// テストケース: NV12Pool_ReleaseMakesAvailable
// =============================================================================

bool Test_NV12Pool_ReleaseMakesAvailable() {
    D3D11Context ctx;
    TEST_ASSERT(ctx.Initialize(true), "D3D11Context initialization should succeed");

    // テクスチャを取得
    auto texture1 = ctx.AcquireNV12SRVTexture(1920, 1080);
    TEST_ASSERT(texture1 != nullptr, "First AcquireNV12SRVTexture should return valid texture");

    // 解放せずにもう一つ同じサイズを取得
    auto texture2 = ctx.AcquireNV12SRVTexture(1920, 1080);
    TEST_ASSERT(texture2 != nullptr, "Second AcquireNV12SRVTexture should return valid texture");

    // まだ使用中なので別のテクスチャが作られるべき
    TEST_ASSERT(texture1.Get() != texture2.Get(), 
        "In-use texture should not be reused");

    // 最初のテクスチャを解放
    ctx.ReleaseNV12SRVTexture(texture1.Get());

    // 再度取得 - 解放済みのtexture1が再利用されるべき
    auto texture3 = ctx.AcquireNV12SRVTexture(1920, 1080);
    TEST_ASSERT(texture3 != nullptr, "Third AcquireNV12SRVTexture should return valid texture");
    TEST_ASSERT(texture3.Get() == texture1.Get(), 
        "Released texture should be reused");

    ctx.ReleaseNV12SRVTexture(texture2.Get());
    ctx.ReleaseNV12SRVTexture(texture3.Get());
    ctx.Shutdown();
    return true;
}

// =============================================================================
// テストケース: ClearNV12Pool クリア後も正常動作
// =============================================================================

bool Test_ClearNV12Pool_WorksAfterClear() {
    D3D11Context ctx;
    TEST_ASSERT(ctx.Initialize(true), "D3D11Context initialization should succeed");

    // テクスチャを取得
    auto texture1 = ctx.AcquireNV12SRVTexture(1920, 1080);
    TEST_ASSERT(texture1 != nullptr, "First AcquireNV12SRVTexture should return valid texture");
    ctx.ReleaseNV12SRVTexture(texture1.Get());

    // プールをクリア
    ctx.ClearNV12Pool();

    // クリア後も新しいテクスチャが取得できることを確認
    auto texture2 = ctx.AcquireNV12SRVTexture(1920, 1080);
    TEST_ASSERT(texture2 != nullptr, "AcquireNV12SRVTexture after clear should return valid texture");

    // クリア後なので別のテクスチャが返されるべき
    TEST_ASSERT(texture1.Get() != texture2.Get(), 
        "New texture should be created after pool clear");

    ctx.ReleaseNV12SRVTexture(texture2.Get());
    ctx.Shutdown();
    return true;
}

// =============================================================================
// メイン関数
// =============================================================================

int main() {
    // COMを初期化
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr)) {
        std::cerr << "Failed to initialize COM" << std::endl;
        return 1;
    }

    // ロガー初期化
    Logger::Initialize(true, "logs/test_nv12_srv_pool.log", LogLevel::Debug);

    std::cout << "========================================" << std::endl;
    std::cout << "NV12 SRV Texture Pool Tests" << std::endl;
    std::cout << "========================================" << std::endl;

    int totalTests = 0;
    int passedTests = 0;
    int failedTests = 0;

    RUN_TEST(Test_NV12Pool_AcquireReturnsValidTexture);
    RUN_TEST(Test_NV12Pool_TextureHasSRVBindFlag);
    RUN_TEST(Test_NV12Pool_ReusesSameTexture);
    RUN_TEST(Test_NV12Pool_DifferentSizeCreatesNew);
    RUN_TEST(Test_NV12Pool_ReleaseMakesAvailable);
    RUN_TEST(Test_ClearNV12Pool_WorksAfterClear);

    std::cout << "\n========================================" << std::endl;
    std::cout << "Test Results: " << passedTests << "/" << totalTests << " passed";
    if (failedTests > 0) {
        std::cout << " (" << failedTests << " failed)";
    }
    std::cout << std::endl;
    std::cout << "========================================" << std::endl;

    CoUninitialize();

    return (failedTests == 0) ? 0 : 1;
}
