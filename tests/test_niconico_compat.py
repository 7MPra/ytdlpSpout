"""
test_niconico_compat.py - フォーマット別・互換性テスト

- 単一フォーマット優先／汎用フォールバックのフォーマット選択
- Cookie・Referer・HLSストリーム対応
- 複数サイトのURL検出（yt-dlp対応ドメイン）
"""

import pytest
import sys
from pathlib import Path
from unittest.mock import Mock, patch, MagicMock
import os

# プロジェクトルートをパスに追加
project_root = Path(__file__).parent.parent
sys.path.insert(0, str(project_root))


class TestFormatSelectionLogic:
    """フォーマット選択ロジックのテスト"""
    
    def test_format_string_contains_aac_fallback(self):
        """フォーマット文字列にAAC音声フォールバックが含まれている"""
        from python.ytdlp_resolver import YtDlpAsyncResolver
        
        resolver = YtDlpAsyncResolver()
        
        # フォーマット文字列を取得するため、_get_ydl_optsを呼び出す
        ydl_opts = resolver._get_ydl_opts()
        format_str = ydl_opts.get('format', '')
        
        # AAC音声フォールバックが含まれていることを確認
        assert 'bestaudio[ext=aac]' in format_str or 'ba' in format_str, \
            f"フォーマット文字列にAAC音声対応が含まれていない: {format_str}"
    
    def test_format_string_contains_generic_fallback(self):
        """フォーマット文字列に汎用フォールバック（bv*+ba/b）が含まれている"""
        from python.ytdlp_resolver import YtDlpAsyncResolver
        
        resolver = YtDlpAsyncResolver()
        ydl_opts = resolver._get_ydl_opts()
        format_str = ydl_opts.get('format', '')
        
        # 汎用フォールバックが含まれていることを確認
        # bv*+ba または bv*+ba/b のいずれかが含まれる
        assert 'bv*+ba' in format_str or 'bv+ba' in format_str or 'best' in format_str, \
            f"フォーマット文字列に汎用フォールバックが含まれていない: {format_str}"
    
    def test_format_string_backward_compatible(self):
        """フォーマット文字列がYouTube向けの優先フォーマットを維持している"""
        from python.ytdlp_resolver import YtDlpAsyncResolver
        
        resolver = YtDlpAsyncResolver()
        ydl_opts = resolver._get_ydl_opts()
        format_str = ydl_opts.get('format', '')
        
        # YouTube用の優先フォーマット（mp4+m4a）が最初に含まれている
        assert format_str.startswith('bestvideo[ext=mp4]+bestaudio[ext=m4a]'), \
            f"YouTube向け優先フォーマットが最初に来ていない: {format_str}"


class TestSingleFormatFirstSelection:
    """単一フォーマット優先（フォーマット別特別処理）のテスト"""
    
    def test_single_format_first_url_uses_best_leading_format(self):
        """単一フォーマット優先のURLではフォーマット文字列が best で始まる"""
        from python.ytdlp_resolver import YtDlpAsyncResolver
        
        resolver = YtDlpAsyncResolver()
        ydl_opts = resolver._get_ydl_opts("https://www.nicovideo.jp/watch/sm12345678")
        format_str = ydl_opts.get('format', '')
        
        assert format_str.startswith('best/') or format_str == 'best', \
            f"単一フォーマット優先時は best 先頭であること: {format_str}"
    
    def test_other_url_uses_default_format(self):
        """単一フォーマット優先でないURLでは従来のフォーマット文字列を使う"""
        from python.ytdlp_resolver import YtDlpAsyncResolver
        
        resolver = YtDlpAsyncResolver()
        ydl_opts = resolver._get_ydl_opts("https://www.youtube.com/watch?v=test")
        format_str = ydl_opts.get('format', '')
        
        assert format_str.startswith('bestvideo[ext=mp4]+bestaudio[ext=m4a]'), \
            f"デフォルトは bestvideo+bestaudio 優先: {format_str}"
    
    def test_use_single_format_first_detection(self):
        """_use_single_format_first が対象ホストを正しく判定する"""
        from python.ytdlp_resolver import YtDlpAsyncResolver
        
        assert YtDlpAsyncResolver._use_single_format_first("https://www.nicovideo.jp/watch/sm1") is True
        assert YtDlpAsyncResolver._use_single_format_first("https://nico.ms/sm1") is True
        assert YtDlpAsyncResolver._use_single_format_first("https://live.nicovideo.jp/watch/lv1") is True
        assert YtDlpAsyncResolver._use_single_format_first("https://www.youtube.com/watch?v=1") is False
        assert YtDlpAsyncResolver._use_single_format_first("") is False


