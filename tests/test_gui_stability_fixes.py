"""
test_gui_stability_fixes.py - gui.pyの停止処理・監視スレッド・プレビュー処理の安定性テスト

対象:
- PY-4: on_stop()がURL解決中のYtDlpAsyncResolverをキャンセルすること
- PY-5: メインスレッド監視スレッドがon_stop()で停止すること
- PY-6: プレビュー更新が常駐ワーカースレッド＋最新フレームのみ保持のキューで処理されること

gui.pyはtkinterウィジェットに依存するため、実ウィジェットを生成せず、
必要な属性/メソッドのみを持つフェイクのself（duck typing）を使い、
gui.Appの実メソッドを直接（アンバインドで）呼び出して検証する。
"""

import queue
import sys
import threading
import time
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import Mock

import pytest

# プロジェクトルートをパスに追加
project_root = Path(__file__).parent.parent
sys.path.insert(0, str(project_root))

import gui  # noqa: E402


class _FakeRoot:
    """tkinter rootの代替。after()はメインループなしで即座に実行する"""

    def after(self, delay, fn=None, *args):
        if fn is None:
            return
        if args:
            fn(*args)
        else:
            fn()


class _FakeAppForStop:
    """gui.App.on_stop()が参照する属性/メソッドのみを備えたフェイクself"""

    def __init__(self):
        self.log = Mock()
        self._cancel_download = Mock()
        self.streamer = None
        self.hide_download_progress = Mock()
        self.main_thread_monitor_active = True
        self._stop_main_thread_monitor = Mock()
        self.spout_sender = None
        self.btn_start = Mock()
        self.btn_stop = Mock()
        self.info_label = Mock()
        self.seek_slider = Mock()
        self.time_label = Mock()
        self.update_seekbar_color = Mock()
        self._delete_local_file_with_retry = Mock()
        self._url_resolving = True
        self._url_resolve_generation = 0
        self._ytdlp_resolver = Mock()
        self._stream_start_generation = 0
        self._stream_lock = threading.Lock()


class TestOnStopCancelsYtDlpResolution:
    """PY-4: on_stop()がURL解決中のリゾルバーをキャンセルする"""

    def test_on_stop_cancels_resolver_and_resets_flag(self):
        app = _FakeAppForStop()

        gui.App.on_stop(app, delete_local_file=False)

        app._ytdlp_resolver.cancel.assert_called_once()
        assert app._url_resolving is False

    def test_on_stop_bumps_generation_to_invalidate_pending_closures(self):
        """世代カウンタが進み、Stop前に発行されたpoll_resolution/on_resolve_completeの
        クロージャが自身の世代と比較して早期returnできるようになる"""
        app = _FakeAppForStop()
        generation_before = app._url_resolve_generation

        gui.App.on_stop(app, delete_local_file=False)

        assert app._url_resolve_generation != generation_before

    def test_on_stop_works_when_no_resolver_pending(self):
        """URL解決中でない場合（_ytdlp_resolver=None）でもエラーにならない"""
        app = _FakeAppForStop()
        app._ytdlp_resolver = None

        # 例外が発生しないことを確認
        gui.App.on_stop(app, delete_local_file=False)
        assert app._url_resolving is False

    def test_on_stop_stops_main_thread_monitor_when_active(self):
        """PY-5: 監視スレッドがアクティブなら on_stop() で停止処理が呼ばれる"""
        app = _FakeAppForStop()
        app.main_thread_monitor_active = True

        gui.App.on_stop(app, delete_local_file=False)

        app._stop_main_thread_monitor.assert_called_once()

    def test_on_stop_skips_monitor_stop_when_already_inactive(self):
        """既に停止済みなら重複して停止処理を呼ばない"""
        app = _FakeAppForStop()
        app.main_thread_monitor_active = False

        gui.App.on_stop(app, delete_local_file=False)

        app._stop_main_thread_monitor.assert_not_called()

    def test_on_stop_bumps_stream_start_generation(self):
        """Issue GUI-2: on_stop()がストリーム開始世代カウンタ(_stream_start_generation)を
        進める。これにより、開始処理中（Streamer構築中）のバックグラウンドスレッドが
        後から完了しても、世代不一致で自動的に破棄されるようになる"""
        app = _FakeAppForStop()
        generation_before = app._stream_start_generation

        gui.App.on_stop(app, delete_local_file=False)

        assert app._stream_start_generation != generation_before


