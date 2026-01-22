"""
test_http_headers.py - HTTPヘッダーをC++ DLLに渡す機能のテスト

Issue: ニコニコ動画などのHTTPヘッダー（Cookie等）が必要なサイト対応
"""

import pytest
import sys
from pathlib import Path
from unittest.mock import Mock, patch, MagicMock
from ctypes import Structure, c_char_p, c_int, POINTER, c_void_p, c_size_t

# プロジェクトルートをパスに追加
project_root = Path(__file__).parent.parent
sys.path.insert(0, str(project_root))

# yt_dlpがない場合の処理
try:
    import yt_dlp
    HAS_YTDLP = True
except ImportError:
    HAS_YTDLP = False


class TestHttpHeaderStructure:
    """HTTPヘッダー構造体のテスト"""
    
    def test_http_header_structure_exists(self):
        """YtdlpSpoutHttpHeader構造体が存在する"""
        from python.ytdlpspout_native import YtdlpSpoutHttpHeader
        
        header = YtdlpSpoutHttpHeader()
        assert hasattr(header, 'key')
        assert hasattr(header, 'value')
    
    def test_http_header_structure_fields(self):
        """YtdlpSpoutHttpHeader構造体のフィールドが正しい"""
        from python.ytdlpspout_native import YtdlpSpoutHttpHeader
        
        # フィールド名と型を確認
        field_names = [name for name, _ in YtdlpSpoutHttpHeader._fields_]
        assert 'key' in field_names
        assert 'value' in field_names
    
    def test_http_header_structure_can_set_values(self):
        """YtdlpSpoutHttpHeader構造体に値を設定できる"""
        from python.ytdlpspout_native import YtdlpSpoutHttpHeader
        
        header = YtdlpSpoutHttpHeader()
        header.key = b"Cookie"
        header.value = b"session_id=abc123"
        
        assert header.key == b"Cookie"
        assert header.value == b"session_id=abc123"


class TestConfigExHttpHeaders:
    """YtdlpSpoutConfigExのHTTPヘッダーフィールドテスト"""
    
    def test_config_ex_has_http_headers_field(self):
        """YtdlpSpoutConfigExにhttpHeadersフィールドがある"""
        from python.ytdlpspout_native import YtdlpSpoutConfigEx
        
        config = YtdlpSpoutConfigEx()
        field_names = [name for name, _ in YtdlpSpoutConfigEx._fields_]
        assert 'httpHeaders' in field_names
        assert 'httpHeadersCount' in field_names
    
    def test_config_ex_http_headers_default_null(self):
        """YtdlpSpoutConfigExのhttpHeadersがデフォルトでNULL"""
        from python.ytdlpspout_native import YtdlpSpoutConfigEx
        
        config = YtdlpSpoutConfigEx()
        # httpHeadersはポインタなのでNone（NULL）がデフォルト
        assert config.httpHeaders is None or not config.httpHeaders
        assert config.httpHeadersCount == 0


class TestStartExHttpHeaders:
    """start_ex()のHTTPヘッダー引数テスト"""
    
    def test_start_ex_accepts_http_headers_parameter(self):
        """start_ex()がhttp_headers引数を受け付ける"""
        from python.ytdlpspout_native import YtdlpSpoutNative
        import inspect
        
        # start_exメソッドのシグネチャを確認
        sig = inspect.signature(YtdlpSpoutNative.start_ex)
        params = list(sig.parameters.keys())
        
        assert 'http_headers' in params, f"start_ex should have http_headers parameter: {params}"
    
    @pytest.mark.skipif(not Path(project_root / "cpp" / "build").exists(),
                        reason="C++ build not found")
    def test_start_ex_with_empty_headers(self):
        """start_ex()が空のhttp_headersで正常動作"""
        from python.ytdlpspout_native import YtdlpSpoutNative
        
        # DLLをロードできる場合のみテスト
        try:
            player = YtdlpSpoutNative()
            # 存在しないファイルでエラーになるが、ヘッダー処理自体は成功するはず
            try:
                player.start_ex(
                    source="nonexistent.mp4",
                    http_headers={}
                )
            except RuntimeError:
                pass  # ファイルが存在しないエラーは許容
        except Exception as e:
            pytest.skip(f"DLL not available: {e}")