class TestYtdlpUrlDetection:
    """yt-dlp対象URL検出テスト"""
    
    def test_video_site_urls_detected(self):
        """動画サイトのURLがyt-dlp対象として検出される"""
        from python.ytdlp_resolver import YtDlpAsyncResolver
        
        # 通常の動画URL
        assert YtDlpAsyncResolver.is_ytdlp_url("https://www.nicovideo.jp/watch/sm12345678") is True
        assert YtDlpAsyncResolver.is_ytdlp_url("https://nicovideo.jp/watch/sm12345678") is True
        
        # 短縮URL
        assert YtDlpAsyncResolver.is_ytdlp_url("https://nico.ms/sm12345678") is True
    
    def test_live_domain_detected(self):
        """ライブ配信ドメインのURLが検出される"""
        from python.ytdlp_resolver import YtDlpAsyncResolver
        
        assert YtDlpAsyncResolver.is_ytdlp_url("https://live.nicovideo.jp/watch/lv12345678") is True


class TestHlsProtocolDetection:
    """HLSプロトコル検出テスト"""
    
    def test_is_hls_stream_detection(self):
        """HLSストリームURLが正しく検出される"""
        from python.ytdlp_resolver import YtDlpAsyncResolver
        
        # HLSストリームは直接再生可能としてFalseを返す
        assert YtDlpAsyncResolver.is_ytdlp_url("https://example.com/stream.m3u8") is False
        assert YtDlpAsyncResolver.is_ytdlp_url("https://cdn.example.com/video/master.m3u8") is False
    
    def test_protocol_in_resolved_url(self):
        """解決されたURLがHLSプロトコルを含む可能性がある（モックテスト）"""
        from python.ytdlp_resolver import YtDlpAsyncResolver, ResolvedInfo
        
        # HLSプロトコルで解決される可能性をテスト
        with patch('yt_dlp.YoutubeDL') as mock_ydl_class:
            mock_ydl = MagicMock()
            mock_ydl.__enter__ = Mock(return_value=mock_ydl)
            mock_ydl.__exit__ = Mock(return_value=False)
            mock_ydl.extract_info.return_value = {
                'url': 'https://dmc.nicovideo.jp/video.m3u8',
                'duration': 120.0,
                'width': 1280,
                'height': 720,
                'title': 'Test',
                'protocol': 'm3u8_native'
            }
            mock_ydl_class.return_value = mock_ydl
            
            resolver = YtDlpAsyncResolver()
            result = resolver.resolve_sync("https://www.nicovideo.jp/watch/sm12345678")
            
            assert result is not None
            assert 'm3u8' in result.stream_url or result.stream_url.endswith('.m3u8')
    
    def test_referer_added_for_some_sources(self):
        """参照元ヘッダーが必要なソースでRefererが含まれる"""
        from python.ytdlp_resolver import YtDlpAsyncResolver
        
        with patch('yt_dlp.YoutubeDL') as mock_ydl_class:
            mock_ydl = MagicMock()
            mock_ydl.__enter__ = Mock(return_value=mock_ydl)
            mock_ydl.__exit__ = Mock(return_value=False)
            mock_ydl.extract_info.return_value = {
                'url': 'https://delivery.domand.nicovideo.jp/.../playlist.m3u8',
                'duration': 60.0,
                'width': 640,
                'height': 360,
                'title': 'テスト',
                'http_headers': {},
            }
            mock_ydl.cookiejar = []
            mock_ydl_class.return_value = mock_ydl
            
            resolver = YtDlpAsyncResolver()
            result = resolver.resolve_sync("https://www.nicovideo.jp/watch/sm12345678")
            
            assert result is not None
            assert result.http_headers.get('Referer')