class TestMainThreadMonitorLifecycle:
    """PY-5: メインスレッド監視スレッドが停止指示で実際にループを終了する"""

    def test_stop_main_thread_monitor_terminates_background_thread(self):
        class _FakeApp:
            def __init__(self):
                self.root = _FakeRoot()
                self.main_thread_monitor_active = False
                self.main_thread_monitor_thread = None
                self._last_heartbeat_time = time.time()

            def log(self, msg):
                pass

            def _main_thread_heartbeat(self):
                self._last_heartbeat_time = time.time()

            _start_main_thread_monitor = gui.App._start_main_thread_monitor
            _stop_main_thread_monitor = gui.App._stop_main_thread_monitor

        app = _FakeApp()
        app._start_main_thread_monitor()

        # スレッドが起動していることを確認
        time.sleep(0.05)
        thread_ref = app.main_thread_monitor_thread
        assert thread_ref is not None
        assert thread_ref.is_alive()

        # 停止を指示
        app._stop_main_thread_monitor()
        assert app.main_thread_monitor_active is False
        assert app.main_thread_monitor_thread is None

        # ループ内のsleep(0.2)消化後、実際にスレッドが終了することを確認
        thread_ref.join(timeout=2.0)
        assert not thread_ref.is_alive(), "停止フラグを立てても監視スレッドが終了しない"


class _FakeAppForStartError:
    """gui.App._handle_start_error()が参照する属性/メソッドのみを備えたフェイクself"""

    def __init__(self):
        self.log = Mock()
        self.streamer = Mock()
        self.hide_download_progress = Mock()
        self.main_thread_monitor_active = True
        self._stop_main_thread_monitor = Mock()
        self.download_in_progress = True
        self.btn_start = Mock()
        self.btn_stop = Mock()
        self.info_label = Mock()
        self.update_seekbar_color = Mock()


class TestHandleStartErrorStopsMainThreadMonitor:
    """Issue G: 開始失敗経路（_handle_start_error）でも監視スレッドが停止される"""

    def test_handle_start_error_stops_monitor_when_active(self):
        app = _FakeAppForStartError()
        app.main_thread_monitor_active = True

        gui.App._handle_start_error(app, "テストエラー")

        app._stop_main_thread_monitor.assert_called_once()

    def test_handle_start_error_skips_monitor_stop_when_already_inactive(self):
        """既に停止済みなら重複して停止処理を呼ばない（既存ガードの再利用を確認）"""
        app = _FakeAppForStartError()
        app.main_thread_monitor_active = False

        gui.App._handle_start_error(app, "テストエラー")

        app._stop_main_thread_monitor.assert_not_called()

    def test_handle_start_error_still_stops_streamer_and_resets_ui(self):
        """監視スレッド停止の追加により、既存の後始末処理が壊れていないことを確認"""
        app = _FakeAppForStartError()
        streamer_mock = app.streamer

        gui.App._handle_start_error(app, "テストエラー")

        streamer_mock.stop.assert_called_once()
        assert app.streamer is None
        assert app.download_in_progress is False
        app.btn_start.configure.assert_called_once_with(state="normal")
        app.btn_stop.configure.assert_called_once_with(state="disabled")


class _FakeAppForLocalFileStreamError:
    """gui.App._start_local_file_stream()がStreamer構築失敗時に
    _handle_start_error()へ委譲することを検証するためのフェイクself"""

    def __init__(self):
        self.log = Mock()
        self.streamer = None
        self.sender_var = Mock(get=Mock(return_value="TestSender"))
        self.vod_loop = Mock(get=Mock(return_value=False))
        self._log_direct = Mock()
        self.on_stream_start_success = Mock()
        self.on_auto_stop = Mock()
        self._handle_start_error = Mock()
        self.update_seekbar_color = Mock()
        self._create_streamer = Mock(side_effect=RuntimeError("構築失敗"))


