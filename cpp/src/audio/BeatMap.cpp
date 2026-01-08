// =============================================================================
// BeatMap.cpp - ビートマップデータ構造実装
// =============================================================================

#include "BeatMap.h"
#include "utils/Logger.h"

#include <nlohmann/json.hpp>
#include <fstream>
#include <algorithm>
#include <cmath>

namespace ytdlpspout {

// =============================================================================
// クエリ
// =============================================================================

const BeatInfo* BeatMap::GetNearestBeat(double timestamp) const {
    if (beats.empty()) {
        return nullptr;
    }
    
    // 二分探索で最も近いビートを探す
    auto it = std::lower_bound(beats.begin(), beats.end(), timestamp,
        [](const BeatInfo& beat, double ts) {
            return beat.timestamp < ts;
        });
    
    // 境界チェック
    if (it == beats.end()) {
        // timestamp が全ビートより大きい
        return &beats.back();
    }
    
    if (it == beats.begin()) {
        // timestamp が最初のビート以下
        return &beats.front();
    }
    
    // 前後のビートと比較して近い方を返す
    auto prevIt = std::prev(it);
    double distToCurrent = std::abs(it->timestamp - timestamp);
    double distToPrev = std::abs(prevIt->timestamp - timestamp);
    
    return (distToPrev <= distToCurrent) ? &(*prevIt) : &(*it);
}

const BeatInfo* BeatMap::GetNextBeat(double timestamp) const {
    if (beats.empty()) {
        return nullptr;
    }
    
    // timestamp より大きい最初のビートを探す
    auto it = std::upper_bound(beats.begin(), beats.end(), timestamp,
        [](double ts, const BeatInfo& beat) {
            return ts < beat.timestamp;
        });
    
    if (it == beats.end()) {
        return nullptr;
    }
    
    return &(*it);
}

const BeatInfo* BeatMap::GetPreviousBeat(double timestamp) const {
    if (beats.empty()) {
        return nullptr;
    }
    
    // timestamp 以上の最初のビートを探す
    auto it = std::lower_bound(beats.begin(), beats.end(), timestamp,
        [](const BeatInfo& beat, double ts) {
            return beat.timestamp < ts;
        });
    
    // 最初のビートより前の場合
    if (it == beats.begin()) {
        return nullptr;
    }
    
    return &(*std::prev(it));
}

// =============================================================================
// シリアライズ
// =============================================================================

bool BeatMap::SaveToFile(const std::string& path) const {
    try {
        nlohmann::json j;
        
        j["version"] = 1;
        j["bpm"] = bpm;
        j["bpmConfidence"] = bpmConfidence;
        j["duration"] = duration;
        
        nlohmann::json beatsJson = nlohmann::json::array();
        for (const auto& beat : beats) {
            nlohmann::json beatJson;
            beatJson["timestamp"] = beat.timestamp;
            beatJson["confidence"] = beat.confidence;
            beatJson["beatNumber"] = beat.beatNumber;
            beatJson["isDownbeat"] = beat.isDownbeat;
            beatsJson.push_back(beatJson);
        }
        j["beats"] = beatsJson;
        
        std::ofstream file(path);
        if (!file.is_open()) {
            LOG_ERROR("Failed to open file for writing: {}", path);
            return false;
        }
        
        file << j.dump(2);  // 2スペースインデント
        file.close();
        
        LOG_INFO("Saved BeatMap to '{}': {} beats, BPM={:.1f}", 
                 path, beats.size(), bpm);
        
        return true;
        
    } catch (const std::exception& e) {
        LOG_ERROR("Failed to save BeatMap: {}", e.what());
        return false;
    }
}

std::optional<BeatMap> BeatMap::LoadFromFile(const std::string& path) {
    try {
        std::ifstream file(path);
        if (!file.is_open()) {
            LOG_ERROR("Failed to open file for reading: {}", path);
            return std::nullopt;
        }
        
        nlohmann::json j;
        file >> j;
        file.close();
        
        BeatMap beatMap;
        
        // バージョンチェック（将来の互換性のため）
        int version = j.value("version", 1);
        if (version > 1) {
            LOG_WARN("BeatMap file version {} may not be fully compatible", version);
        }
        
        beatMap.bpm = j.value("bpm", 0.0f);
        beatMap.bpmConfidence = j.value("bpmConfidence", 0.0f);
        beatMap.duration = j.value("duration", 0.0);
        
        if (j.contains("beats") && j["beats"].is_array()) {
            for (const auto& beatJson : j["beats"]) {
                BeatInfo beat;
                beat.timestamp = beatJson.value("timestamp", 0.0);
                beat.confidence = beatJson.value("confidence", 0.0f);
                beat.beatNumber = beatJson.value("beatNumber", 0);
                beat.isDownbeat = beatJson.value("isDownbeat", false);
                beatMap.beats.push_back(beat);
            }
        }
        
        LOG_INFO("Loaded BeatMap from '{}': {} beats, BPM={:.1f}",
                 path, beatMap.beats.size(), beatMap.bpm);
        
        return beatMap;
        
    } catch (const std::exception& e) {
        LOG_ERROR("Failed to load BeatMap: {}", e.what());
        return std::nullopt;
    }
}

} // namespace ytdlpspout
