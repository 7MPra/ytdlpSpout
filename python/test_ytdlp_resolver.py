"""
test_ytdlp_resolver.py - YtDlpAsyncResolverのユニットテスト

YtDlpAsyncResolverクラスの非同期URL解決機能をテストします。
"""

import pytest
import sys
from pathlib import Path
from unittest.mock import Mock, patch, MagicMock
import concurrent.futures
import threading

# プロジェクトルートをパスに追加
project_root = Path(__file__).parent.parent
sys.path.insert(0, str(project_root))


class TestResolvedInfo:
    """ResolvedInfoクラスのテスト"""
    
    def test_create_resolved_info(self):
        """ResolvedInfoの作成"""
        from python.ytdlp_resolver import ResolvedInfo
        
        info = ResolvedInfo(
            stream_url="https://example.com/video.mp4",
            duration=120.5,
            width=1920,
            height=1080,
            title="Test Video"
        )
        
        assert info.stream_url == "https://example.com/video.mp4"
        assert info.duration == 120.5
        assert info.width == 1920
        assert info.height == 1080
        assert info.title == "Test Video"
    
    def test_resolved_info_defaults(self):
        """ResolvedInfoのデフォルト値"""
        from python.ytdlp_resolver import ResolvedInfo
        
        info = ResolvedInfo(stream_url="https://example.com/video.mp4")
        
        assert info.stream_url == "https://example.com/video.mp4"
        assert info.duration == 0.0
        assert info.width == 0
        assert info.height == 0
        assert info.title == ""


class TestYtDlpAsyncResolverUrlDetection:
    """YtDlpAsyncResolver.is_ytdlp_url() のテスト

    PY-1: allowlist方式を反転し、http(s)のページURL（直接メディア拡張子で
    終わらないもの）は未知ドメインでもyt-dlp解決を試みるように変更した。
    """

    def test_youtube_url(self):
        """YouTubeのURLを検出"""
        from python.ytdlp_resolver import YtDlpAsyncResolver

        assert YtDlpAsyncResolver.is_ytdlp_url("https://www.youtube.com/watch?v=dQw4w9WgXcQ") is True
        assert YtDlpAsyncResolver.is_ytdlp_url("https://youtube.com/watch?v=dQw4w9WgXcQ") is True
        assert YtDlpAsyncResolver.is_ytdlp_url("https://youtu.be/dQw4w9WgXcQ") is True
        assert YtDlpAsyncResolver.is_ytdlp_url("https://www.youtube.com/shorts/abcdefg") is True

    def test_unknown_domain_page_url_is_true(self):
        """未知ドメインのページURL（拡張子なし）はyt-dlp解決を試みる(True)"""
        from python.ytdlp_resolver import YtDlpAsyncResolver

        assert YtDlpAsyncResolver.is_ytdlp_url("https://example-video-site.com/watch/123") is True

    def test_unknown_domain_direct_media_extension_is_false(self):
        """未知ドメインでも直接メディア拡張子で終わるURLはFalse"""
        from python.ytdlp_resolver import YtDlpAsyncResolver

        assert YtDlpAsyncResolver.is_ytdlp_url("https://example-video-site.com/video.mp4") is False

    def test_known_domain_with_direct_extension_is_still_true(self):
        """既知ドメインは拡張子が直接メディアでもyt-dlp優先(True)"""
        from python.ytdlp_resolver import YtDlpAsyncResolver

        assert YtDlpAsyncResolver.is_ytdlp_url("https://www.youtube.com/video.mp4") is True
    
    def test_twitch_url(self):
        """TwitchのURLを検出"""
        from python.ytdlp_resolver import YtDlpAsyncResolver
        
        assert YtDlpAsyncResolver.is_ytdlp_url("https://www.twitch.tv/username") is True
        assert YtDlpAsyncResolver.is_ytdlp_url("https://twitch.tv/videos/123456789") is True
    
    def test_supported_domain_urls(self):
        """対応ドメインのURLがyt-dlp対象として検出される"""
        from python.ytdlp_resolver import YtDlpAsyncResolver
        
        assert YtDlpAsyncResolver.is_ytdlp_url("https://www.nicovideo.jp/watch/sm12345678") is True
        assert YtDlpAsyncResolver.is_ytdlp_url("https://nicovideo.jp/watch/sm12345678") is True
    
    def test_direct_video_url(self):
        """直接のビデオURLはFalse"""
        from python.ytdlp_resolver import YtDlpAsyncResolver
        
        # 直接のファイルURLはyt-dlpを必要としない
        assert YtDlpAsyncResolver.is_ytdlp_url("https://example.com/video.mp4") is False
        assert YtDlpAsyncResolver.is_ytdlp_url("https://cdn.example.com/stream.m3u8") is False
        assert YtDlpAsyncResolver.is_ytdlp_url("http://server.com/file.webm") is False
    
    def test_local_file_path(self):
        """ローカルファイルパスはFalse"""
        from python.ytdlp_resolver import YtDlpAsyncResolver
        
        assert YtDlpAsyncResolver.is_ytdlp_url("C:\\Videos\\test.mp4") is False
        assert YtDlpAsyncResolver.is_ytdlp_url("/home/user/video.mp4") is False
        assert YtDlpAsyncResolver.is_ytdlp_url("./video.mp4") is False
    
    def test_vimeo_url(self):
        """VimeoのURLを検出"""
        from python.ytdlp_resolver import YtDlpAsyncResolver
        
        assert YtDlpAsyncResolver.is_ytdlp_url("https://vimeo.com/123456789") is True
        assert YtDlpAsyncResolver.is_ytdlp_url("https://www.vimeo.com/123456789") is True
    
    def test_twitter_url(self):
        """Twitter/XのURLを検出"""
        from python.ytdlp_resolver import YtDlpAsyncResolver
        
        assert YtDlpAsyncResolver.is_ytdlp_url("https://twitter.com/user/status/123456789") is True
        assert YtDlpAsyncResolver.is_ytdlp_url("https://x.com/user/status/123456789") is True
    
    def test_empty_and_invalid(self):
        """空文字や無効な入力"""
        from python.ytdlp_resolver import YtDlpAsyncResolver
        
        assert YtDlpAsyncResolver.is_ytdlp_url("") is False
        assert YtDlpAsyncResolver.is_ytdlp_url("   ") is False
        assert YtDlpAsyncResolver.is_ytdlp_url("not a url") is False