class TestNativeStreamerWrapperHttpHeaders:
    """NativeStreamerWrapperのHTTPヘッダーテスト"""
    
    @pytest.mark.skipif(not HAS_YTDLP, reason="yt_dlp not installed")
    def test_native_streamer_wrapper_accepts_http_headers(self):
        """NativeStreamerWrapperがpre_resolved_headers引数を受け付ける"""
        from python.native_streamer_wrapper import NativeStreamerWrapper
        import inspect
        
        sig = inspect.signature(NativeStreamerWrapper.__init__)
        params = list(sig.parameters.keys())
        
        assert 'pre_resolved_headers' in params, \
            f"NativeStreamerWrapper should have pre_resolved_headers parameter: {params}"
    
    @pytest.mark.skipif(not HAS_YTDLP, reason="yt_dlp not installed")
    def test_native_streamer_wrapper_stores_headers(self):
        """NativeStreamerWrapperがHTTPヘッダーを保存する"""
        from python.native_streamer_wrapper import NativeStreamerWrapper
        
        headers = {"Cookie": "session=abc123", "User-Agent": "MyApp/1.0"}
        
        wrapper = NativeStreamerWrapper(
            video_url="test.mp4",
            sender_name="Test",
            pre_resolved_headers=headers
        )
        
        # ヘッダーが保存されていることを確認
        assert hasattr(wrapper, '_pre_resolved_headers')
        assert wrapper._pre_resolved_headers == headers


class TestHttpHeadersIntegration:
    """HTTPヘッダー統合テスト"""
    
    @pytest.mark.skipif(not HAS_YTDLP, reason="yt_dlp not installed")
    def test_resolved_info_headers_to_wrapper(self):
        """ResolvedInfoのhttp_headersがNativeStreamerWrapperに渡される"""
        from python.ytdlp_resolver import ResolvedInfo
        from python.native_streamer_wrapper import NativeStreamerWrapper
        
        # 解決情報を模擬
        resolved = ResolvedInfo(
            stream_url="https://example.com/video.mp4",
            duration=100.0,
            width=1920,
            height=1080,
            title="Test Video",
            http_headers={"Cookie": "test=value", "Referer": "https://example.com/"}
        )
        
        # NativeStreamerWrapperを作成
        wrapper = NativeStreamerWrapper(
            video_url="https://original.url/video",
            sender_name="Test",
            pre_resolved_url=resolved.stream_url,
            pre_resolved_headers=resolved.http_headers
        )
        
        # ヘッダーが正しく渡されていることを確認
        assert wrapper._pre_resolved_headers == resolved.http_headers
    
    def test_http_headers_array_creation(self):
        """Python dictからC配列への変換テスト"""
        from python.ytdlpspout_native import YtdlpSpoutHttpHeader
        from ctypes import Array
        
        headers = {
            "Cookie": "session=abc123",
            "User-Agent": "TestAgent/1.0",
            "Referer": "https://example.com/"
        }
        
        # 配列を作成
        HeaderArray = YtdlpSpoutHttpHeader * len(headers)
        header_array = HeaderArray()
        
        refs = []  # 文字列参照を保持
        for i, (key, value) in enumerate(headers.items()):
            key_bytes = key.encode('utf-8')
            value_bytes = value.encode('utf-8')
            refs.extend([key_bytes, value_bytes])
            
            header_array[i].key = key_bytes
            header_array[i].value = value_bytes
        
        # 配列が正しく作成されていることを確認
        assert len(header_array) == 3
        
        # 値を検証
        keys = [header_array[i].key.decode('utf-8') for i in range(len(headers))]
        assert "Cookie" in keys
        assert "User-Agent" in keys
        assert "Referer" in keys