class TestStartLocalFileStreamErrorRecovery:
    """Issue GUI-1: ローカルファイルStreamer構築失敗時、self.streamerが未代入(None)の
    ままon_auto_stop()を呼ぶとボタンが復帰しないため、他の開始失敗経路と同じ
    _handle_start_error()に統一する"""

    def test_uses_handle_start_error_not_on_auto_stop(self):
        app = _FakeAppForLocalFileStreamError()

        gui.App._start_local_file_stream(app, "C:/dummy/video.mp4")

        app._handle_start_error.assert_called_once()
        app.on_auto_stop.assert_not_called()


class _FakeAppForLocalFileStreamRecovery:
    """_start_local_file_stream()と実際の_handle_start_error()を組み合わせ、
    Streamer構築失敗時にStart/Stopボタンが実際に正しい状態へ復帰することを
    確認するためのフェイクself"""

    def __init__(self):
        self.log = Mock()
        self.streamer = None
        self.sender_var = Mock(get=Mock(return_value="TestSender"))
        self.vod_loop = Mock(get=Mock(return_value=False))
        self._log_direct = Mock()
        self.on_stream_start_success = Mock()
        self.on_auto_stop = Mock()
        self.update_seekbar_color = Mock()
        self.hide_download_progress = Mock()
        self.main_thread_monitor_active = True
        self._stop_main_thread_monitor = Mock()
        self.download_in_progress = False
        self.btn_start = Mock()
        self.btn_stop = Mock()
        self.info_label = Mock()
        self._create_streamer = Mock(side_effect=RuntimeError("構築失敗"))

    _handle_start_error = gui.App._handle_start_error


class TestStartLocalFileStreamButtonsRecoverOnFailure:
    """Issue GUI-1受け入れ条件: 構築失敗後にbtn_startがnormal・btn_stopがdisabledに
    戻り、main_thread_monitorも停止すること"""

    def test_buttons_and_monitor_reset_after_construction_failure(self):
        app = _FakeAppForLocalFileStreamRecovery()

        gui.App._start_local_file_stream(app, "C:/dummy/video.mp4")

        app.on_auto_stop.assert_not_called()
        app._stop_main_thread_monitor.assert_called_once()
        app.btn_start.configure.assert_called_once_with(state="normal")
        app.btn_stop.configure.assert_called_once_with(state="disabled")
        assert app.streamer is None


class _FakeAppForCommitStreamer:
    """gui.App._commit_started_streamer()の検証用フェイクself"""

    def __init__(self, generation: int = 0):
        self.log = Mock()
        self.streamer = None
        self._stream_start_generation = generation
        self._stream_lock = threading.Lock()

    _commit_started_streamer = gui.App._commit_started_streamer


class TestCommitStartedStreamer:
    """Issue GUI-2: Start処理中(バックグラウンドでのStreamer構築中)にStopが
    呼ばれ世代が進んだ場合、後から構築完了したStreamerを自動的に破棄し、
    self.streamerへの代入・start()を行わないことで孤立ストリームを防ぐ"""

    def test_commits_streamer_when_generation_matches(self):
        """Stopが挟まれなければ従来通り採用・start()される"""
        app = _FakeAppForCommitStreamer(generation=0)
        streamer = Mock()
        on_committed = Mock()

        result = gui.App._commit_started_streamer(app, streamer, 0, on_committed=on_committed)

        assert result is True
        assert app.streamer is streamer
        streamer.start.assert_called_once()
        streamer.stop.assert_not_called()
        on_committed.assert_called_once()

    def test_discards_streamer_when_stop_bumped_generation(self):
        """構築完了前にStopが押され世代が進んでいた場合、構築済みStreamerは
        即座にstop()され、self.streamerには代入されない"""
        app = _FakeAppForCommitStreamer(generation=0)
        captured_generation = app._stream_start_generation  # Startクリック時点の世代

        # Stop相当の操作：世代を進める（on_stop()内で行われるのと同じ）
        app._stream_start_generation += 1

        streamer = Mock()
        on_committed = Mock()

        result = gui.App._commit_started_streamer(app, streamer, captured_generation, on_committed=on_committed)

        assert result is False
        assert app.streamer is None
        streamer.start.assert_not_called()
        streamer.stop.assert_called_once()
        on_committed.assert_not_called()


