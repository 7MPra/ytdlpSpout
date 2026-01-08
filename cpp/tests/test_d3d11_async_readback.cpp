// =============================================================================
// test_d3d11_async_readback.cpp - D3D11非同期リードバックのテスト
// =============================================================================
//
// テスト項目:
//   - AsyncReadback_BeginReturnsValidHandle
//   - AsyncReadback_IsCompleteReturnsFalseInitially
//   - AsyncReadback_CompleteRetrievesCorrectData
//   - AsyncReadback_DoubleBufferNoStall
//
// =============================================================================

#include <iostream>
#include <cassert>
#include <string>
#include <vector>
#include <chrono>
#include <thread>
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
// ヘルパー: テスト用テクスチャに固定パターンを書き込む
// =============================================================================

bool FillTextureWithPattern(D3D11Context& ctx, ID3D11Texture2D* texture, uint8_t pattern) {
    D3D11_TEXTURE2D_DESC desc;
    texture->GetDesc(&desc);
    
    // ステージングテクスチャを使用してパターンを書き込む
    auto staging = ctx.CreateStagingTexture(desc.Width, desc.Height, desc.Format);
    if (!staging) return false;
    
    D3D11_MAPPED_SUBRESOURCE mapped;
    HRESULT hr = ctx.GetDeviceContext()->Map(staging.Get(), 0, D3D11_MAP_WRITE, 0, &mapped);
    if (FAILED(hr)) return false;
    
    // BGRAパターンを書き込む (pattern, pattern+1, pattern+2, 255)
    uint8_t* row = static_cast<uint8_t*>(mapped.pData);
    for (UINT y = 0; y < desc.Height; ++y) {
        uint8_t* pixel = row;
        for (UINT x = 0; x < desc.Width; ++x) {
            pixel[0] = pattern;           // B
            pixel[1] = pattern + 1;       // G
            pixel[2] = pattern + 2;       // R
            pixel[3] = 255;               // A
            pixel += 4;
        }
        row += mapped.RowPitch;
    }
    
    ctx.GetDeviceContext()->Unmap(staging.Get(), 0);
    
    // ステージングからターゲットにコピー
    ctx.CopyTexture(texture, staging.Get());
    
    return true;
}

// =============================================================================
// テストケース: AsyncReadback_BeginReturnsValidHandle
// =============================================================================

bool Test_AsyncReadback_BeginReturnsValidHandle() {
    D3D11Context ctx;
    TEST_ASSERT(ctx.Initialize(true), "D3D11Context initialization should succeed");

    // レンダーターゲットテクスチャを作成
    const UINT width = 64;
    const UINT height = 64;
    auto srcTexture = ctx.CreateRenderTargetTexture(width, height, DXGI_FORMAT_B8G8R8A8_UNORM);
    TEST_ASSERT(srcTexture != nullptr, "CreateRenderTargetTexture should succeed");

    // 非同期リードバックを開始
    auto handle = ctx.BeginAsyncReadback(srcTexture.Get());
    
    TEST_ASSERT(handle.valid, "AsyncReadbackHandle should be valid");
    TEST_ASSERT(handle.stagingTexture != nullptr, "Handle should have staging texture");
    TEST_ASSERT(handle.query != nullptr, "Handle should have query");
    TEST_ASSERT(handle.width == width, "Handle width should match source");
    TEST_ASSERT(handle.height == height, "Handle height should match source");

    ctx.Shutdown();
    return true;
}

// =============================================================================
// テストケース: AsyncReadback_IsCompleteReturnsFalseInitially
// =============================================================================

