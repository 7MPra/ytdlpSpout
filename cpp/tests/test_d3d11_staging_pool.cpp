// =============================================================================
// test_d3d11_staging_pool.cpp - D3D11ステージングテクスチャプールのテスト
// =============================================================================
//
// テスト項目:
//   - StagingPool_AcquireReturnsValidTexture
//   - StagingPool_ReusesSameTexture
//   - StagingPool_DifferentSizeCreatesNew
//   - StagingPool_ReleaseMakesAvailable
//   - ReadbackTexture_UsesPool (統合テスト)
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
// テストケース: StagingPool_AcquireReturnsValidTexture
// =============================================================================

bool Test_StagingPool_AcquireReturnsValidTexture() {
    D3D11Context ctx;
    TEST_ASSERT(ctx.Initialize(true), "D3D11Context initialization should succeed");

    // ステージングテクスチャを取得
    auto texture = ctx.AcquireStagingTexture(1920, 1080, DXGI_FORMAT_B8G8R8A8_UNORM);
    TEST_ASSERT(texture != nullptr, "AcquireStagingTexture should return valid texture");

    // テクスチャの属性を確認
    D3D11_TEXTURE2D_DESC desc;
    texture->GetDesc(&desc);
    TEST_ASSERT(desc.Width == 1920, "Texture width should be 1920");
    TEST_ASSERT(desc.Height == 1080, "Texture height should be 1080");
    TEST_ASSERT(desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM, "Texture format should match");
    TEST_ASSERT(desc.Usage == D3D11_USAGE_STAGING, "Texture should be staging usage");
    TEST_ASSERT((desc.CPUAccessFlags & D3D11_CPU_ACCESS_READ) != 0, "Texture should have CPU read access");

    ctx.ReleaseStagingTexture(texture.Get());
    ctx.Shutdown();
    return true;
}

// =============================================================================
// テストケース: StagingPool_ReusesSameTexture
// =============================================================================

bool Test_StagingPool_ReusesSameTexture() {
    D3D11Context ctx;
    TEST_ASSERT(ctx.Initialize(true), "D3D11Context initialization should succeed");

    // 同じサイズ・フォーマットのテクスチャを2回取得
    auto texture1 = ctx.AcquireStagingTexture(1920, 1080, DXGI_FORMAT_B8G8R8A8_UNORM);
    TEST_ASSERT(texture1 != nullptr, "First AcquireStagingTexture should return valid texture");

    // 解放する
    ctx.ReleaseStagingTexture(texture1.Get());

    // 同じサイズで再度取得 - 再利用されるべき
    auto texture2 = ctx.AcquireStagingTexture(1920, 1080, DXGI_FORMAT_B8G8R8A8_UNORM);
    TEST_ASSERT(texture2 != nullptr, "Second AcquireStagingTexture should return valid texture");

    // 同じテクスチャポインタが返されるべき
    TEST_ASSERT(texture1.Get() == texture2.Get(), 
        "Same texture should be reused after release");

    ctx.ReleaseStagingTexture(texture2.Get());
    ctx.Shutdown();
    return true;
}

// =============================================================================
// テストケース: StagingPool_DifferentSizeCreatesNew
// =============================================================================

bool Test_StagingPool_DifferentSizeCreatesNew() {
    D3D11Context ctx;
    TEST_ASSERT(ctx.Initialize(true), "D3D11Context initialization should succeed");

    // 1920x1080テクスチャを取得
    auto texture1 = ctx.AcquireStagingTexture(1920, 1080, DXGI_FORMAT_B8G8R8A8_UNORM);
    TEST_ASSERT(texture1 != nullptr, "First AcquireStagingTexture should return valid texture");

    // 異なるサイズのテクスチャを取得（解放せずに）
    auto texture2 = ctx.AcquireStagingTexture(1280, 720, DXGI_FORMAT_B8G8R8A8_UNORM);
    TEST_ASSERT(texture2 != nullptr, "Second AcquireStagingTexture should return valid texture");

    // 異なるテクスチャポインタが返されるべき
    TEST_ASSERT(texture1.Get() != texture2.Get(), 
        "Different size should create new texture");

    // サイズを確認
    D3D11_TEXTURE2D_DESC desc1, desc2;
    texture1->GetDesc(&desc1);
    texture2->GetDesc(&desc2);
    TEST_ASSERT(desc1.Width == 1920 && desc1.Height == 1080, "First texture dimensions should be 1920x1080");
    TEST_ASSERT(desc2.Width == 1280 && desc2.Height == 720, "Second texture dimensions should be 1280x720");

    ctx.ReleaseStagingTexture(texture1.Get());
    ctx.ReleaseStagingTexture(texture2.Get());
    ctx.Shutdown();
    return true;
}

// =============================================================================
// テストケース: StagingPool_ReleaseMakesAvailable
// =============================================================================