class TestMainThreadMonitorNotStoppedOnSuccessfulStart:
    """正常開始→Stopの既存動作に影響がないことの確認（回帰防止）"""

    def test_on_stop_still_stops_monitor_after_successful_start_flow(self):
        """on_stream()相当（監視開始）→on_stop()相当（監視停止）が従来通り動作する"""
        app = _FakeAppForStop()
        app.main_thread_monitor_active = True

        gui.App.on_stop(app, delete_local_file=False)

        app._stop_main_thread_monitor.assert_called_once()


class _FakeRootWithState:
    """tkinter rootの代替。state()でウィンドウの最小化状態を模擬する"""

    def __init__(self, initial_state: str = "normal"):
        self._state = initial_state

    def state(self):
        return self._state


class _FakeRootStateRaises:
    """state()呼び出しが例外を投げるrootの代替（判定不能ケースの検証用）"""

    def state(self):
        raise RuntimeError("state() unavailable")


class _LegacyStreamerWithoutPreviewEnabled:
    """preview_enabled属性を持たない旧来のStreamer相当（後方互換確認用）"""
    pass


class _FakeAppForPreviewSync:
    """gui.App._sync_streamer_preview_enabled()が参照する属性のみを持つフェイクself"""

    def __init__(self):
        self.root = _FakeRootWithState("normal")
        self.streamer = Mock()
        self.streamer.preview_enabled = True

    _sync_streamer_preview_enabled = gui.App._sync_streamer_preview_enabled


class TestSyncStreamerPreviewEnabledOnMinimize:
    """Issue F-3: ウィンドウ最小化状態をstreamer.preview_enabledに連動させる"""

    def test_sets_preview_disabled_when_window_minimized(self):
        app = _FakeAppForPreviewSync()
        app.root._state = "iconic"

        app._sync_streamer_preview_enabled()

        assert app.streamer.preview_enabled is False

    def test_restores_preview_enabled_when_window_restored(self):
        app = _FakeAppForPreviewSync()
        app.root._state = "iconic"
        app._sync_streamer_preview_enabled()
        assert app.streamer.preview_enabled is False

        app.root._state = "normal"
        app._sync_streamer_preview_enabled()

        assert app.streamer.preview_enabled is True

    def test_noop_when_no_streamer(self):
        app = _FakeAppForPreviewSync()
        app.streamer = None
        app.root._state = "iconic"

        # 例外が発生しないことを確認
        app._sync_streamer_preview_enabled()

    def test_noop_when_streamer_lacks_preview_enabled_attribute(self):
        """preview_enabledを持たない旧来のStreamerでも例外を投げない"""
        app = _FakeAppForPreviewSync()
        app.streamer = _LegacyStreamerWithoutPreviewEnabled()
        app.root._state = "iconic"

        app._sync_streamer_preview_enabled()

        assert not hasattr(app.streamer, "preview_enabled")

    def test_noop_when_root_state_unavailable(self):
        """root.state()が例外を投げる環境でも安全側（変更なし）に倒れる"""
        app = _FakeAppForPreviewSync()
        app.root = _FakeRootStateRaises()

        # 例外が発生しないことを確認
        app._sync_streamer_preview_enabled()
        assert app.streamer.preview_enabled is True


class _FakeAppForPreview:
    """gui.Appのプレビュー関連メソッドが参照する属性のみを持つフェイクself"""

    def __init__(self):
        self._preview_frame_queue: queue.Queue = queue.Queue(maxsize=1)
        self._preview_worker_thread = None
        self.root = _FakeRoot()
        self.debug_log = Mock()
        self._finalize_preview_update = Mock()

    _start_preview_worker = gui.App._start_preview_worker
    _update_preview_frame_async = gui.App._update_preview_frame_async


