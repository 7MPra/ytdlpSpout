// =============================================================================
// BeatMap.h - ビートマップデータ構造
// =============================================================================
//
// 機能:
//   - ビート情報の格納
//   - 時刻によるビート検索（最近接、次、前）
//   - JSON形式でのシリアライズ/デシリアライズ
//
// =============================================================================

#pragma once

#include <string>
#include <vector>
#include <optional>

namespace ytdlpspout {

/// @brief ビート情報構造体
struct BeatInfo {
    double timestamp = 0.0;     ///< タイムスタンプ（秒）
    float confidence = 0.0f;    ///< 信頼度（0.0-1.0）
    int beatNumber = 0;         ///< ビート番号（4/4拍子で0-3）
    bool isDownbeat = false;    ///< ダウンビート（小節の頭）かどうか
};

/// @brief ビートマップクラス
/// @details 楽曲のビート情報を保持し、時刻によるクエリを提供
class BeatMap {
public:
    float bpm = 0.0f;                   ///< BPM（Beats Per Minute）
    float bpmConfidence = 0.0f;         ///< BPM検出の信頼度（0.0-1.0）
    std::vector<BeatInfo> beats;        ///< ビート配列
    double duration = 0.0;              ///< 楽曲の長さ（秒）

    // =========================================================================
    // クエリ
    // =========================================================================

    /// @brief 指定時刻に最も近いビートを取得
    /// @param timestamp 時刻（秒）
    /// @return ビート情報へのポインタ（見つからない場合nullptr）
    const BeatInfo* GetNearestBeat(double timestamp) const;

    /// @brief 指定時刻より後の次のビートを取得
    /// @param timestamp 時刻（秒）
    /// @return ビート情報へのポインタ（見つからない場合nullptr）
    const BeatInfo* GetNextBeat(double timestamp) const;

    /// @brief 指定時刻より前のビートを取得
    /// @param timestamp 時刻（秒）
    /// @return ビート情報へのポインタ（見つからない場合nullptr）
    const BeatInfo* GetPreviousBeat(double timestamp) const;

    // =========================================================================
    // シリアライズ
    // =========================================================================

    /// @brief ファイルに保存（JSON形式）
    /// @param path ファイルパス
    /// @return 成功した場合true
    bool SaveToFile(const std::string& path) const;

    /// @brief ファイルから読み込み（JSON形式）
    /// @param path ファイルパス
    /// @return BeatMapオブジェクト（失敗時nullopt）
    static std::optional<BeatMap> LoadFromFile(const std::string& path);
};

} // namespace ytdlpspout