class TestCookieValidation:
    """Cookie検証テスト"""

    def test_get_default_cookie_file_returns_first_existing(self):
        """get_default_cookie_file は基準ディレクトリから最初に存在する候補を返す"""
        from python.ytdlp_resolver import YtDlpAsyncResolver

        # 存在しないディレクトリなら None
        result = YtDlpAsyncResolver.get_default_cookie_file(Path(__file__).parent / "nonexistent_dir_12345")
        assert result is None
        # プロジェクトルートで data/cookies.txt か cookies.txt があればパスが返る（どちらも無ければ None）
        result = YtDlpAsyncResolver.get_default_cookie_file(Path(__file__).parent.parent)
        if result is not None:
            assert "cookies.txt" in result

    def test_cookie_file_used_when_provided(self):
        """Cookieファイルが提供された場合に使用される"""
        from python.ytdlp_resolver import YtDlpAsyncResolver
        
        # 存在しないCookieファイルでもオプションとして設定される
        cookie_path = "data/cookies.txt"
        resolver = YtDlpAsyncResolver(cookie_file=cookie_path)
        
        ydl_opts = resolver._get_ydl_opts()
        assert ydl_opts.get('cookiefile') == cookie_path
    
    def test_cookie_file_not_set_when_disabled(self):
        """cookie_file='' の場合はCookieを使わない"""
        from python.ytdlp_resolver import YtDlpAsyncResolver
        
        resolver = YtDlpAsyncResolver(cookie_file="")
        ydl_opts = resolver._get_ydl_opts()
        
        assert ydl_opts.get('cookiefile') is None
    
    def test_cookie_log_on_use(self):
        """Cookieファイル使用時にログが出力される"""
        from python.ytdlp_resolver import YtDlpAsyncResolver
        
        log_messages = []
        def log_cb(msg):
            log_messages.append(msg)
        
        cookie_path = "data/cookies.txt"
        resolver = YtDlpAsyncResolver(cookie_file=cookie_path, log_cb=log_cb)
        
        # _log_cookie_info メソッドが存在し、動作することを確認
        resolver._log_cookie_info()
        
        # ログメッセージにCookie関連情報が含まれている（ファイル内容は含まない）
        cookie_logs = [msg for msg in log_messages if 'cookie' in msg.lower() or 'Cookie' in msg]
        assert len(cookie_logs) > 0, f"Cookieログが出力されていない: {log_messages}"
    
    def test_cookie_content_not_logged(self):
        """Cookieの内容がログに含まれないこと（セキュリティ）"""
        from python.ytdlp_resolver import YtDlpAsyncResolver
        
        log_messages = []
        def log_cb(msg):
            log_messages.append(msg)
        
        cookie_path = "data/cookies.txt"
        resolver = YtDlpAsyncResolver(cookie_file=cookie_path, log_cb=log_cb)
        resolver._log_cookie_info()
        
        # Cookieの内容（具体的な値）がログに含まれていないことを確認
        # セッションIDやトークンらしき文字列が含まれていないか
        for msg in log_messages:
            # 典型的なCookie値のパターン
            assert not any(pattern in msg for pattern in [
                'session_id=', 'user_session=', 'nicosid=', 'SAPISID=', 'SID='
            ]), f"Cookieの内容がログに含まれている: {msg}"
    
    def test_cookie_log_does_not_contain_sensitive_or_service_names(self):
        """Cookieログに機密や特定サービス名を含まない"""
        from python.ytdlp_resolver import YtDlpAsyncResolver
        
        log_messages = []
        def log_cb(msg):
            log_messages.append(msg)
        
        resolver = YtDlpAsyncResolver(cookie_file="data/cookies.txt", log_cb=log_cb)
        resolver._log_cookie_info()
        
        for msg in log_messages:
            assert "ニコニコ" not in msg and "niconico" not in msg.lower(), \
                f"サービス名をログに含めない: {msg}"


class TestNativeStreamerWrapperFormat:
    """NativeStreamerWrapperのフォーマット選択テスト"""
    
    def test_fallback_formats_include_aac(self):
        """フォールバックフォーマットにAAC音声が含まれている"""
        from python.native_streamer_wrapper import NativeStreamerWrapper
        
        # フォールバックフォーマットの定義を確認
        # _resolve_url_with_ytdlp メソッド内でAAC対応フォーマットが使われる
        wrapper = NativeStreamerWrapper(
            video_url="https://www.nicovideo.jp/watch/sm12345678",
            sender_name="TestSpout"
        )
        
        # フォールバックフォーマットリストを取得
        fallback_formats = wrapper._get_fallback_formats()
        
        # AAC音声対応が含まれている
        formats_str = '/'.join(fallback_formats)
        assert 'aac' in formats_str or 'ba' in formats_str, \
            f"フォールバックフォーマットにAAC対応が含まれていない: {fallback_formats}"


