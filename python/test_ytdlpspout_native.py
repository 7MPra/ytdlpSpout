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
        # CMakeビルド出力（Debug優先、開発中はDebugが最新の可能性が高い）
        project_root / "cpp" / "build" / "bin" / "Debug" / "ytdlpspout.dll",
        project_root / "cpp" / "build" / "bin" / "Release" / "ytdlpspout.dll",
        # ローカルpythonフォルダ
        project_root / "python" / "ytdlpspout.dll",
        # レガシーパス（VS2022）
        project_root / "cpp" / "build" / "vs2022" / "bin" / "Release" / "ytdlpspout.dll",
        project_root / "cpp" / "build" / "vs2022" / "bin" / "Debug" / "ytdlpspout.dll",
        # Presetビルドパス
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
class TestHlsCacheStatsAPI:
    """HLSキャッシュ統計API関連のテスト"""
    
    def test_hls_cache_stats_structure_exists(self):
        """YtdlpSpoutHlsCacheStats 構造体が存在することを確認"""
        from python.ytdlpspout_native import YtdlpSpoutHlsCacheStats
        
        stats = YtdlpSpoutHlsCacheStats()
        assert hasattr(stats, 'cachedSegments')
        assert hasattr(stats, 'totalSegments')
        assert hasattr(stats, 'downloadProgress')
        assert hasattr(stats, 'bandwidth')
        assert hasattr(stats, 'isFullyCached')
        assert hasattr(stats, 'isHlsMode')
    
    def test_hls_cache_stats_structure_fields(self):
        """YtdlpSpoutHlsCacheStats 構造体のフィールド型を確認"""
        from python.ytdlpspout_native import YtdlpSpoutHlsCacheStats
        
        stats = YtdlpSpoutHlsCacheStats()
        # 初期値を確認（全てゼロ）
        assert stats.cachedSegments == 0
        assert stats.totalSegments == 0
        assert stats.downloadProgress == 0.0
        assert stats.bandwidth == 0.0
        assert stats.isFullyCached == 0
        assert stats.isHlsMode == 0
    
    def test_get_hls_cache_stats_method_exists(self):
        """get_hls_cache_stats メソッドが存在することを確認"""
        from python.ytdlpspout_native import YtdlpSpoutNative
        
        player = YtdlpSpoutNative(DLL_PATH)
        assert hasattr(player, 'get_hls_cache_stats')
        assert callable(getattr(player, 'get_hls_cache_stats'))
    
    def test_get_hls_cache_stats_without_playback(self):
        """未再生時のget_hls_cache_stats呼び出し"""
        from python.ytdlpspout_native import YtdlpSpoutNative
        
        player = YtdlpSpoutNative(DLL_PATH)
        stats = player.get_hls_cache_stats()
        
        # 未再生時は辞書形式でデフォルト値を返す
        assert stats is not None
        assert isinstance(stats, dict)
        assert 'cached_segments' in stats
        assert 'total_segments' in stats
        assert 'download_progress' in stats
        assert 'bandwidth' in stats
        assert 'is_fully_cached' in stats
        assert 'is_hls_mode' in stats
        
        # デフォルト値の確認
        assert stats['cached_segments'] == 0
        assert stats['total_segments'] == 0
        assert stats['download_progress'] == 0.0
        assert stats['bandwidth'] == 0.0
        assert stats['is_fully_cached'] is False
        assert stats['is_hls_mode'] is False
    
    def test_get_hls_cache_stats_return_types(self):
        """get_hls_cache_stats の戻り値の型を確認"""
        from python.ytdlpspout_native import YtdlpSpoutNative
        
        player = YtdlpSpoutNative(DLL_PATH)
        stats = player.get_hls_cache_stats()
        
        assert isinstance(stats['cached_segments'], int)
        assert isinstance(stats['total_segments'], int)
        assert isinstance(stats['download_progress'], float)
        assert isinstance(stats['bandwidth'], float)
        assert isinstance(stats['is_fully_cached'], bool)
        assert isinstance(stats['is_hls_mode'], bool)


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


@pytest.mark.skipif(DLL_PATH is None, reason=SKIP_REASON)
class TestHlsHintConfig:
    """HLS判定ヒント（FFI拡張、P-1）のテスト"""

    def test_config_ex_has_is_hls_hint_field(self):
        """YtdlpSpoutConfigEx構造体にisHlsHintフィールドが存在する"""
        from python.ytdlpspout_native import YtdlpSpoutConfigEx

        config = YtdlpSpoutConfigEx()
        assert hasattr(config, 'isHlsHint')

    def test_start_ex_accepts_is_hls_argument(self):
        """start_ex()がis_hls引数を受け取れる（デフォルトNone=自動判定で完全後方互換）"""
        from python.ytdlpspout_native import YtdlpSpoutNative
        import inspect

        player = YtdlpSpoutNative(DLL_PATH)
        sig = inspect.signature(player.start_ex)
        assert 'is_hls' in sig.parameters
        assert sig.parameters['is_hls'].default is None

    def _capture_is_hls_hint(self, player):
        """start_exがDLLへ渡すconfig.isHlsHintを横取りするヘルパー

        注: ytdlpspout_start_ex自体はC++側の再生処理まで到達するため、
        DLL呼び出しをモックしてPython側（ytdlpspout_native.py）の
        isHlsHint設定ロジックのみを検証する（C++側の再ビルドは不要）。
        """
        import ctypes
        from python.ytdlpspout_native import YtdlpSpoutConfigEx

        captured = {}

        def fake_start_ex(handle, config_ref):
            ptr = ctypes.cast(config_ref, ctypes.POINTER(YtdlpSpoutConfigEx))
            captured['isHlsHint'] = ptr.contents.isHlsHint
            return 0

        player._lib.ytdlpspout_start_ex = fake_start_ex
        return captured

    def test_start_ex_default_is_hls_none_sets_auto_hint(self):
        """is_hls省略時はC側にisHlsHint=-1（自動判定）を渡す（完全後方互換）"""
        from python.ytdlpspout_native import YtdlpSpoutNative

        player = YtdlpSpoutNative(DLL_PATH)
        captured = self._capture_is_hls_hint(player)
        player.start_ex(source="test.mp4")
        assert captured['isHlsHint'] == -1

    def test_start_ex_is_hls_true_sets_hint(self):
        """is_hls=Trueを指定するとC側にisHlsHint=1を渡す"""
        from python.ytdlpspout_native import YtdlpSpoutNative

        player = YtdlpSpoutNative(DLL_PATH)
        captured = self._capture_is_hls_hint(player)
        player.start_ex(source="test.mp4", is_hls=True)
        assert captured['isHlsHint'] == 1

    def test_start_ex_is_hls_false_sets_hint(self):
        """is_hls=Falseを指定するとC側にisHlsHint=0を渡す"""
        from python.ytdlpspout_native import YtdlpSpoutNative

        player = YtdlpSpoutNative(DLL_PATH)
        captured = self._capture_is_hls_hint(player)
        player.start_ex(source="test.mp4", is_hls=False)
        assert captured['isHlsHint'] == 0


