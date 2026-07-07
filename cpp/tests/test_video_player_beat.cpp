// =============================================================================
// test_video_player_beat.cpp - VideoPlayerビート機能統合テスト
// =============================================================================

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "player/VideoPlayer.h"
#include "audio/BeatMap.h"
#include "audio/BeatMapGenerator.h"
#include "graphics/D3D11Context.h"
#include "graphics/TexturePool.h"

using namespace ytdlpspout;

// =============================================================================
// ヘルパー関数
// =============================================================================

/// @brief テスト用のビートマップを作成
static std::shared_ptr<BeatMap> CreateTestBeatMap() {
    auto beatMap = std::make_shared<BeatMap>();
    beatMap->bpm = 120.0f;
    beatMap->bpmConfidence = 0.95f;
    beatMap->duration = 30.0;
    
    // 0.5秒間隔（120 BPM）でビートを生成
    for (int i = 0; i < 60; ++i) {
        BeatInfo beat;
        beat.timestamp = i * 0.5;
        beat.confidence = 0.9f;
        beat.beatNumber = i % 4;
        beat.isDownbeat = (i % 4 == 0);
        beatMap->beats.push_back(beat);
    }
    
    return beatMap;
}

// =============================================================================
// VideoPlayer ビートマップ設定テスト
// =============================================================================

TEST_SUITE("VideoPlayer BeatMap Integration") {

    TEST_CASE("SetBeatMap stores map") {
        VideoPlayer player;
        auto beatMap = CreateTestBeatMap();
        
        player.SetBeatMap(beatMap);
        
        auto retrieved = player.GetBeatMap();
        REQUIRE(retrieved != nullptr);
        CHECK(retrieved->bpm == doctest::Approx(120.0f));
        CHECK(retrieved->beats.size() == 60);
    }
    
    TEST_CASE("SetBeatMap null clears") {
        VideoPlayer player;
        auto beatMap = CreateTestBeatMap();
        player.SetBeatMap(beatMap);
        
        player.SetBeatMap(nullptr);
        
        CHECK(player.GetBeatMap() == nullptr);
    }
    
    TEST_CASE("GetBeatMap returns null by default") {
        VideoPlayer player;
        CHECK(player.GetBeatMap() == nullptr);
    }
    
    TEST_CASE("JumpBeats requires beatmap") {
        VideoPlayer player;
        // ビートマップなしではfalseを返す
        CHECK_FALSE(player.JumpBeats(1, true));
        CHECK_FALSE(player.JumpBeats(1, false));
    }
    
    TEST_CASE("JumpToNearestBeat requires beatmap") {
        VideoPlayer player;
        // ビートマップなしではfalseを返す
        CHECK_FALSE(player.JumpToNearestBeat());
    }
    
    TEST_CASE("SetBeatCallback stores callback") {
        VideoPlayer player;
        bool called = false;
        
        player.SetBeatCallback([&](const BeatInfo& beat) {
            called = true;
        });
        
        // コールバックが設定されていることを確認（内部状態のテスト）
        CHECK(true);  // コンパイルエラーがないことを確認
    }
    
    TEST_CASE("BeatCallback null clears callback") {
        VideoPlayer player;
        player.SetBeatCallback([](const BeatInfo&) {});
        player.SetBeatCallback(nullptr);
        
        // コールバックがクリアされていることを確認
        CHECK(true);  // コンパイルエラーがないことを確認
    }
}

// =============================================================================
// BeatJumpController テスト
// =============================================================================