class TestErrorHandling:
    """エラーハンドリングテスト"""
    
    def test_format_not_available_error_handling(self):
        """'Requested format is not available' エラー時のフォールバック処理"""
        from python.ytdlp_resolver import YtDlpAsyncResolver
        import yt_dlp
        
        log_messages = []
        def log_cb(msg):
            log_messages.append(msg)
        
        with patch('yt_dlp.YoutubeDL') as mock_ydl_class:
            # 最初のフォーマットでエラー、フォールバックで成功
            call_count = [0]
            
            def extract_side_effect(url, download=False):
                call_count[0] += 1
                if call_count[0] == 1:
                    raise yt_dlp.utils.DownloadError("Requested format is not available")
                return {
                    'url': 'https://resolved.example.com/video.mp4',
                    'duration': 120.0,
                    'width': 1280,
                    'height': 720,
                    'title': 'Fallback Test'
                }
            
            mock_ydl = MagicMock()
            mock_ydl.__enter__ = Mock(return_value=mock_ydl)
            mock_ydl.__exit__ = Mock(return_value=False)
            mock_ydl.extract_info.side_effect = extract_side_effect
            mock_ydl_class.return_value = mock_ydl
            
            resolver = YtDlpAsyncResolver(log_cb=log_cb)
            result = resolver.resolve_sync("https://www.nicovideo.jp/watch/sm12345678")
            
            # フォールバックで成功した場合は結果が返る
            # または適切なエラーメッセージがログに出力される
            if result is None:
                # エラー発生時は適切なログが出力されていることを確認
                error_logs = [msg for msg in log_messages if 'エラー' in msg or 'error' in msg.lower() or 'フォールバック' in msg or 'fallback' in msg.lower()]
                assert len(error_logs) > 0, f"エラー発生時に適切なログが出力されていない: {log_messages}"
    
    def test_authentication_error_message(self):
        """認証エラー時の適切なメッセージ出力"""
        from python.ytdlp_resolver import YtDlpAsyncResolver
        import yt_dlp
        
        log_messages = []
        def log_cb(msg):
            log_messages.append(msg)
        
        with patch('yt_dlp.YoutubeDL') as mock_ydl_class:
            mock_ydl = MagicMock()
            mock_ydl.__enter__ = Mock(return_value=mock_ydl)
            mock_ydl.__exit__ = Mock(return_value=False)
            mock_ydl.extract_info.side_effect = yt_dlp.utils.DownloadError(
                "This video is for premium members only"
            )
            mock_ydl_class.return_value = mock_ydl
            
            resolver = YtDlpAsyncResolver(log_cb=log_cb)
            result = resolver.resolve_sync("https://www.nicovideo.jp/watch/sm12345678")
            
            assert result is None
            # エラーログが出力されている
            error_logs = [msg for msg in log_messages if 'エラー' in msg or 'error' in msg.lower()]
            assert len(error_logs) > 0


class TestBackwardCompatibility:
    """後方互換性テスト"""
    
    @patch('yt_dlp.YoutubeDL')
    def test_youtube_format_still_works(self, mock_ydl_class):
        """YouTubeのフォーマット選択が引き続き動作する"""
        from python.ytdlp_resolver import YtDlpAsyncResolver
        
        mock_ydl = MagicMock()
        mock_ydl.__enter__ = Mock(return_value=mock_ydl)
        mock_ydl.__exit__ = Mock(return_value=False)
        mock_ydl.extract_info.return_value = {
            'url': 'https://rr5---sn-xxx.googlevideo.com/videoplayback',
            'duration': 180.0,
            'width': 1920,
            'height': 1080,
            'title': 'YouTube Test Video'
        }
        mock_ydl_class.return_value = mock_ydl
        
        resolver = YtDlpAsyncResolver()
        result = resolver.resolve_sync("https://www.youtube.com/watch?v=test123")
        
        assert result is not None
        assert result.stream_url is not None
        assert result.width == 1920
        assert result.height == 1080
    
    def test_existing_domain_detection_unchanged(self):
        """既存のドメイン検出が変更されていない"""
        from python.ytdlp_resolver import YtDlpAsyncResolver
        
        # YouTubeドメイン
        assert YtDlpAsyncResolver.is_ytdlp_url("https://www.youtube.com/watch?v=test") is True
        assert YtDlpAsyncResolver.is_ytdlp_url("https://youtu.be/test") is True
        
        # Twitchドメイン
        assert YtDlpAsyncResolver.is_ytdlp_url("https://www.twitch.tv/streamer") is True
        
        # Vimeoドメイン
        assert YtDlpAsyncResolver.is_ytdlp_url("https://vimeo.com/123456") is True
        
        # 直接URLは引き続きFalse
        assert YtDlpAsyncResolver.is_ytdlp_url("https://example.com/video.mp4") is False


if __name__ == "__main__":
    pytest.main([__file__, "-v"])
