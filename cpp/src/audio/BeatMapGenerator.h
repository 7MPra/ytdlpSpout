// =============================================================================
// BeatMapGenerator.h - ビートマップ生成
// =============================================================================
//
// 機能:
//   - 音声ファイルからビートマップを生成
//   - 同期/非同期生成
//   - キャンセル対応
//   - 進捗コールバック
//
// 使用例:
//   BeatMapGenerator generator;
//   auto beatMap = generator.Generate("audio.mp3", [](double p) {
//       std::cout << "Progress: " << (p * 100) << "%" << std::endl;
//   });
//
// =============================================================================

#pragma once

#include "BeatMap.h"
#include "BeatDetector.h"

#include <functional>
#include <future>
#include <atomic>
#include <string>

namespace ytdlpspout {

/// @brief ビートマップ生成器
class BeatMapGenerator {
public:
    /// @brief 進捗コールバック型
    using ProgressCallback = std::function<void(double progress)>;
    
    /// @brief コンストラクタ
    BeatMapGenerator();
    
    /// @brief デストラクタ
    ~BeatMapGenerator() = default;
    
    // =========================================================================
    // 生成
    // =========================================================================
    
    /// @brief 同期的にビートマップを生成
    /// @param audioPath 音声ファイルパス
    /// @param progressCb 進捗コールバック（オプション）
    /// @return 生成されたビートマップ
    BeatMap Generate(const std::string& audioPath,
                     ProgressCallback progressCb = nullptr);
    
    /// @brief 非同期的にビートマップを生成
    /// @param audioPath 音声ファイルパス
    /// @param progressCb 進捗コールバック（オプション）
    /// @return ビートマップのfuture
    std::future<BeatMap> GenerateAsync(const std::string& audioPath,
                                       ProgressCallback progressCb = nullptr);
    
    // =========================================================================
    // キャンセル
    // =========================================================================
    
    /// @brief 生成をキャンセル
    void Cancel();
    
    /// @brief キャンセルされたかどうか
    /// @return キャンセルされた場合true
    bool IsCancelled() const;
    
private:
    std::atomic<bool> cancelled_;
};

/// @brief ビートジャンプコントローラー
/// @details ビート単位でのシーク制御を提供
class BeatJumpController {
public:
    /// @brief ジャンプ単位
    enum class JumpUnit {
        Beat_1,   ///< 1ビート
        Beat_2,   ///< 2ビート
        Beat_4,   ///< 4ビート（1小節 in 4/4）
        Beat_8,   ///< 8ビート（2小節）
        Beat_16   ///< 16ビート（4小節）
    };
    
    /// @brief コンストラクタ
    BeatJumpController();
    
    /// @brief デストラクタ
    ~BeatJumpController() = default;
    
    // =========================================================================
    // BeatMap設定
    // =========================================================================
    
    /// @brief ビートマップを設定
    /// @param beatMap ビートマップ
    void SetBeatMap(std::shared_ptr<BeatMap> beatMap);
    
    // =========================================================================
    // ジャンプ計算
    // =========================================================================
    
    /// @brief ジャンプ先の時刻を計算
    /// @param currentTime 現在時刻（秒）
    /// @param unit ジャンプ単位
    /// @param forward true=前進、false=後退
    /// @return ジャンプ先時刻（秒）
    double CalculateJumpTarget(double currentTime, JumpUnit unit, bool forward);
    
    /// @brief 最寄りのビートにクォンタイズ
    /// @param currentTime 現在時刻（秒）
    /// @return 最寄りのビート時刻（秒）
    double QuantizeToNearestBeat(double currentTime);
    
private:
    std::shared_ptr<BeatMap> beatMap_;
    
    /// @brief JumpUnitをビート数に変換
    int UnitToBeats(JumpUnit unit) const;
};

} // namespace ytdlpspout