class TestPreviewWorkerQueue:
    """PY-6: プレビュー更新が常駐ワーカー＋最新フレームのみのキューで処理される"""

    def test_update_preview_frame_async_keeps_only_latest_frame(self):
        """ワーカーが未起動でキューが詰まっている状況でも、古いフレームは破棄される"""
        app = _FakeAppForPreview()
        # ワーカーが消費する前に2回積んでも、キューには最新の1件のみが残る
        app._update_preview_frame_async("frame1", 1.0)
        app._update_preview_frame_async("frame2", 2.0)

        assert app._preview_frame_queue.qsize() == 1
        item = app._preview_frame_queue.get_nowait()
        assert item == ("frame2", 2.0)

    def test_no_thread_spawned_per_call(self):
        """フレーム更新のたびにThreadを生成しない（キューへの投入のみ）"""
        app = _FakeAppForPreview()
        original_thread = gui.threading.Thread
        gui.threading.Thread = Mock(side_effect=AssertionError("Threadが生成された"))
        try:
            app._update_preview_frame_async("frame1", 1.0)
        finally:
            gui.threading.Thread = original_thread

    def test_preview_worker_processes_frame_and_calls_finalize(self):
        """常駐ワーカースレッドがキューのフレームを処理し、finalizeをroot.after経由で呼ぶ"""
        import numpy as np

        app = _FakeAppForPreview()
        app._start_preview_worker()

        assert app._preview_worker_thread is not None
        assert app._preview_worker_thread.is_alive()

        frame = np.zeros((4, 4, 3), dtype="uint8")
        app._update_preview_frame_async(frame, 42.0)

        for _ in range(50):
            if app._finalize_preview_update.called:
                break
            time.sleep(0.02)

        assert app._finalize_preview_update.called
        call_args = app._finalize_preview_update.call_args
        rgb_arg, start_time_arg = call_args[0]
        assert rgb_arg.shape == frame.shape
        assert start_time_arg == 42.0


class _FakeAppForResolutionSettings:
    """gui.App._get_resolution_settings()の検証用フェイクself"""

    def __init__(self, *, perf_limit=False, max_enable=False, manual_enable=False,
                 maxw="", maxh="", manw="", manh=""):
        self.log = Mock()
        self.perf_limit = Mock(get=Mock(return_value=perf_limit))
        self.max_enable = Mock(get=Mock(return_value=max_enable))
        self.manual_enable = Mock(get=Mock(return_value=manual_enable))
        self.maxw_var = Mock(get=Mock(return_value=maxw))
        self.maxh_var = Mock(get=Mock(return_value=maxh))
        self.manw_var = Mock(get=Mock(return_value=manw))
        self.manh_var = Mock(get=Mock(return_value=manh))

    _get_resolution_settings = gui.App._get_resolution_settings


class TestResolutionSettingsInvalidValueWarning:
    """Issue GUI-3: 解像度欄への不正値入力時に無警告でCap/Manual設定が
    無効化されるのを防ぐため、self.log()で明確な警告を出す"""

    def test_logs_warning_on_invalid_max_resolution(self):
        app = _FakeAppForResolutionSettings(max_enable=True, maxw="abc", maxh="1080")

        max_res, _ = gui.App._get_resolution_settings(app)

        assert max_res is None
        app.log.assert_called_once()
        assert "警告" in app.log.call_args[0][0]

    def test_logs_warning_on_invalid_manual_resolution(self):
        app = _FakeAppForResolutionSettings(manual_enable=True, manw="1920", manh="xyz")

        _, manual_res = gui.App._get_resolution_settings(app)

        assert manual_res is None
        app.log.assert_called_once()
        assert "警告" in app.log.call_args[0][0]

    def test_no_warning_on_valid_max_resolution(self):
        app = _FakeAppForResolutionSettings(max_enable=True, maxw="1920", maxh="1080")

        max_res, _ = gui.App._get_resolution_settings(app)

        assert max_res == (1920, 1080)
        app.log.assert_not_called()

    def test_no_warning_when_fields_left_blank(self):
        """Cap/Manualが有効でも欄が空欄なだけなら（不正値ではないので）警告しない"""
        app = _FakeAppForResolutionSettings(max_enable=True, maxw="", maxh="")

        max_res, _ = gui.App._get_resolution_settings(app)

        assert max_res is None
        app.log.assert_not_called()


class _FakeHeadlessAppForResolution:
    """gui.HeadlessApp._get_max_resolution()/_get_manual_resolution()の検証用フェイクself"""

    def __init__(self, **kwargs):
        args_defaults = dict(
            max_width=None, max_height=None, no_limit=True, verbose=False,
            width=None, height=None,
        )
        args_defaults.update(kwargs)
        self.args = SimpleNamespace(**args_defaults)
        self.log = Mock()

    _get_max_resolution = gui.HeadlessApp._get_max_resolution
    _get_manual_resolution = gui.HeadlessApp._get_manual_resolution