@pytest.mark.skipif(DLL_PATH is None, reason=SKIP_REASON)
class TestStartExSliceDefaults:
    """PLY-2: start_ex()のスライス関連引数が未指定時にC++側の
    チューニング済み既定値（ytdlpspout_config_ex_init()が設定する値）を
    上書きしないことを検証する。
    """

    def _capture_slice_config(self, player):
        """start_exがDLLへ渡すconfig.slice構造体を横取りするヘルパー

        _capture_is_hls_hintと同様、ytdlpspout_start_ex自体をモックして
        Python側（ytdlpspout_native.py）のconfig構築ロジックのみを検証する
        （C++側の再ビルドは不要）。
        """
        import ctypes
        from python.ytdlpspout_native import YtdlpSpoutConfigEx

        captured = {}

        def fake_start_ex(handle, config_ref):
            ptr = ctypes.cast(config_ref, ctypes.POINTER(YtdlpSpoutConfigEx))
            slice_cfg = ptr.contents.slice
            captured['chunkSize'] = slice_cfg.chunkSize
            captured['maxCacheMemory'] = slice_cfg.maxCacheMemory
            captured['maxConcurrentDownloads'] = slice_cfg.maxConcurrentDownloads
            captured['prefetchChunksAhead'] = slice_cfg.prefetchChunksAhead
            captured['criticalChunksAhead'] = slice_cfg.criticalChunksAhead
            captured['enableContinuousDownload'] = slice_cfg.enableContinuousDownload
            return 0

        player._lib.ytdlpspout_start_ex = fake_start_ex
        return captured

    def test_start_ex_signature_defaults_slice_args_to_none(self):
        """start_ex()のスライス関連引数のデフォルトがNone（未指定）である"""
        from python.ytdlpspout_native import YtdlpSpoutNative
        import inspect

        player = YtdlpSpoutNative(DLL_PATH)
        sig = inspect.signature(player.start_ex)
        for name in (
            'chunk_size', 'max_cache_memory',
            'max_concurrent_downloads', 'prefetch_chunks_ahead',
        ):
            assert name in sig.parameters
            assert sig.parameters[name].default is None

    def test_start_ex_unspecified_slice_args_keep_cpp_defaults(self):
        """スライス引数を省略した場合、ytdlpspout_config_ex_init()が設定する
        C++側のチューニング済み既定値（chunkSize=2MB, maxCacheMemory=256MB,
        maxConcurrentDownloads=6, prefetchChunksAhead=24等）が維持される"""
        import ctypes
        from python.ytdlpspout_native import YtdlpSpoutNative, YtdlpSpoutConfigEx

        player = YtdlpSpoutNative(DLL_PATH)

        # config_ex_initが実際に設定する既定値を先に取得（DLL依存の値をハードコードしない）
        default_config = YtdlpSpoutConfigEx()
        player._lib.ytdlpspout_config_ex_init(ctypes.byref(default_config))

        captured = self._capture_slice_config(player)
        player.start_ex(source="test.mp4")

        assert captured['chunkSize'] == default_config.slice.chunkSize
        assert captured['maxCacheMemory'] == default_config.slice.maxCacheMemory
        assert captured['maxConcurrentDownloads'] == default_config.slice.maxConcurrentDownloads
        assert captured['prefetchChunksAhead'] == default_config.slice.prefetchChunksAhead
        assert captured['criticalChunksAhead'] == default_config.slice.criticalChunksAhead
        assert captured['enableContinuousDownload'] == default_config.slice.enableContinuousDownload

    def test_start_ex_explicit_slice_args_override_cpp_defaults(self):
        """スライス引数を明示的に指定した場合、その値がconfigに反映される"""
        from python.ytdlpspout_native import YtdlpSpoutNative

        player = YtdlpSpoutNative(DLL_PATH)
        captured = self._capture_slice_config(player)
        player.start_ex(
            source="test.mp4",
            chunk_size=512 * 1024,
            max_cache_memory=64 * 1024 * 1024,
            max_concurrent_downloads=2,
            prefetch_chunks_ahead=3,
        )

        assert captured['chunkSize'] == 512 * 1024
        assert captured['maxCacheMemory'] == 64 * 1024 * 1024
        assert captured['maxConcurrentDownloads'] == 2
        assert captured['prefetchChunksAhead'] == 3


# スタンドアロン実行用
if __name__ == "__main__":
    pytest.main([__file__, "-v"])