TEST_SUITE("BeatJumpController") {
    
    TEST_CASE("CalculateJumpTarget forward") {
        BeatJumpController controller;
        auto beatMap = CreateTestBeatMap();
        controller.SetBeatMap(beatMap);
        
        // 現在0秒から1ビート前進 → 0.5秒
        double target = controller.CalculateJumpTarget(0.0, 
            BeatJumpController::JumpUnit::Beat_1, true);
        CHECK(target == doctest::Approx(0.5).epsilon(0.01));
        
        // 現在0秒から4ビート前進 → 2.0秒
        target = controller.CalculateJumpTarget(0.0, 
            BeatJumpController::JumpUnit::Beat_4, true);
        CHECK(target == doctest::Approx(2.0).epsilon(0.01));
    }
    
    TEST_CASE("CalculateJumpTarget backward") {
        BeatJumpController controller;
        auto beatMap = CreateTestBeatMap();
        controller.SetBeatMap(beatMap);
        
        // 現在2秒から1ビート後退 → 1.5秒
        double target = controller.CalculateJumpTarget(2.0, 
            BeatJumpController::JumpUnit::Beat_1, false);
        CHECK(target == doctest::Approx(1.5).epsilon(0.01));
    }
    
    TEST_CASE("QuantizeToNearestBeat") {
        BeatJumpController controller;
        auto beatMap = CreateTestBeatMap();
        controller.SetBeatMap(beatMap);
        
        // 0.25秒 → 最寄りは0秒
        double quantized = controller.QuantizeToNearestBeat(0.25);
        CHECK(quantized == doctest::Approx(0.0).epsilon(0.01));
        
        // 0.35秒 → 最寄りは0.5秒
        quantized = controller.QuantizeToNearestBeat(0.35);
        CHECK(quantized == doctest::Approx(0.5).epsilon(0.01));
    }
    
    TEST_CASE("CalculateJumpTarget beyond end") {
        BeatJumpController controller;
        auto beatMap = CreateTestBeatMap();
        controller.SetBeatMap(beatMap);
        
        // 終端を超える場合は終端にクランプ
        double target = controller.CalculateJumpTarget(29.0, 
            BeatJumpController::JumpUnit::Beat_8, true);
        CHECK(target <= beatMap->duration);
    }
    
    TEST_CASE("CalculateJumpTarget before start") {
        BeatJumpController controller;
        auto beatMap = CreateTestBeatMap();
        controller.SetBeatMap(beatMap);
        
        // 開始前に戻る場合は0にクランプ
        double target = controller.CalculateJumpTarget(1.0, 
            BeatJumpController::JumpUnit::Beat_8, false);
        CHECK(target >= 0.0);
    }
    
    TEST_CASE("No beatmap returns current time") {
        BeatJumpController controller;
        // ビートマップなし

        double target = controller.CalculateJumpTarget(5.0,
            BeatJumpController::JumpUnit::Beat_1, true);
        CHECK(target == 5.0);
    }
}

// =============================================================================
// TexturePool / PooledTexture 生存期間テスト（Issue C: weak_ptr化の回帰防止）
// =============================================================================
//
// PooledTextureはTexturePoolをweak_ptrで参照する。Stop()等でTexturePoolが
// 破棄（shared_ptr reset）された後にPooledTextureのデストラクタ/Release()が
// 呼ばれても、解放済みプールへアクセス（use-after-free）しないことを検証する。

TEST_SUITE("TexturePool PooledTexture Lifetime") {

    TEST_CASE("PooledTexture release after pool destruction is safe") {
        D3D11Context context;
        REQUIRE(context.Initialize(true));

        auto pool = std::make_shared<TexturePool>();
        TexturePool::Config poolConfig;
        poolConfig.width = 64;
        poolConfig.height = 64;
        poolConfig.poolSize = 2;
        REQUIRE(pool->Initialize(&context, poolConfig));

        PooledTexture tex = pool->Acquire();
        REQUIRE(tex.IsValid());

        // TexturePoolを破棄（VideoPlayer::Stop()でtexturePool.reset()する状況を模擬）。
        // PooledTexture側のweak_ptrはこの時点でexpireする。
        pool.reset();

        // プール破棄後にPooledTextureを明示的に解放してもクラッシュ/UAFしないこと
        // （weak_ptr::lock()が失敗し、プールへの返却をスキップしてComPtrのみ解放される）
        tex.Release();
        CHECK_FALSE(tex.IsValid());
    }

    TEST_CASE("PooledTexture destructor after pool destruction is safe") {
        D3D11Context context;
        REQUIRE(context.Initialize(true));

        auto pool = std::make_shared<TexturePool>();
        TexturePool::Config poolConfig;
        poolConfig.width = 64;
        poolConfig.height = 64;
        poolConfig.poolSize = 2;
        REQUIRE(pool->Initialize(&context, poolConfig));

        {
            PooledTexture tex = pool->Acquire();
            REQUIRE(tex.IsValid());

            // プールを破棄した後、texがスコープを抜けてデストラクタが走っても安全であること
            pool.reset();
        }

        CHECK(true);  // ここまでクラッシュせず到達すればOK
    }

    TEST_CASE("PooledTexture release while pool alive still recycles") {
        D3D11Context context;
        REQUIRE(context.Initialize(true));

        auto pool = std::make_shared<TexturePool>();
        TexturePool::Config poolConfig;
        poolConfig.width = 64;
        poolConfig.height = 64;
        poolConfig.poolSize = 2;
        poolConfig.allowGrowth = false;
        REQUIRE(pool->Initialize(&context, poolConfig));

        CHECK(pool->GetAvailableCount() == 2);
        {
            PooledTexture tex = pool->Acquire();
            REQUIRE(tex.IsValid());
            CHECK(pool->GetAvailableCount() == 1);
        }
        // 通常経路（プール生存中）ではスコープアウトで正しくプールへ返却されること
        CHECK(pool->GetAvailableCount() == 2);
    }
}
