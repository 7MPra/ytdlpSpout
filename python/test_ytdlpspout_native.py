"""
test_ytdlpspout_native.py - Pythonバインディングのテスト

このテストは DLL が存在する場合にのみ実行されます。
"""

import pytest
import sys
from pathlib import Path

# プロジェクトルートをパスに追加
project_root = Path(__file__).parent.parent
sys.path.insert(0, str(project_root))


# DLLが存在するかチェック
def find_dll():
    """テスト用にDLLを探す"""
    search_paths = [
        project_root / "python" / "ytdlpspout.dll",
        project_root / "cpp" / "build" / "vs2022" / "bin" / "Release" / "ytdlpspout.dll",
        project_root / "cpp" / "build" / "vs2022" / "bin" / "Debug" / "ytdlpspout.dll",
        project_root / "cpp" / "build" / "windows-x64-release" / "bin" / "ytdlpspout.dll",
        project_root / "cpp" / "build" / "windows-x64-debug" / "bin" / "ytdlpspout.dll",
    ]
    
    for path in search_paths:
        if path.exists():
            return str(path)
    return None


DLL_PATH = find_dll()
SKIP_REASON = "ytdlpspout.dll not found - build the C++ library first"


@pytest.mark.skipif(DLL_PATH is None, reason=SKIP_REASON)
class TestYtdlpSpoutNative:
    """YtdlpSpoutNativeクラスのテスト"""
    
    def test_import(self):
        """モジュールがインポートできる"""
        from python.ytdlpspout_native import YtdlpSpoutNative
        assert YtdlpSpoutNative is not None
    
    def test_create_player(self):
        """プレイヤーを作成できる"""
        from python.ytdlpspout_native import YtdlpSpoutNative
        
        player = YtdlpSpoutNative(DLL_PATH)
        assert player is not None
        assert player._handle is not None
    
    def test_version(self):
        """バージョンを取得できる"""
        from python.ytdlpspout_native import YtdlpSpoutNative
        
        player = YtdlpSpoutNative(DLL_PATH)
        version = player.version
        assert version is not None
        assert isinstance(version, str)
        assert len(version) > 0
    
    def test_initial_state_is_stopped(self):
        """初期状態がSTOPPED"""
        from python.ytdlpspout_native import YtdlpSpoutNative, YtdlpSpoutState
        
        player = YtdlpSpoutNative(DLL_PATH)
        assert player.state == YtdlpSpoutState.STOPPED
    
    def test_is_playing_initially_false(self):
        """初期状態ではis_playingがFalse"""
        from python.ytdlpspout_native import YtdlpSpoutNative
        
        player = YtdlpSpoutNative(DLL_PATH)
        assert player.is_playing is False
    
    def test_position_initially_zero(self):
        """初期状態でpositionが0"""
        from python.ytdlpspout_native import YtdlpSpoutNative
        
        player = YtdlpSpoutNative(DLL_PATH)
        assert player.position == 0.0
    
    def test_duration_initially_zero(self):
        """初期状態でdurationが0"""
        from python.ytdlpspout_native import YtdlpSpoutNative
        
        player = YtdlpSpoutNative(DLL_PATH)
        assert player.duration == 0.0
    
    def test_bpm_initially_zero(self):
        """ビートマップがない場合bpmが0"""
        from python.ytdlpspout_native import YtdlpSpoutNative
        
        player = YtdlpSpoutNative(DLL_PATH)
        assert player.bpm == 0.0
    
    def test_get_last_error(self):
        """エラーメッセージを取得できる"""
        from python.ytdlpspout_native import YtdlpSpoutNative
        
        player = YtdlpSpoutNative(DLL_PATH)
        error = player.get_last_error()
        assert isinstance(error, str)
    
    def test_stop_without_start(self):
        """start前のstopでエラーが発生しない"""
        from python.ytdlpspout_native import YtdlpSpoutNative
        
        player = YtdlpSpoutNative(DLL_PATH)
        player.stop()  # Should not raise
    
    def test_context_manager(self):
        """コンテキストマネージャーとして使用できる"""
        from python.ytdlpspout_native import YtdlpSpoutNative
        
        with YtdlpSpoutNative(DLL_PATH) as player:
            assert player is not None
            assert player.state == 0  # STOPPED
    
    def test_jump_beats_without_beatmap(self):
        """ビートマップなしでjump_beatsがFalse"""
        from python.ytdlpspout_native import YtdlpSpoutNative
        
        player = YtdlpSpoutNative(DLL_PATH)
        result = player.jump_beats(4, forward=True)
        assert result is False
    
    def test_get_video_info_without_playback(self):
        """再生なしでget_video_infoを呼べる"""
        from python.ytdlpspout_native import YtdlpSpoutNative
        
        player = YtdlpSpoutNative(DLL_PATH)
        info = player.get_video_info()
        # 再生していないのでNoneまたは0の情報
        # (実装によってはNone、またはゼロ埋めの辞書)


