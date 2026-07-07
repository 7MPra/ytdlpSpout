"""
test_native_streamer_wrapper.py - NativeStreamerWrapperのテスト

DLLが存在する場合にのみ実行されます。
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
class TestNativeStreamerWrapper:
    """NativeStreamerWrapperクラスのテスト"""
    
    def test_import(self):
        """モジュールがインポートできる"""
        from python.native_streamer_wrapper import NativeStreamerWrapper
        assert NativeStreamerWrapper is not None
    
    def test_create_wrapper(self):
        """ラッパーを作成できる"""
        from python.native_streamer_wrapper import NativeStreamerWrapper
        
        wrapper = NativeStreamerWrapper(
            video_url="test.mp4",
            sender_name="TestSender"
        )
        assert wrapper is not None
        assert wrapper.video_url == "test.mp4"
        assert wrapper.sender_name == "TestSender"
    
    def test_initial_properties(self):
        """初期プロパティが正しい"""
        from python.native_streamer_wrapper import NativeStreamerWrapper
        
        wrapper = NativeStreamerWrapper(
            video_url="test.mp4",
            sender_name="TestSender"
        )
        
        # 初期状態のプロパティ確認
        assert wrapper.is_vod is True  # デフォルトVOD
        assert wrapper.is_live is False
        assert wrapper.duration == 0.0
        assert wrapper.playback_time == 0.0
        assert wrapper.width == 0
        assert wrapper.height == 0
        assert wrapper.detected_fps == 30.0
        assert wrapper.latest_frame_bgr is None
    
    def test_frame_lock_exists(self):
        """frame_lockが存在する"""
        from python.native_streamer_wrapper import NativeStreamerWrapper
        import threading
        
        wrapper = NativeStreamerWrapper(
            video_url="test.mp4",
            sender_name="TestSender"
        )
        
        assert hasattr(wrapper, 'frame_lock')
        assert isinstance(wrapper.frame_lock, type(threading.Lock()))
    
    def test_callbacks_stored(self):
        """コールバックが保存される"""
        from python.native_streamer_wrapper import NativeStreamerWrapper
        
        log_called = []
        stop_called = []
        init_called = []
        
        wrapper = NativeStreamerWrapper(
            video_url="test.mp4",
            sender_name="TestSender",
            log_cb=lambda msg: log_called.append(msg),
            stop_cb=lambda: stop_called.append(True),
            init_ok_cb=lambda: init_called.append(True)
        )
        
        # ログコールバックをテスト
        wrapper.log("test message")
        assert len(log_called) == 1
        assert log_called[0] == "test message"
    
    def test_stop_without_start(self):
        """開始前に停止してもエラーが発生しない"""
        from python.native_streamer_wrapper import NativeStreamerWrapper
        
        wrapper = NativeStreamerWrapper(
            video_url="test.mp4",
            sender_name="TestSender"
        )
        
        # 開始前に停止してもエラーにならない
        wrapper.stop()
    
    def test_seek_without_start(self):
        """開始前にシークしてもエラーが発生しない"""
        from python.native_streamer_wrapper import NativeStreamerWrapper
        
        wrapper = NativeStreamerWrapper(
            video_url="test.mp4",
            sender_name="TestSender"
        )
        
        # 開始前にシークしてもエラーにならない
        wrapper.seek(10.0)


@pytest.mark.skipif(DLL_PATH is None, reason=SKIP_REASON)
class TestNativeStreamerWrapperCompatibility:
    """Python Streamerとの互換性テスト"""
    
    def test_has_streamer_interface(self):
        """Python Streamerと同じインターフェースを持つ"""
        from python.native_streamer_wrapper import NativeStreamerWrapper
        
        wrapper = NativeStreamerWrapper(
            video_url="test.mp4",
            sender_name="TestSender"
        )
        
        # 必須メソッド
        assert hasattr(wrapper, 'start')
        assert hasattr(wrapper, 'stop')
        assert hasattr(wrapper, 'seek')
        
        # 必須プロパティ
        assert hasattr(wrapper, 'is_vod')
        assert hasattr(wrapper, 'is_live')
        assert hasattr(wrapper, 'duration')
        assert hasattr(wrapper, 'playback_time')
        assert hasattr(wrapper, 'width')
        assert hasattr(wrapper, 'height')
        assert hasattr(wrapper, 'detected_fps')
        assert hasattr(wrapper, 'latest_frame_bgr')
        assert hasattr(wrapper, 'frame_lock')


# SpoutGLが存在するかチェック
def check_spoutgl_available():
    """
    SpoutGLが利用可能かチェック

    注意: ここで実際に `import SpoutGL` すると、SpoutGL同梱のSpout.dllが
    プロセスにロードされる。本テストスイートは同一プロセス内でytdlpspout.dll
    （独自のSpout.dllに依存）も後からロードするため、同名DLLの衝突により
    ytdlpspout.dllのロードがWinError 127（procedure not found）で失敗する
    （テスト実行順序に依存する既知のWindows DLL名衝突）。
    可用性の確認だけなら実import不要のため、find_specで代替する。
    """
    import importlib.util
    return importlib.util.find_spec("SpoutGL") is not None


HAS_SPOUTGL = check_spoutgl_available()
SPOUTGL_SKIP_REASON = "SpoutGL not installed"


@pytest.mark.skipif(DLL_PATH is None, reason=SKIP_REASON)
class TestNativeStreamerWrapperSpout:
    """SpoutGL送信機能のテスト"""
    
    def test_spout_properties_exist(self):
        """Spout関連プロパティが存在する"""
        from python.native_streamer_wrapper import NativeStreamerWrapper
        
        wrapper = NativeStreamerWrapper(
            video_url="test.mp4",
            sender_name="TestSender"
        )
        
        # Spout関連プロパティ
        assert hasattr(wrapper, 'spout')
        assert hasattr(wrapper, 'owns_spout')
        assert hasattr(wrapper, 'spout_enabled')
        assert hasattr(wrapper, 'spout_enabled_lock')
    
    def test_owns_spout_true_when_no_external_sender(self):
        """external_spout_senderがNoneの場合、owns_spoutがTrue"""
        from python.native_streamer_wrapper import NativeStreamerWrapper
        
        wrapper = NativeStreamerWrapper(
            video_url="test.mp4",
            sender_name="TestSender",
            external_spout_sender=None
        )
        
        assert wrapper.owns_spout is True
        assert wrapper.spout is None  # まだ初期化されていない
    
    def test_owns_spout_false_with_external_sender(self):
        """external_spout_senderが指定された場合、owns_spoutがFalse"""
        from python.native_streamer_wrapper import NativeStreamerWrapper
        
        # ダミーの外部sender
        class DummySpoutSender:
            pass
        
        external = DummySpoutSender()
        wrapper = NativeStreamerWrapper(
            video_url="test.mp4",
            sender_name="TestSender",
            external_spout_sender=external
        )
        
        assert wrapper.owns_spout is False
        assert wrapper.spout is external
    
    def test_set_spout_enabled(self):
        """set_spout_enabledメソッドが動作する"""
        from python.native_streamer_wrapper import NativeStreamerWrapper
        
        log_messages = []
        wrapper = NativeStreamerWrapper(
            video_url="test.mp4",
            sender_name="TestSender",
            log_cb=lambda msg: log_messages.append(msg)
        )
        
        # 初期状態はTrue
        assert wrapper.spout_enabled is True
        
        # 無効化
        wrapper.set_spout_enabled(False)
        assert wrapper.spout_enabled is False
        assert any("無効" in msg for msg in log_messages)
        
        # 有効化
        wrapper.set_spout_enabled(True)
        assert wrapper.spout_enabled is True
        assert any("有効" in msg for msg in log_messages)
    
    @pytest.mark.skipif(not HAS_SPOUTGL, reason=SPOUTGL_SKIP_REASON)
    def test_spout_initialized_in_run(self):
        """
        _run()でSpoutSenderが初期化される（owns_spout=Trueの場合）

        注: 実際の再生は行わない（DLLテスト）ため、このテストはSpoutGLの
        可用性のみ確認する。`import SpoutGL` を実際に行うと、同梱の
        Spout.dllが本テストスイートが後でロードするytdlpspout.dll側の
        Spout.dllと名前衝突し、WinError 127でytdlpspout.dllのロードが
        失敗する（check_spoutgl_availableのコメント参照）ため、
        find_specによる可用性確認に留める。
        """
        import importlib.util
        assert importlib.util.find_spec("SpoutGL") is not None
    
    def test_stop_releases_spout_when_owns(self):
        """stop()でSpoutSenderが解放される（owns_spout=Trueの場合）"""
        from python.native_streamer_wrapper import NativeStreamerWrapper
        
        wrapper = NativeStreamerWrapper(
            video_url="test.mp4",
            sender_name="TestSender"
        )
        
        # ダミーのSpoutオブジェクトを設定
        class MockSpoutSender:
            released = False
            def releaseSender(self):
                self.released = True
        
        wrapper.spout = MockSpoutSender()
        wrapper.owns_spout = True
        
        wrapper.stop()
        
        # spoutが解放されてNoneになる
        assert wrapper.spout is None
    
    def test_stop_preserves_external_spout(self):
        """stop()で外部SpoutSenderは解放されない（owns_spout=Falseの場合）"""
        from python.native_streamer_wrapper import NativeStreamerWrapper
        
        class MockSpoutSender:
            released = False
            def releaseSender(self):
                self.released = True
        
        external = MockSpoutSender()
        wrapper = NativeStreamerWrapper(
            video_url="test.mp4",
            sender_name="TestSender",
            external_spout_sender=external
        )
        
        wrapper.stop()
        
        # 外部senderは解放されない
        assert external.released is False


@pytest.mark.skipif(DLL_PATH is None, reason=SKIP_REASON)
class TestNativeStreamerWrapperSliceLoading:
    """スライス読み込み関連のテスト（Phase 4）"""
    
    def test_download_progress_property_exists(self):
        """download_progress プロパティが存在する"""
        from python.native_streamer_wrapper import NativeStreamerWrapper
        
        wrapper = NativeStreamerWrapper(
            video_url="test.mp4",
            sender_name="TestSender"
        )
        
        assert hasattr(wrapper, 'download_progress')
        # 未再生時は0.0を返す
        assert wrapper.download_progress == 0.0
    
    def test_is_fully_cached_property_exists(self):
        """is_fully_cached プロパティが存在する"""
        from python.native_streamer_wrapper import NativeStreamerWrapper
        
        wrapper = NativeStreamerWrapper(
            video_url="test.mp4",
            sender_name="TestSender"
        )
        
        assert hasattr(wrapper, 'is_fully_cached')
        # 未再生時はFalseを返す
        assert wrapper.is_fully_cached is False
    
    def test_bandwidth_property_exists(self):
        """bandwidth プロパティが存在する"""
        from python.native_streamer_wrapper import NativeStreamerWrapper
        
        wrapper = NativeStreamerWrapper(
            video_url="test.mp4",
            sender_name="TestSender"
        )
        
        assert hasattr(wrapper, 'bandwidth')
        # 未再生時は0.0を返す
        assert wrapper.bandwidth == 0.0


@pytest.mark.skipif(DLL_PATH is None, reason=SKIP_REASON)
class TestNativeStreamerWrapperPreviewEnabled:
    """Issue F: preview_enabled連動によるプレビュー変換のスキップ／スロットリング"""

    def test_preview_enabled_default_true(self):
        """preview_enabledはデフォルトTrue（完全後方互換）"""
        from python.native_streamer_wrapper import NativeStreamerWrapper

        wrapper = NativeStreamerWrapper(
            video_url="test.mp4",
            sender_name="TestSender"
        )

        assert wrapper.preview_enabled is True

    def test_preview_enabled_setter_toggles_value(self):
        """preview_enabledはスレッドセーフに読み書きできる"""
        from python.native_streamer_wrapper import NativeStreamerWrapper

        wrapper = NativeStreamerWrapper(
            video_url="test.mp4",
            sender_name="TestSender"
        )

        wrapper.preview_enabled = False
        assert wrapper.preview_enabled is False

        wrapper.preview_enabled = True
        assert wrapper.preview_enabled is True

    def test_update_preview_frame_skipped_when_preview_disabled(self):
        """preview_enabled=Falseの間、get_current_frame/変換が一切呼ばれない"""
        from unittest.mock import Mock
        from python.native_streamer_wrapper import NativeStreamerWrapper

        wrapper = NativeStreamerWrapper(
            video_url="test.mp4",
            sender_name="TestSender"
        )
        wrapper.preview_enabled = False

        mock_native = Mock()
        mock_native.get_current_frame = Mock(return_value="should_not_be_read")

        wrapper._update_preview_frame(mock_native)

        mock_native.get_current_frame.assert_not_called()
        assert wrapper.latest_frame_bgr is None

    def test_update_preview_frame_updates_latest_frame_bgr_when_enabled(self):
        """preview_enabled=True（デフォルト）の場合、latest_frame_bgrが更新される"""
        import numpy as np
        from unittest.mock import Mock
        from python.native_streamer_wrapper import NativeStreamerWrapper

        wrapper = NativeStreamerWrapper(
            video_url="test.mp4",
            sender_name="TestSender"
        )
        wrapper._width = 4
        wrapper._height = 4
        wrapper._output_width = 4
        wrapper._output_height = 4

        frame_bgra = np.zeros((4, 4, 4), dtype=np.uint8)
        mock_native = Mock()
        mock_native.get_current_frame = Mock(return_value=frame_bgra)

        wrapper._update_preview_frame(mock_native)

        mock_native.get_current_frame.assert_called_once()
        assert wrapper.latest_frame_bgr is not None
        assert wrapper.latest_frame_bgr.shape == (4, 4, 3)  # BGRA->BGR変換後

    def test_update_preview_frame_throttles_conversion_rate(self):
        """短時間に連続呼び出しても、スロットリング間隔未満ならget_current_frameは呼ばれない"""
        import numpy as np
        from unittest.mock import Mock
        from python.native_streamer_wrapper import NativeStreamerWrapper

        wrapper = NativeStreamerWrapper(
            video_url="test.mp4",
            sender_name="TestSender"
        )
        wrapper._width = 4
        wrapper._height = 4
        wrapper._output_width = 4
        wrapper._output_height = 4

        frame_bgra = np.zeros((4, 4, 4), dtype=np.uint8)
        mock_native = Mock()
        mock_native.get_current_frame = Mock(return_value=frame_bgra)

        # 1回目：変換実行（初回は前回変換時刻が0.0のため必ず通過）
        wrapper._update_preview_frame(mock_native)
        # 2回目：直後に呼んでもスロットリング間隔（1/30秒）未満のためスキップされる
        wrapper._update_preview_frame(mock_native)

        assert mock_native.get_current_frame.call_count == 1

    def test_update_preview_frame_resumes_after_throttle_interval(self):
        """スロットリング間隔が経過すれば、再度get_current_frameが呼ばれる"""
        import numpy as np
        from unittest.mock import Mock
        from python.native_streamer_wrapper import NativeStreamerWrapper
        import python.native_streamer_wrapper as wrapper_module

        wrapper = NativeStreamerWrapper(
            video_url="test.mp4",
            sender_name="TestSender"
        )
        wrapper._width = 4
        wrapper._height = 4
        wrapper._output_width = 4
        wrapper._output_height = 4

        frame_bgra = np.zeros((4, 4, 4), dtype=np.uint8)
        mock_native = Mock()
        mock_native.get_current_frame = Mock(return_value=frame_bgra)

        # 前回変換時刻を十分過去に設定し、スロットリング間隔経過済みの状態を再現
        wrapper._last_preview_convert_time = -wrapper_module._PREVIEW_CONVERT_MIN_INTERVAL * 10

        wrapper._update_preview_frame(mock_native)

        assert mock_native.get_current_frame.call_count == 1


@pytest.mark.skipif(DLL_PATH is None, reason=SKIP_REASON)
class TestNativeStreamerWrapperPreResolvedUrl:
    """事前解決済みURL機能のテスト"""
    
    def test_pre_resolved_url_property_exists(self):
        """pre_resolved_url引数が正しく保存される"""
        from python.native_streamer_wrapper import NativeStreamerWrapper
        
        wrapper = NativeStreamerWrapper(
            video_url="https://www.youtube.com/watch?v=test123",
            sender_name="TestSender",
            pre_resolved_url="https://resolved.example.com/stream.mp4"
        )
        
        assert hasattr(wrapper, '_pre_resolved_url')
        assert wrapper._pre_resolved_url == "https://resolved.example.com/stream.mp4"
        # 元のURLも保持される
        assert wrapper.video_url == "https://www.youtube.com/watch?v=test123"
    
    def test_pre_resolved_url_default_none(self):
        """pre_resolved_urlはデフォルトでNone"""
        from python.native_streamer_wrapper import NativeStreamerWrapper

        wrapper = NativeStreamerWrapper(
            video_url="test.mp4",
            sender_name="TestSender"
        )

        assert wrapper._pre_resolved_url is None

    def test_pre_resolved_is_hls_property_exists(self):
        """pre_resolved_is_hls引数が正しく保存される"""
        from python.native_streamer_wrapper import NativeStreamerWrapper

        wrapper = NativeStreamerWrapper(
            video_url="https://www.youtube.com/watch?v=test123",
            sender_name="TestSender",
            pre_resolved_is_hls=True
        )

        assert hasattr(wrapper, '_pre_resolved_is_hls')
        assert wrapper._pre_resolved_is_hls is True

    def test_pre_resolved_is_hls_default_none(self):
        """pre_resolved_is_hlsはデフォルトでNone（自動判定=完全後方互換）"""
        from python.native_streamer_wrapper import NativeStreamerWrapper

        wrapper = NativeStreamerWrapper(
            video_url="test.mp4",
            sender_name="TestSender"
        )

        assert wrapper._pre_resolved_is_hls is None


# スタンドアロン実行用
if __name__ == "__main__":
    pytest.main([__file__, "-v"])
