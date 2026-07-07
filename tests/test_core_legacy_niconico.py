"""
test_core_legacy_niconico.py - レガシー再生経路(ytdlpSpout/core.py)の回帰テスト

C++ DLLバックエンドが使えない環境(USE_NATIVE_BACKEND=False / DLL未ビルド)向けの
フォールバック再生経路である ytdlpSpout/core.py の Streamer に対して、
python/ytdlp_resolver.py 側にのみ入っていた以下の修正が移植されていることを検証する:

- single-format-first のフォーマット選択（ニコニコ系ホストは best 優先）
- ニコニコCDN向け Referer 付与
- Cookieヘッダーのドメインスコープ化（無関係サイトのCookie混入防止・既存Cookie尊重）

加えて、停止処理でffmpegサブプロセスが確実に terminate -> kill にエスカレーションして
回収されることも検証する。
"""

import subprocess
import sys
import threading
import time
from pathlib import Path
from unittest.mock import patch

# プロジェクトルートをパスに追加
project_root = Path(__file__).parent.parent
sys.path.insert(0, str(project_root))

from ytdlpSpout.core import Streamer
from python.ytdlp_resolver import YtDlpAsyncResolver


def _make_streamer(url: str) -> Streamer:
    return Streamer(video_url=url, sender_name="TestSender", verbose=False)


class _FakeCookie:
    def __init__(self, domain: str, name: str, value: str):
        self.domain = domain
        self.name = name
        self.value = value


class _FakeYDL:
    """yt_dlp.YoutubeDL の最小スタブ"""

    captured_opts = None

    def __init__(self, opts):
        self.__class__.captured_opts = dict(opts)
        self.cookiejar = []

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc_val, exc_tb):
        return False

    def extract_info(self, url, download=False):
        raise NotImplementedError


def _fake_ydl_factory(info: dict, cookiejar=None):
    """extract_infoが指定infoを返すFakeYDLクラスを生成する"""

    class _FakeYDLWithInfo(_FakeYDL):
        def __init__(self, opts):
            super().__init__(opts)
            if cookiejar is not None:
                self.cookiejar = cookiejar

        def extract_info(self, url, download=False):
            return info

    return _FakeYDLWithInfo


class TestFormatSelectionMigration:
    """LEG-3: フォーマット選択のsingle-format-first移植"""

    def test_niconico_uses_single_format_first_string(self):
        streamer = _make_streamer("https://www.nicovideo.jp/watch/sm12345678")
        info = {
            "url": "https://delivery.domand.nicovideo.jp/xxx/playlist.m3u8",
            "http_headers": {},
            "duration": 30.0,
            "is_live": False,
            "width": 640,
            "height": 360,
        }
        fake_cls = _fake_ydl_factory(info)

        with patch("yt_dlp.YoutubeDL", fake_cls):
            result = streamer._yt_refresh()

        assert result is True
        assert fake_cls.captured_opts.get("format") == YtDlpAsyncResolver.SINGLE_FORMAT_FIRST_FORMAT_STRING

    def test_youtube_uses_generic_fallback_not_single_format_first(self):
        streamer = _make_streamer("https://www.youtube.com/watch?v=test123")
        info = {
            "url": "https://rr5---sn-xxx.googlevideo.com/videoplayback",
            "http_headers": {},
            "duration": 30.0,
            "is_live": False,
            "width": 1920,
            "height": 1080,
        }
        fake_cls = _fake_ydl_factory(info)

        with patch("yt_dlp.YoutubeDL", fake_cls):
            result = streamer._yt_refresh()

        assert result is True
        fmt = fake_cls.captured_opts.get("format", "")
        assert fmt != YtDlpAsyncResolver.SINGLE_FORMAT_FIRST_FORMAT_STRING
        assert not fmt.startswith("best/")