bool Test_StagingPool_ReleaseMakesAvailable() {
    D3D11Context ctx;
    TEST_ASSERT(ctx.Initialize(true), "D3D11Context initialization should succeed");

    // テクスチャを取得
    auto texture1 = ctx.AcquireStagingTexture(1920, 1080, DXGI_FORMAT_B8G8R8A8_UNORM);
    TEST_ASSERT(texture1 != nullptr, "First AcquireStagingTexture should return valid texture");

    // 解放せずにもう一つ同じサイズを取得
    auto texture2 = ctx.AcquireStagingTexture(1920, 1080, DXGI_FORMAT_B8G8R8A8_UNORM);
    TEST_ASSERT(texture2 != nullptr, "Second AcquireStagingTexture should return valid texture");

    // まだ使用中なので別のテクスチャが作られるべき
    TEST_ASSERT(texture1.Get() != texture2.Get(), 
        "In-use texture should not be reused");

    // 最初のテクスチャを解放
    ctx.ReleaseStagingTexture(texture1.Get());

    // 再度取得 - 解放済みのtexture1が再利用されるべき
    auto texture3 = ctx.AcquireStagingTexture(1920, 1080, DXGI_FORMAT_B8G8R8A8_UNORM);
    TEST_ASSERT(texture3 != nullptr, "Third AcquireStagingTexture should return valid texture");
    TEST_ASSERT(texture3.Get() == texture1.Get(), 
        "Released texture should be reused");

    ctx.ReleaseStagingTexture(texture2.Get());
    ctx.ReleaseStagingTexture(texture3.Get());
    ctx.Shutdown();
    return true;
}

// =============================================================================
// テストケース: ReadbackTexture_UsesPool (統合テスト)
// =============================================================================

bool Test_ReadbackTexture_UsesPool() {
    D3D11Context ctx;
    TEST_ASSERT(ctx.Initialize(true), "D3D11Context initialization should succeed");

    // レンダーターゲットテクスチャを作成
    const UINT width = 64;
    const UINT height = 64;
    auto srcTexture = ctx.CreateRenderTargetTexture(width, height, DXGI_FORMAT_B8G8R8A8_UNORM);
    TEST_ASSERT(srcTexture != nullptr, "CreateRenderTargetTexture should succeed");

    // 読み取りバッファを準備
    std::vector<uint8_t> buffer(width * height * 4);

    // 複数回ReadbackTextureを呼び出し（プールが機能しているか確認）
    for (int i = 0; i < 3; i++) {
        bool result = ctx.ReadbackTexture(srcTexture.Get(), buffer.data(), buffer.size());
        TEST_ASSERT(result, "ReadbackTexture should succeed on iteration " + std::to_string(i));
    }

    // プールをクリアしてもエラーにならないことを確認
    ctx.ClearStagingPool();

    ctx.Shutdown();
    return true;
}

// =============================================================================
// テストケース: ClearStagingPool クリア後も正常動作
// =============================================================================

bool Test_ClearStagingPool_WorksAfterClear() {
    D3D11Context ctx;
    TEST_ASSERT(ctx.Initialize(true), "D3D11Context initialization should succeed");

    // テクスチャを取得
    auto texture1 = ctx.AcquireStagingTexture(1920, 1080, DXGI_FORMAT_B8G8R8A8_UNORM);
    TEST_ASSERT(texture1 != nullptr, "First AcquireStagingTexture should return valid texture");
    ctx.ReleaseStagingTexture(texture1.Get());

    // プールをクリア
    ctx.ClearStagingPool();

    // クリア後も新しいテクスチャが取得できることを確認
    auto texture2 = ctx.AcquireStagingTexture(1920, 1080, DXGI_FORMAT_B8G8R8A8_UNORM);
    TEST_ASSERT(texture2 != nullptr, "AcquireStagingTexture after clear should return valid texture");

    // クリア後なので別のテクスチャが返されるべき
    TEST_ASSERT(texture1.Get() != texture2.Get(), 
        "New texture should be created after pool clear");

    ctx.ReleaseStagingTexture(texture2.Get());
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
    Logger::Initialize(true, "logs/test_d3d11_staging_pool.log", LogLevel::Debug);

    std::cout << "========================================" << std::endl;
    std::cout << "D3D11 Staging Pool Tests" << std::endl;
    std::cout << "========================================" << std::endl;

    int totalTests = 0;
    int passedTests = 0;
    int failedTests = 0;

    RUN_TEST(Test_StagingPool_AcquireReturnsValidTexture);
    RUN_TEST(Test_StagingPool_ReusesSameTexture);
    RUN_TEST(Test_StagingPool_DifferentSizeCreatesNew);
    RUN_TEST(Test_StagingPool_ReleaseMakesAvailable);
    RUN_TEST(Test_ReadbackTexture_UsesPool);
    RUN_TEST(Test_ClearStagingPool_WorksAfterClear);

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