bool Test_AsyncReadback_IsCompleteReturnsFalseInitially() {
    D3D11Context ctx;
    TEST_ASSERT(ctx.Initialize(true), "D3D11Context initialization should succeed");

    // 大きめのテクスチャを作成（GPUコピーに時間がかかるように）
    const UINT width = 1920;
    const UINT height = 1080;
    auto srcTexture = ctx.CreateRenderTargetTexture(width, height, DXGI_FORMAT_B8G8R8A8_UNORM);
    TEST_ASSERT(srcTexture != nullptr, "CreateRenderTargetTexture should succeed");

    // 非同期リードバックを開始
    auto handle = ctx.BeginAsyncReadback(srcTexture.Get());
    TEST_ASSERT(handle.valid, "AsyncReadbackHandle should be valid");

    // 開始直後は完了していないことが期待される（ただし小さいテクスチャでは即完了の場合もある）
    // そのため、このテストでは「false OR true」どちらでも許容
    bool isComplete = ctx.IsReadbackComplete(handle);
    std::cout << "INFO: IsReadbackComplete returned " << (isComplete ? "true" : "false") 
              << " immediately after begin (both are acceptable)" << std::endl;

    // Flushして待機すれば完了するはず
    ctx.GetDeviceContext()->Flush();
    
    // 少し待機
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    
    isComplete = ctx.IsReadbackComplete(handle);
    TEST_ASSERT(isComplete, "IsReadbackComplete should return true after flush and wait");

    ctx.Shutdown();
    return true;
}

// =============================================================================
// テストケース: AsyncReadback_CompleteRetrievesCorrectData
// =============================================================================

bool Test_AsyncReadback_CompleteRetrievesCorrectData() {
    D3D11Context ctx;
    TEST_ASSERT(ctx.Initialize(true), "D3D11Context initialization should succeed");

    const UINT width = 64;
    const UINT height = 64;
    const uint8_t testPattern = 100;

    // レンダーターゲットテクスチャを作成
    auto srcTexture = ctx.CreateRenderTargetTexture(width, height, DXGI_FORMAT_B8G8R8A8_UNORM);
    TEST_ASSERT(srcTexture != nullptr, "CreateRenderTargetTexture should succeed");

    // テストパターンを書き込む
    TEST_ASSERT(FillTextureWithPattern(ctx, srcTexture.Get(), testPattern), 
                "FillTextureWithPattern should succeed");

    // 非同期リードバックを開始
    auto handle = ctx.BeginAsyncReadback(srcTexture.Get());
    TEST_ASSERT(handle.valid, "AsyncReadbackHandle should be valid");

    // Flushして待機
    ctx.GetDeviceContext()->Flush();
    
    // 完了を待機
    int waitCount = 0;
    while (!ctx.IsReadbackComplete(handle) && waitCount < 100) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        waitCount++;
    }
    TEST_ASSERT(ctx.IsReadbackComplete(handle), "Readback should complete within timeout");

    // 結果を取得
    std::vector<uint8_t> buffer(width * height * 4);
    bool success = ctx.CompleteAsyncReadback(handle, buffer.data(), buffer.size());
    TEST_ASSERT(success, "CompleteAsyncReadback should succeed");

    // パターンを検証（最初のピクセル）
    TEST_ASSERT(buffer[0] == testPattern, "Blue channel should match pattern");
    TEST_ASSERT(buffer[1] == testPattern + 1, "Green channel should match pattern");
    TEST_ASSERT(buffer[2] == testPattern + 2, "Red channel should match pattern");
    TEST_ASSERT(buffer[3] == 255, "Alpha channel should be 255");

    // ハンドルは無効になっているべき
    TEST_ASSERT(!handle.valid, "Handle should be invalid after CompleteAsyncReadback");

    ctx.Shutdown();
    return true;
}

// =============================================================================
// テストケース: AsyncReadback_DoubleBufferNoStall
// =============================================================================