class TestYtDlpAsyncResolverSync:
    """YtDlpAsyncResolver.resolve_sync() のテスト"""
    
    @patch('yt_dlp.YoutubeDL')
    def test_resolve_sync_success(self, mock_ydl_class):
        """同期解決の成功ケース"""
        from python.ytdlp_resolver import YtDlpAsyncResolver, ResolvedInfo
        
        # Mock yt-dlp response
        mock_ydl = MagicMock()
        mock_ydl.__enter__ = Mock(return_value=mock_ydl)
        mock_ydl.__exit__ = Mock(return_value=False)
        mock_ydl.extract_info.return_value = {
            'url': 'https://resolved.example.com/video.mp4',
            'duration': 180.0,
            'width': 1920,
            'height': 1080,
            'title': 'Test Video Title'
        }
        mock_ydl_class.return_value = mock_ydl
        
        resolver = YtDlpAsyncResolver()
        result = resolver.resolve_sync("https://www.youtube.com/watch?v=test123")
        
        assert result is not None
        assert result.stream_url == 'https://resolved.example.com/video.mp4'
        assert result.duration == 180.0
        assert result.width == 1920
        assert result.height == 1080
        assert result.title == 'Test Video Title'
    
    @patch('yt_dlp.YoutubeDL')
    def test_resolve_sync_failure(self, mock_ydl_class):
        """同期解決の失敗ケース"""
        from python.ytdlp_resolver import YtDlpAsyncResolver
        
        mock_ydl = MagicMock()
        mock_ydl.__enter__ = Mock(return_value=mock_ydl)
        mock_ydl.__exit__ = Mock(return_value=False)
        mock_ydl.extract_info.side_effect = Exception("Network error")
        mock_ydl_class.return_value = mock_ydl
        
        resolver = YtDlpAsyncResolver()
        result = resolver.resolve_sync("https://www.youtube.com/watch?v=test123")
        
        assert result is None
    
    @patch('yt_dlp.YoutubeDL')
    def test_resolve_sync_with_requested_formats(self, mock_ydl_class):
        """requested_formatsからURLを取得するケース"""
        from python.ytdlp_resolver import YtDlpAsyncResolver
        
        mock_ydl = MagicMock()
        mock_ydl.__enter__ = Mock(return_value=mock_ydl)
        mock_ydl.__exit__ = Mock(return_value=False)
        mock_ydl.extract_info.return_value = {
            'url': None,  # 直接URLがない
            'duration': 120.0,
            'width': 1280,
            'height': 720,
            'title': 'Multi-format Video',
            'requested_formats': [
                {'vcodec': 'avc1', 'url': 'https://video.example.com/stream.mp4'},
                {'vcodec': 'none', 'acodec': 'mp4a', 'url': 'https://audio.example.com/audio.m4a'}
            ]
        }
        mock_ydl_class.return_value = mock_ydl
        
        resolver = YtDlpAsyncResolver()
        result = resolver.resolve_sync("https://www.youtube.com/watch?v=test123")
        
        assert result is not None
        assert result.stream_url == 'https://video.example.com/stream.mp4'


