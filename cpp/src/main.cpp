// =============================================================================
// main.cpp - CLIエントリーポイント
// =============================================================================
//
// ytdlpSpout C++ Backend
//
// 使用方法:
//   ytdlpSpoutCLI <input> [options]
//
// 入力:
//   ローカルファイル、HTTP URL、YouTube等のURLを指定可能
//
// オプション:
//   -n, --name <name>     Spout sender name (default: ytdlpSpout)
//   -l, --loop            Loop playback
//   -v, --verbose         Enable verbose logging
//   -f, --format <fmt>    Format selection (e.g., "best", "1080p", "720p")
//   --height <pixels>     Preferred video height (default: 1080)
//   --ytdlp-path <path>   Path to yt-dlp executable
//   --no-hwaccel          Disable hardware acceleration
//   --no-progress         Disable progress display
//   -h, --help            Show help
//   --version             Show version
//
// 例:
//   ytdlpSpoutCLI video.mp4
//   ytdlpSpoutCLI video.mp4 --name "MyVideo" --loop
//   ytdlpSpoutCLI "https://www.youtube.com/watch?v=xxx" -f 1080p
//   ytdlpSpoutCLI "https://youtu.be/xxx" --height 720
//
// =============================================================================

#include "Application.h"

#include <Windows.h>
#include <objbase.h>
#include <iostream>

// Windows コンソール設定
static void SetupConsole() {
    // UTF-8コンソール出力を有効化
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);

    // ANSI エスケープシーケンスを有効化（Windows 10+）
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hOut != INVALID_HANDLE_VALUE) {
        DWORD dwMode = 0;
        if (GetConsoleMode(hOut, &dwMode)) {
            dwMode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
            SetConsoleMode(hOut, dwMode);
        }
    }
}

// =============================================================================
// メインエントリーポイント
// =============================================================================

int main(int argc, char* argv[]) {
    // コンソール設定
    SetupConsole();

    // COM初期化（DirectX使用のため）
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr)) {
        std::cerr << "Failed to initialize COM" << std::endl;
        return 1;
    }

    int result = 0;

    try {
        // アプリケーション実行
        ytdlpspout::Application app;

        if (!app.ParseArguments(argc, argv)) {
            // ヘルプ表示などで終了
            result = 0;
        } else {
            result = app.Run();
        }

    } catch (const std::exception& e) {
        std::cerr << "Fatal error: " << e.what() << std::endl;
        result = 1;
    } catch (...) {
        std::cerr << "Unknown fatal error" << std::endl;
        result = 1;
    }

    // COM終了
    CoUninitialize();

    return result;
}

// =============================================================================
// Windows GUI アプリとしてビルドする場合のエントリーポイント
// =============================================================================

#ifdef YTDLPSPOUT_GUI
int WINAPI WinMain(
    _In_ HINSTANCE hInstance,
    _In_opt_ HINSTANCE hPrevInstance,
    _In_ LPSTR lpCmdLine,
    _In_ int nCmdShow)
{
    return main(__argc, __argv);
}
#endif