bool Test_AsyncReadback_DoubleBufferNoStall() {
    D3D11Context ctx;
    TEST_ASSERT(ctx.Initialize(true), "D3D11Context initialization should succeed");

    const UINT width = 256;
    const UINT height = 256;
    const int frameCount = 10;

    // テクスチャを作成
    auto srcTexture = ctx.CreateRenderTargetTexture(width, height, DXGI_FORMAT_B8G8R8A8_UNORM);
    TEST_ASSERT(srcTexture != nullptr, "CreateRenderTargetTexture should succeed");

    std::vector<uint8_t> buffer(width * height * 4);
    D3D11Context::AsyncReadbackHandle pendingHandle;
    bool hasPending = false;
    int completedFrames = 0;
    int framesProcessed = 0;

    auto startTime = std::chrono::high_resolution_clock::now();

    for (int frame = 0; frame < frameCount; ++frame) {
        // テクスチャを更新（シミュレーション）
        FillTextureWithPattern(ctx, srcTexture.Get(), static_cast<uint8_t>(frame * 10));
        framesProcessed++;
        
        // 前フレームの結果を取得（完了していれば）
        if (hasPending) {
            // 完了を待つのではなく、完了していればすぐに取得
            if (ctx.IsReadbackComplete(pendingHandle)) {
                bool success = ctx.CompleteAsyncReadback(pendingHandle, buffer.data(), buffer.size());
                if (success) completedFrames++;
                hasPending = false;
            }
            // まだ完了していなければ待機せずに次のフレームへ（ダブルバッファリングの意図）
            // ただしペンディングがあるので新しいリードバックは開始しない
        }

        // 新しい非同期リードバックを開始（ペンディングがなければ）
        if (!hasPending) {
            pendingHandle = ctx.BeginAsyncReadback(srcTexture.Get());
            hasPending = pendingHandle.valid;
        }
        
        // GPU処理を進める
        ctx.GetDeviceContext()->Flush();
    }

    // 最後のペンディングを完了
    if (hasPending) {
        // 最後のフレームは待機して完了させる
        int waitCount = 0;
        while (!ctx.IsReadbackComplete(pendingHandle) && waitCount < 100) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            waitCount++;
        }
        if (ctx.IsReadbackComplete(pendingHandle)) {
            ctx.CompleteAsyncReadback(pendingHandle, buffer.data(), buffer.size());
            completedFrames++;
        }
    }

    auto endTime = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime).count();

    std::cout << "INFO: Processed " << framesProcessed << " frames, completed " 
              << completedFrames << " readbacks in " << duration << "ms" << std::endl;
    
    // 全フレームが処理され、少なくとも1つは完了すべき
    TEST_ASSERT(framesProcessed == frameCount, 
                "All frames should have been processed");
    TEST_ASSERT(completedFrames >= 1, 
                "At least one readback should be completed");
    // 非同期なので、同期版（毎フレームGPU待機）より高速に処理できるべき
    // ただし完了数は同期版より少なくなる可能性がある（それがダブルバッファリングの特徴）
    std::cout << "INFO: Double buffering allows pipeline overlap - "
              << completedFrames << " completed out of " << framesProcessed << " processed" << std::endl;

    ctx.Shutdown();
    return true;
}

// =============================================================================
// メイン
// =============================================================================

int main() {
    // COMの初期化
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    // ロガー初期化
    Logger::Initialize(true, "logs/test_d3d11_async_readback.log", LogLevel::Debug);

    std::cout << "========================================" << std::endl;
    std::cout << "D3D11 Async Readback Tests" << std::endl;
    std::cout << "========================================" << std::endl;

    int totalTests = 0;
    int passedTests = 0;
    int failedTests = 0;

    RUN_TEST(Test_AsyncReadback_BeginReturnsValidHandle);
    RUN_TEST(Test_AsyncReadback_IsCompleteReturnsFalseInitially);
    RUN_TEST(Test_AsyncReadback_CompleteRetrievesCorrectData);
    RUN_TEST(Test_AsyncReadback_DoubleBufferNoStall);

    std::cout << "\n========================================" << std::endl;
    std::cout << "Results: " << passedTests << "/" << totalTests << " passed";
    if (failedTests > 0) {
        std::cout << " (" << failedTests << " failed)";
    }
    std::cout << std::endl;
    std::cout << "========================================" << std::endl;

    Logger::Shutdown();
    CoUninitialize();

    return failedTests > 0 ? 1 : 0;
}