class TestYtDlpAsyncResolverAsync:
    """YtDlpAsyncResolver.resolve_async() のテスト"""
    
    @patch('yt_dlp.YoutubeDL')
    def test_resolve_async_returns_future(self, mock_ydl_class):
        """非同期解決がFutureを返す"""
        from python.ytdlp_resolver import YtDlpAsyncResolver
        
        mock_ydl = MagicMock()
        mock_ydl.__enter__ = Mock(return_value=mock_ydl)
        mock_ydl.__exit__ = Mock(return_value=False)
        mock_ydl.extract_info.return_value = {
            'url': 'https://resolved.example.com/video.mp4',
            'duration': 60.0,
            'width': 640,
            'height': 480,
            'title': 'Async Test'
        }
        mock_ydl_class.return_value = mock_ydl
        
        resolver = YtDlpAsyncResolver()
        future = resolver.resolve_async("https://www.youtube.com/watch?v=test123")
        
        assert isinstance(future, concurrent.futures.Future)
        
        # 結果を取得
        result = future.result(timeout=5.0)
        assert result is not None
        assert result.stream_url == 'https://resolved.example.com/video.mp4'
        
        resolver.shutdown()
    
    @patch('yt_dlp.YoutubeDL')
    def test_resolve_async_handles_exception(self, mock_ydl_class):
        """非同期解決の例外処理"""
        from python.ytdlp_resolver import YtDlpAsyncResolver
        
        mock_ydl = MagicMock()
        mock_ydl.__enter__ = Mock(return_value=mock_ydl)
        mock_ydl.__exit__ = Mock(return_value=False)
        mock_ydl.extract_info.side_effect = Exception("Timeout")
        mock_ydl_class.return_value = mock_ydl
        
        resolver = YtDlpAsyncResolver()
        future = resolver.resolve_async("https://www.youtube.com/watch?v=test123")
        
        result = future.result(timeout=5.0)
        assert result is None  # エラー時はNone
        
        resolver.shutdown()


class TestYtDlpAsyncResolverCancel:
    """YtDlpAsyncResolver.cancel() のテスト"""
    
    def test_cancel_before_complete(self):
        """完了前のキャンセル"""
        from python.ytdlp_resolver import YtDlpAsyncResolver
        
        resolver = YtDlpAsyncResolver()
        
        # キャンセルが正常に動作することを確認
        resolver.cancel()
        
        # shutdownも正常に動作
        resolver.shutdown()
    
    def test_cancel_sets_flag(self):
        """キャンセルでフラグが設定される"""
        from python.ytdlp_resolver import YtDlpAsyncResolver
        
        resolver = YtDlpAsyncResolver()
        assert resolver._cancelled is False
        
        resolver.cancel()
        assert resolver._cancelled is True
        
        resolver.shutdown()