class TestRefererMigration:
    """LEG-2: ニコニコCDN向けReferer付与の移植"""

    def test_referer_added_for_niconico(self):
        streamer = _make_streamer("https://www.nicovideo.jp/watch/sm12345678")
        info = {
            "url": "https://delivery.domand.nicovideo.jp/xxx/playlist.m3u8",
            "http_headers": {},
            "duration": 30.0,
            "is_live": False,
            "width": 640,
            "height": 360,
        }
        fake_cls = _fake_ydl_factory(info)

        with patch("yt_dlp.YoutubeDL", fake_cls):
            result = streamer._yt_refresh()

        assert result is True
        assert streamer.http_headers.get("Referer") == "https://www.nicovideo.jp/"

    def test_referer_not_added_for_other_sites(self):
        streamer = _make_streamer("https://www.youtube.com/watch?v=test123")
        info = {
            "url": "https://rr5---sn-xxx.googlevideo.com/videoplayback",
            "http_headers": {},
            "duration": 30.0,
            "is_live": False,
            "width": 1920,
            "height": 1080,
        }
        fake_cls = _fake_ydl_factory(info)

        with patch("yt_dlp.YoutubeDL", fake_cls):
            result = streamer._yt_refresh()

        assert result is True
        assert "Referer" not in streamer.http_headers

    def test_existing_referer_not_overwritten(self):
        streamer = _make_streamer("https://www.nicovideo.jp/watch/sm12345678")
        info = {
            "url": "https://delivery.domand.nicovideo.jp/xxx/playlist.m3u8",
            "http_headers": {"Referer": "https://custom.example/"},
            "duration": 30.0,
            "is_live": False,
            "width": 640,
            "height": 360,
        }
        fake_cls = _fake_ydl_factory(info)

        with patch("yt_dlp.YoutubeDL", fake_cls):
            result = streamer._yt_refresh()

        assert result is True
        assert streamer.http_headers.get("Referer") == "https://custom.example/"


class TestCookieScopingMigration:
    """LEG-4: Cookieヘッダーのドメインスコープ化の移植"""

    def test_only_matching_domain_cookie_included(self):
        streamer = _make_streamer("https://www.nicovideo.jp/watch/sm12345678")
        cookies = [
            _FakeCookie(".nicovideo.jp", "niconico_session", "abc123"),
            _FakeCookie(".otherdomain.example", "other_session", "xyz789"),
        ]
        info = {
            "url": "https://delivery.domand.nicovideo.jp/xxx/playlist.m3u8",
            "http_headers": {},
            "duration": 30.0,
            "is_live": False,
            "width": 640,
            "height": 360,
        }
        fake_cls = _fake_ydl_factory(info, cookiejar=cookies)

        with patch("yt_dlp.YoutubeDL", fake_cls):
            result = streamer._yt_refresh()

        assert result is True
        cookie_header = streamer.http_headers.get("Cookie", "")
        assert "niconico_session=abc123" in cookie_header
        assert "other_session=xyz789" not in cookie_header

    def test_existing_cookie_header_not_overwritten(self):
        streamer = _make_streamer("https://www.nicovideo.jp/watch/sm12345678")
        cookies = [_FakeCookie(".nicovideo.jp", "niconico_session", "abc123")]
        info = {
            "url": "https://delivery.domand.nicovideo.jp/xxx/playlist.m3u8",
            "http_headers": {"Cookie": "ytdlp_computed=zzz"},
            "duration": 30.0,
            "is_live": False,
            "width": 640,
            "height": 360,
        }
        fake_cls = _fake_ydl_factory(info, cookiejar=cookies)

        with patch("yt_dlp.YoutubeDL", fake_cls):
            result = streamer._yt_refresh()

        assert result is True
        assert streamer.http_headers.get("Cookie") == "ytdlp_computed=zzz"

    def test_no_unrelated_domain_leak_when_no_niconico_cookie(self):
        """関係ないドメインしかCookieが無い場合、Cookieヘッダーが付与されない"""
        streamer = _make_streamer("https://www.nicovideo.jp/watch/sm12345678")
        cookies = [_FakeCookie(".otherdomain.example", "other_session", "xyz789")]
        info = {
            "url": "https://delivery.domand.nicovideo.jp/xxx/playlist.m3u8",
            "http_headers": {},
            "duration": 30.0,
            "is_live": False,
            "width": 640,
            "height": 360,
        }
        fake_cls = _fake_ydl_factory(info, cookiejar=cookies)

        with patch("yt_dlp.YoutubeDL", fake_cls):
            result = streamer._yt_refresh()

        assert result is True
        assert "Cookie" not in streamer.http_headers