@pytest.mark.skipif(DLL_PATH is None, reason=SKIP_REASON)
class TestCallbacks:
    """コールバック関連のテスト"""
    
    def test_set_progress_callback(self):
        """進捗コールバックを設定できる"""
        from python.ytdlpspout_native import YtdlpSpoutNative
        
        player = YtdlpSpoutNative(DLL_PATH)
        
        def on_progress(current, duration):
            pass
        
        player.set_progress_callback(on_progress)
        assert player._on_progress is not None
        
        player.set_progress_callback(None)
        assert player._on_progress is None
    
    def test_set_error_callback(self):
        """エラーコールバックを設定できる"""
        from python.ytdlpspout_native import YtdlpSpoutNative
        
        player = YtdlpSpoutNative(DLL_PATH)
        
        def on_error(message):
            pass
        
        player.set_error_callback(on_error)
        assert player._on_error is not None
        
        player.set_error_callback(None)
        assert player._on_error is None
    
    def test_set_completion_callback(self):
        """完了コールバックを設定できる"""
        from python.ytdlpspout_native import YtdlpSpoutNative
        
        player = YtdlpSpoutNative(DLL_PATH)
        
        def on_completion():
            pass
        
        player.set_completion_callback(on_completion)
        assert player._on_completion is not None
        
        player.set_completion_callback(None)
        assert player._on_completion is None


@pytest.mark.skipif(DLL_PATH is None, reason=SKIP_REASON)
class TestFrameBuffer:
    """フレームバッファ関連のテスト"""
    
    def test_get_frame_buffer_size_initially_zero(self):
        """初期状態でフレームバッファサイズが0"""
        from python.ytdlpspout_native import YtdlpSpoutNative
        
        player = YtdlpSpoutNative(DLL_PATH)
        size = player.get_frame_buffer_size()
        assert size == 0  # 再生前は0
    
    def test_get_current_frame_returns_none_without_playback(self):
        """再生前はget_current_frameがNoneを返す"""
        from python.ytdlpspout_native import YtdlpSpoutNative
        
        player = YtdlpSpoutNative(DLL_PATH)
        frame = player.get_current_frame()
        assert frame is None  # 再生前はNone


@pytest.mark.skipif(DLL_PATH is None, reason=SKIP_REASON)
class TestHelperFunctions:
    """ヘルパー関数のテスト"""
    
    def test_get_version(self):
        """get_version関数が動作する"""
        from python.ytdlpspout_native import get_version
        
        version = get_version(DLL_PATH)
        assert version is not None
        assert isinstance(version, str)