class TestYtDlpAsyncResolverCallback:
    """YtDlpAsyncResolver のコールバックテスト"""
    
    @patch('yt_dlp.YoutubeDL')
    def test_log_callback(self, mock_ydl_class):
        """ログコールバックが呼ばれる"""
        from python.ytdlp_resolver import YtDlpAsyncResolver
        
        mock_ydl = MagicMock()
        mock_ydl.__enter__ = Mock(return_value=mock_ydl)
        mock_ydl.__exit__ = Mock(return_value=False)
        mock_ydl.extract_info.return_value = {
            'url': 'https://resolved.example.com/video.mp4',
            'duration': 60.0,
            'width': 640,
            'height': 480,
            'title': 'Callback Test'
        }
        mock_ydl_class.return_value = mock_ydl
        
        log_messages = []
        
        resolver = YtDlpAsyncResolver(log_cb=lambda msg: log_messages.append(msg))
        result = resolver.resolve_sync("https://www.youtube.com/watch?v=test123")
        
        assert result is not None
        assert len(log_messages) > 0  # ログが出力されている
        
        resolver.shutdown()


class TestYtDlpAsyncResolverCookieFile:
    """YtDlpAsyncResolver のcookieファイル設定テスト"""
    
    @patch('yt_dlp.YoutubeDL')
    def test_cookie_file_option(self, mock_ydl_class):
        """cookieファイルオプションが正しく設定される"""
        from python.ytdlp_resolver import YtDlpAsyncResolver
        import os
        
        mock_ydl = MagicMock()
        mock_ydl.__enter__ = Mock(return_value=mock_ydl)
        mock_ydl.__exit__ = Mock(return_value=False)
        mock_ydl.extract_info.return_value = {
            'url': 'https://resolved.example.com/video.mp4',
            'duration': 60.0,
            'width': 640,
            'height': 480,
            'title': 'Cookie Test'
        }
        mock_ydl_class.return_value = mock_ydl
        
        resolver = YtDlpAsyncResolver(cookie_file="test_cookies.txt")
        result = resolver.resolve_sync("https://www.youtube.com/watch?v=test123")
        
        # YoutubeDLが呼ばれた際のオプションを確認
        call_args = mock_ydl_class.call_args
        assert call_args is not None
        opts = call_args[0][0]  # 最初の位置引数
        assert opts['cookiefile'] == "test_cookies.txt"
        
        resolver.shutdown()


class _MockCookie:
    """テスト用のcookiejarエントリ模擬クラス"""

    def __init__(self, name: str, value: str, domain: str):
        self.name = name
        self.value = value
        self.domain = domain