class TestHeadlessResolutionPartialArgsWarning:
    """Issue GUI-4: ヘッドレスCLIで幅・高さの片方だけ指定した場合に
    無警告で無視せず、両方必要である旨を警告する"""

    def test_warns_when_only_max_width_given(self):
        app = _FakeHeadlessAppForResolution(max_width=1920)

        result = app._get_max_resolution()

        assert result is None
        app.log.assert_called_once()
        assert "警告" in app.log.call_args[0][0]

    def test_warns_when_only_max_height_given(self):
        app = _FakeHeadlessAppForResolution(max_height=1080)

        result = app._get_max_resolution()

        assert result is None
        app.log.assert_called_once()
        assert "警告" in app.log.call_args[0][0]

    def test_no_warning_when_both_max_dims_given(self):
        app = _FakeHeadlessAppForResolution(max_width=1920, max_height=1080)

        result = app._get_max_resolution()

        assert result == (1920, 1080)
        app.log.assert_not_called()

    def test_no_warning_when_neither_max_dim_given(self):
        """従来通り：両方未指定ならデフォルトの1080p制限が無警告で適用される"""
        app = _FakeHeadlessAppForResolution(no_limit=False)

        result = app._get_max_resolution()

        assert result == (1920, 1080)
        app.log.assert_not_called()

    def test_warns_when_only_width_given_for_manual_resolution(self):
        app = _FakeHeadlessAppForResolution(width=1280)

        result = app._get_manual_resolution()

        assert result is None
        app.log.assert_called_once()
        assert "警告" in app.log.call_args[0][0]

    def test_no_warning_when_both_manual_dims_given(self):
        app = _FakeHeadlessAppForResolution(width=1280, height=720)

        result = app._get_manual_resolution()

        assert result == (1280, 720)
        app.log.assert_not_called()

    def test_no_warning_when_neither_manual_dim_given(self):
        app = _FakeHeadlessAppForResolution()

        result = app._get_manual_resolution()

        assert result is None
        app.log.assert_not_called()


class _NativeLikeStreamer:
    """NativeStreamerWrapper相当（self._thread）のみを持つダミー"""

    def __init__(self, thread_alive: bool):
        self._thread = Mock(is_alive=Mock(return_value=thread_alive))


class _LegacyLikeStreamer:
    """レガシーStreamer相当（self.thread）のみを持つダミー"""

    def __init__(self, thread_alive: bool):
        self.thread = Mock(is_alive=Mock(return_value=thread_alive))


class _StopEventOnlyStreamer:
    """threadを持たずstop_eventのみを持つダミー（フォールバック判定確認用）"""

    def __init__(self):
        self._stop_event = threading.Event()


class TestHeadlessIsStreamerAlive:
    """Issue GUI-5: 存在しないis_running属性ではなく、NativeStreamerWrapper/
    レガシーStreamerの両方に実在する属性(_thread/thread, _stop_event/stop_event)
    で終了検知を行う"""

    def test_true_when_native_thread_alive(self):
        assert gui.HeadlessApp._is_streamer_alive(_NativeLikeStreamer(True)) is True

    def test_false_when_native_thread_dead(self):
        assert gui.HeadlessApp._is_streamer_alive(_NativeLikeStreamer(False)) is False

    def test_true_when_legacy_thread_alive(self):
        assert gui.HeadlessApp._is_streamer_alive(_LegacyLikeStreamer(True)) is True

    def test_false_when_legacy_thread_dead(self):
        assert gui.HeadlessApp._is_streamer_alive(_LegacyLikeStreamer(False)) is False

    def test_uses_stop_event_when_no_thread_attribute(self):
        streamer = _StopEventOnlyStreamer()
        assert gui.HeadlessApp._is_streamer_alive(streamer) is True

        streamer._stop_event.set()
        assert gui.HeadlessApp._is_streamer_alive(streamer) is False

    def test_none_streamer_is_not_alive(self):
        assert gui.HeadlessApp._is_streamer_alive(None) is False


if __name__ == "__main__":
    pytest.main([__file__, "-v"])