class _FakeFfmpegProcess:
    """subprocess.Popen相当のスタブ。terminate/killの呼び出し順と回収を検証する。"""

    def __init__(self, alive_after_terminate: bool = False):
        self.terminate_called = False
        self.kill_called = False
        self.wait_calls = []
        self._alive_after_terminate = alive_after_terminate
        self._terminated = False
        self._killed = False

    def poll(self):
        if self._killed:
            return -9
        if self._terminated and not self._alive_after_terminate:
            return 0
        return None  # まだ実行中

    def terminate(self):
        self.terminate_called = True
        self._terminated = True

    def kill(self):
        self.kill_called = True
        self._killed = True

    def wait(self, timeout=None):
        self.wait_calls.append(timeout)
        if self.poll() is None:
            raise subprocess.TimeoutExpired(cmd="ffmpeg", timeout=timeout)
        return self.poll()


class TestFfmpegStopEscalation:
    """LEG-1: 停止処理でffmpegプロセスがterminate->killで確実に終了すること"""

    def test_stop_terminates_process_when_it_dies_promptly(self):
        streamer = _make_streamer("https://example.com/video.mp4")
        proc = _FakeFfmpegProcess(alive_after_terminate=False)
        streamer.proc = proc
        streamer.thread = None

        streamer.stop()

        assert proc.terminate_called is True
        assert proc.kill_called is False

    def test_stop_escalates_to_kill_when_terminate_insufficient(self):
        streamer = _make_streamer("https://example.com/video.mp4")
        proc = _FakeFfmpegProcess(alive_after_terminate=True)
        streamer.proc = proc
        streamer.thread = None

        streamer.stop()

        assert proc.terminate_called is True
        assert proc.kill_called is True

    def test_cleanup_uses_same_escalation(self):
        streamer = _make_streamer("https://example.com/video.mp4")
        proc = _FakeFfmpegProcess(alive_after_terminate=True)
        streamer.proc = proc

        streamer.cleanup()

        assert proc.terminate_called is True
        assert proc.kill_called is True

    def test_already_exited_process_is_not_touched(self):
        """既に終了済みのプロセスにはterminate/killを呼ばない"""
        streamer = _make_streamer("https://example.com/video.mp4")
        proc = _FakeFfmpegProcess()
        proc._terminated = True  # poll()が0を返す＝既に終了済み扱い
        streamer.proc = proc
        streamer.thread = None

        streamer.stop()

        assert proc.terminate_called is False
        assert proc.kill_called is False

    def test_stop_kills_process_even_if_run_thread_is_blocked(self):
        """
        run()のメインループがブロック（stop_eventを見ない状態）していても、
        stop()がffmpegプロセスを直接terminateし、長時間ハングしないこと
        （ゾンビプロセス化の回帰防止）
        """
        streamer = _make_streamer("https://example.com/video.mp4")
        proc = _FakeFfmpegProcess(alive_after_terminate=False)
        streamer.proc = proc

        blocked_started = threading.Event()

        def fake_blocked_run():
            blocked_started.set()
            time.sleep(10)  # stop_eventを見ずにブロックし続けるスレッドを模擬

        streamer.thread = threading.Thread(target=fake_blocked_run, daemon=True)
        streamer.thread.start()
        assert blocked_started.wait(timeout=1.0)

        start = time.perf_counter()
        streamer.stop()
        elapsed = time.perf_counter() - start

        assert proc.terminate_called is True
        # thread.join(timeout=2)以上に長時間ブロックしない
        assert elapsed < 5.0


if __name__ == "__main__":
    import pytest

    pytest.main([__file__, "-v"])