class TestYtDlpAsyncResolverCookieDomainFilter:
    """PY-2: Cookieヘッダー構築時のドメインフィルタリングのテスト"""

    def test_other_domain_cookie_not_included(self):
        """マッチしないドメインのCookieは連結対象に含まれない"""
        from python.ytdlp_resolver import YtDlpAsyncResolver

        cookiejar = [
            _MockCookie("session", "abc123", ".nicovideo.jp"),
            _MockCookie("unrelated", "xyz789", ".other-site.example"),
        ]

        header = YtDlpAsyncResolver._build_cookie_header(cookiejar, "delivery.domand.nicovideo.jp")

        assert "session=abc123" in header
        assert "unrelated=xyz789" not in header

    def test_nicovideo_domain_cookie_matches_domand_host(self):
        """.nicovideo.jpドメインのcookieがdelivery.domand.nicovideo.jpホストにマッチする"""
        from python.ytdlp_resolver import YtDlpAsyncResolver

        cookiejar = [_MockCookie("nicosid", "sid-value", ".nicovideo.jp")]

        header = YtDlpAsyncResolver._build_cookie_header(cookiejar, "delivery.domand.nicovideo.jp")

        assert header == "nicosid=sid-value"

    @patch('yt_dlp.YoutubeDL')
    def test_existing_cookie_header_not_overwritten(self, mock_ydl_class):
        """yt-dlpが既に計算したCookieヘッダーは上書きされない"""
        from python.ytdlp_resolver import YtDlpAsyncResolver

        mock_ydl = MagicMock()
        mock_ydl.__enter__ = Mock(return_value=mock_ydl)
        mock_ydl.__exit__ = Mock(return_value=False)
        mock_ydl.extract_info.return_value = {
            'url': 'https://resolved.example.com/video.mp4',
            'duration': 60.0,
            'width': 640,
            'height': 480,
            'title': 'Cookie Preserve Test',
            'http_headers': {'Cookie': 'from_ytdlp=already_set'},
        }
        mock_ydl.cookiejar = [_MockCookie("other", "value", ".resolved.example.com")]
        mock_ydl_class.return_value = mock_ydl

        resolver = YtDlpAsyncResolver()
        result = resolver.resolve_sync("https://www.youtube.com/watch?v=test123")

        assert result is not None
        assert result.http_headers['Cookie'] == 'from_ytdlp=already_set'

    @patch('yt_dlp.YoutubeDL')
    def test_cookiejar_not_merged_when_domain_mismatched(self, mock_ydl_class):
        """resolve_sync全体でも、無関係ドメインのCookieはヘッダーに設定されない"""
        from python.ytdlp_resolver import YtDlpAsyncResolver

        mock_ydl = MagicMock()
        mock_ydl.__enter__ = Mock(return_value=mock_ydl)
        mock_ydl.__exit__ = Mock(return_value=False)
        mock_ydl.extract_info.return_value = {
            'url': 'https://resolved.example.com/video.mp4',
            'duration': 60.0,
            'width': 640,
            'height': 480,
            'title': 'Cookie Domain Test',
            'http_headers': {},
        }
        mock_ydl.cookiejar = [_MockCookie("secret", "leak", ".unrelated-site.example")]
        mock_ydl_class.return_value = mock_ydl

        resolver = YtDlpAsyncResolver()
        result = resolver.resolve_sync("https://www.youtube.com/watch?v=test123")

        assert result is not None
        assert 'Cookie' not in result.http_headers


class TestResolvedInfoProtocolAndIsHls:
    """PY-3: ResolvedInfoのprotocol/is_liveフィールドとis_hlsプロパティのテスト"""

    def test_is_hls_true_for_m3u8_protocol(self):
        """protocolに'm3u8'を含む場合はis_hlsがTrue"""
        from python.ytdlp_resolver import ResolvedInfo

        info = ResolvedInfo(stream_url="https://example.com/playlist.m3u8", protocol="m3u8_native")
        assert info.is_hls is True

    def test_is_hls_false_for_https_protocol(self):
        """protocolが'https'などm3u8を含まない場合はis_hlsがFalse"""
        from python.ytdlp_resolver import ResolvedInfo

        info = ResolvedInfo(stream_url="https://example.com/video.mp4", protocol="https")
        assert info.is_hls is False

    def test_is_hls_none_when_protocol_unset(self):
        """protocolが空文字（未設定）の場合はis_hlsがNone"""
        from python.ytdlp_resolver import ResolvedInfo

        info = ResolvedInfo(stream_url="https://example.com/video.mp4")
        assert info.protocol == ""
        assert info.is_live is False
        assert info.is_hls is None

    @patch('yt_dlp.YoutubeDL')
    def test_resolve_sync_sets_protocol_and_is_live(self, mock_ydl_class):
        """resolve_syncがinfoのprotocol/is_liveをResolvedInfoに反映する"""
        from python.ytdlp_resolver import YtDlpAsyncResolver

        mock_ydl = MagicMock()
        mock_ydl.__enter__ = Mock(return_value=mock_ydl)
        mock_ydl.__exit__ = Mock(return_value=False)
        mock_ydl.extract_info.return_value = {
            'url': 'https://dmc.nicovideo.jp/video.m3u8',
            'duration': 0.0,
            'width': 1280,
            'height': 720,
            'title': 'Live Test',
            'protocol': 'm3u8_native',
            'is_live': True,
        }
        mock_ydl_class.return_value = mock_ydl

        resolver = YtDlpAsyncResolver()
        result = resolver.resolve_sync("https://live.nicovideo.jp/watch/lv12345")

        assert result is not None
        assert result.protocol == 'm3u8_native'
        assert result.is_live is True
        assert result.is_hls is True


if __name__ == "__main__":
    pytest.main([__file__, "-v"])
