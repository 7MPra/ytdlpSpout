// =============================================================================
// Application.h - アプリケーションメインロジック
// =============================================================================
//
// 機能:
//   - CLIアプリケーションのメインロジック
//   - コマンドライン引数解析
//   - シグナルハンドリング
//   - 初期化→再生ループ→クリーンアップの制御
//   - yt-dlp連携によるURL解決
//
// =============================================================================

#pragma once

#include <string>
#include <memory>

namespace ytdlpspout {

/// @brief アプリケーション設定
struct AppConfig {
    std::string inputFile;                  // 入力ファイルパス/URL
    std::string senderName = "ytdlpSpout";  // Spout Sender名
    bool loop = false;                      // ループ再生
    bool verbose = false;                   // 詳細ログ出力
    bool useHardwareAccel = true;           // ハードウェアアクセラレーション
    bool showProgress = true;               // 進捗表示
    std::string ytdlpPath = "yt-dlp";       // yt-dlpパス
    std::string format = "best";            // フォーマット選択
    int preferredHeight = 1080;             // 希望する高さ
    
    // ビートマップ関連
    bool analyzeBpm = false;                // 再生前にBPM解析を実行
    std::string loadBeatmapPath;            // ビートマップ読み込みパス
    std::string saveBeatmapPath;            // ビートマップ保存パス
};

/// @brief CLIアプリケーションクラス
class Application {
public:
    Application();
    ~Application();

    // コピー禁止
    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    // =========================================================================
    // 初期化・実行
    // =========================================================================

    /// @brief コマンドライン引数を解析
    /// @param argc 引数の数
    /// @param argv 引数配列
    /// @return 成功した場合true、ヘルプ表示などで終了する場合はfalse
    bool ParseArguments(int argc, char* argv[]);

    /// @brief アプリケーションを実行
    /// @return 終了コード（0=成功）
    int Run();

    /// @brief 設定を取得
    /// @return 現在の設定
    const AppConfig& GetConfig() const;

    // =========================================================================
    // 制御
    // =========================================================================

    /// @brief アプリケーションを停止
    void Stop();

    /// @brief 停止要求されているか
    /// @return 停止要求されている場合true
    bool IsStopRequested() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace ytdlpspout