@pytest.mark.skipif(DLL_PATH is None, reason=SKIP_REASON)
class TestSliceLoadingAPI:
    """スライス読み込みAPI関連のテスト（Phase 4）"""
    
    def test_start_ex_method_exists(self):
        """start_ex メソッドが存在することを確認"""
        from python.ytdlpspout_native import YtdlpSpoutNative
        
        player = YtdlpSpoutNative(DLL_PATH)
        assert hasattr(player, 'start_ex')
        assert callable(getattr(player, 'start_ex'))
    
    def test_download_progress_property_exists(self):
        """download_progress プロパティが存在することを確認"""
        from python.ytdlpspout_native import YtdlpSpoutNative
        
        player = YtdlpSpoutNative(DLL_PATH)
        assert hasattr(player, 'download_progress')
        # 未再生時は0.0
        assert player.download_progress == 0.0
    
    def test_bandwidth_property_exists(self):
        """bandwidth プロパティが存在することを確認"""
        from python.ytdlpspout_native import YtdlpSpoutNative
        
        player = YtdlpSpoutNative(DLL_PATH)
        assert hasattr(player, 'bandwidth')
        # 未再生時は0.0
        assert player.bandwidth == 0.0
    
    def test_is_fully_cached_property_exists(self):
        """is_fully_cached プロパティが存在することを確認"""
        from python.ytdlpspout_native import YtdlpSpoutNative
        
        player = YtdlpSpoutNative(DLL_PATH)
        assert hasattr(player, 'is_fully_cached')
        # 未再生時はFalse
        assert player.is_fully_cached is False
    
    def test_get_cache_stats_method_exists(self):
        """get_cache_stats メソッドが存在することを確認"""
        from python.ytdlpspout_native import YtdlpSpoutNative
        
        player = YtdlpSpoutNative(DLL_PATH)
        assert hasattr(player, 'get_cache_stats')
        assert callable(getattr(player, 'get_cache_stats'))
        # 未再生時は(0, 0)を返す
        stats = player.get_cache_stats()
        assert isinstance(stats, tuple)
        assert len(stats) == 2
        assert stats[0] == 0
        assert stats[1] == 0


@pytest.mark.skipif(DLL_PATH is None, reason=SKIP_REASON)
class TestSliceLoadingStructures:
    """スライス読み込み構造体のテスト"""
    
    def test_slice_config_structure_exists(self):
        """YtdlpSpoutSliceConfig 構造体が存在することを確認"""
        from python.ytdlpspout_native import YtdlpSpoutSliceConfig
        
        config = YtdlpSpoutSliceConfig()
        assert hasattr(config, 'enabled')
        assert hasattr(config, 'chunkSize')
        assert hasattr(config, 'maxCacheMemory')
        assert hasattr(config, 'maxConcurrentDownloads')
        assert hasattr(config, 'prefetchChunksAhead')
        assert hasattr(config, 'criticalChunksAhead')
        assert hasattr(config, 'enableContinuousDownload')
        assert hasattr(config, 'cachePath')
    
    def test_ytdlp_config_structure_exists(self):
        """YtdlpSpoutYtDlpConfig 構造体が存在することを確認"""
        from python.ytdlpspout_native import YtdlpSpoutYtDlpConfig
        
        config = YtdlpSpoutYtDlpConfig()
        assert hasattr(config, 'path')
        assert hasattr(config, 'preferredHeight')
    
    def test_config_ex_structure_exists(self):
        """YtdlpSpoutConfigEx 構造体が存在することを確認"""
        from python.ytdlpspout_native import YtdlpSpoutConfigEx
        
        config = YtdlpSpoutConfigEx()
        assert hasattr(config, 'source')
        assert hasattr(config, 'senderName')
        assert hasattr(config, 'outputWidth')
        assert hasattr(config, 'outputHeight')
        assert hasattr(config, 'loop')
        assert hasattr(config, 'useHardwareAccel')
        assert hasattr(config, 'verbose')
        assert hasattr(config, 'slice')
        assert hasattr(config, 'ytdlp')


# スタンドアロン実行用
if __name__ == "__main__":
    pytest.main([__file__, "-v"])