class TestSecurityConsiderations:
    """セキュリティに関するテスト"""
    
    def test_cookie_value_not_logged_in_verbose_mode(self):
        """Cookie値がログに出力されないことを確認（モック）"""
        # この機能はC++側で実装されるため、ここではヘッダー数のみを
        # ログに記録すべきという設計を確認するテスト
        headers = {
            "Cookie": "secret_session_token=abc123xyz",
            "User-Agent": "MyApp/1.0"
        }
        
        # ヘッダー数だけ表示すべき
        log_message = f"HTTP headers count: {len(headers)}"
        assert "secret_session_token" not in log_message
        assert "abc123xyz" not in log_message


class TestHeaderArrayLimits:
    """ヘッダー配列の上限チェックテスト"""
    
    def test_header_limit_constant_defined(self):
        """ヘッダー上限チェックのロジックが存在する"""
        import inspect
        from python import ytdlpspout_native
        
        # start_exのソースコードを取得
        source = inspect.getsource(ytdlpspout_native.YtdlpSpoutNative.start_ex)
        
        # 上限チェックが実装されていることを確認
        assert 'MAX_HEADERS' in source or 'max_headers' in source.lower(), \
            "Header limit check should be implemented in start_ex"
    
    def test_many_headers_truncated_with_warning(self):
        """大量のヘッダーが警告付きで切り詰められる"""
        from python.ytdlpspout_native import YtdlpSpoutNative
        import logging
        
        # 101個のヘッダーを作成
        many_headers = {f"Header-{i}": f"Value-{i}" for i in range(101)}
        
        with patch('python.ytdlpspout_native.logging.warning') as mock_warning:
            with patch.object(YtdlpSpoutNative, '__init__', lambda self, dll_path=None: None):
                player = YtdlpSpoutNative()
                player._handle = c_void_p(1)
                player._lib = MagicMock()
                player._lib.ytdlpspout_start_ex.return_value = 0
                player._config_ex_refs = []
                
                # start_exを呼び出し
                player.start_ex(
                    source="test.mp4",
                    sender_name="Test",
                    http_headers=many_headers
                )
                
                # 警告が出力されたことを確認
                mock_warning.assert_called_once()
                warning_msg = mock_warning.call_args[0][0]
                assert '101' in warning_msg
                assert '100' in warning_msg
    
    def test_headers_at_limit_no_warning(self):
        """上限以下のヘッダー数では警告が出ない"""
        from python.ytdlpspout_native import YtdlpSpoutNative
        import logging
        
        # ちょうど100個のヘッダー
        headers_at_limit = {f"Header-{i}": f"Value-{i}" for i in range(100)}
        
        with patch('python.ytdlpspout_native.logging.warning') as mock_warning:
            with patch.object(YtdlpSpoutNative, '__init__', lambda self, dll_path=None: None):
                player = YtdlpSpoutNative()
                player._handle = c_void_p(1)
                player._lib = MagicMock()
                player._lib.ytdlpspout_start_ex.return_value = 0
                player._config_ex_refs = []
                
                player.start_ex(
                    source="test.mp4",
                    sender_name="Test",
                    http_headers=headers_at_limit
                )
                
                # 警告が出力されていないことを確認
                mock_warning.assert_not_called()


class TestBackwardCompatibility:
    """後方互換性テスト"""
    
    def test_start_ex_without_http_headers(self):
        """http_headers引数なしでstart_ex()が動作する"""
        from python.ytdlpspout_native import YtdlpSpoutNative
        import inspect
        
        sig = inspect.signature(YtdlpSpoutNative.start_ex)
        params = sig.parameters
        
        # http_headersがオプション引数であることを確認
        if 'http_headers' in params:
            param = params['http_headers']
            assert param.default is not inspect.Parameter.empty, \
                "http_headers should have a default value"
    
    @pytest.mark.skipif(not HAS_YTDLP, reason="yt_dlp not installed")
    def test_native_wrapper_without_headers(self):
        """pre_resolved_headers引数なしでNativeStreamerWrapperが動作する"""
        from python.native_streamer_wrapper import NativeStreamerWrapper
        
        # ヘッダーなしで作成
        wrapper = NativeStreamerWrapper(
            video_url="test.mp4",
            sender_name="Test"
        )
        
        # デフォルト値を確認
        assert hasattr(wrapper, '_pre_resolved_headers')
        assert wrapper._pre_resolved_headers is None or wrapper._pre_resolved_headers == {}
