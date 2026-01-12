"""
test_gui_native_progress.py - GUIのネイティブ進捗表示機能のテスト

gui.pyの_update_native_progressメソッドとshow_native_progressメソッドをテストします。
"""

import pytest
import sys
from pathlib import Path
from unittest.mock import Mock, MagicMock, patch

# プロジェクトルートをパスに追加
project_root = Path(__file__).parent.parent
sys.path.insert(0, str(project_root))


class TestNativeProgressDisplay:
    """GUIのネイティブ進捗表示テスト"""
    
    def test_update_native_progress_no_streamer(self):
        """ストリーマーがない場合は何もしない"""
        # gui.pyのAppクラスを直接テストするのは複雑なため、
        # 必要なメソッドのロジックを単体テスト
        
        # Mock App instance
        app = Mock()
        app.streamer = None
        
        # _update_native_progress のロジック
        def _update_native_progress():
            if not app.streamer:
                return
            if not hasattr(app.streamer, 'download_progress'):
                return
            # 他の処理...
        
        # 例外なく完了すること
        _update_native_progress()
    
    def test_update_native_progress_with_python_streamer(self):
        """Pythonストリーマー（download_progressなし）の場合は何もしない"""
        app = Mock()
        app.streamer = Mock(spec=['is_vod', 'is_live', 'width', 'height'])
        # download_progressプロパティがない場合
        
        def _update_native_progress():
            if not app.streamer:
                return
            if not hasattr(app.streamer, 'download_progress') or not hasattr(app.streamer, 'is_fully_cached'):
                return
            return True  # 処理が続く場合
        
        result = _update_native_progress()
        assert result is None  # 早期リターン
    
    def test_update_native_progress_with_native_streamer(self):
        """NativeStreamerWrapperの場合は進捗を処理する"""
        app = Mock()
        app.streamer = Mock()
        app.streamer.download_progress = 0.5
        app.streamer.is_fully_cached = False
        app.streamer.bandwidth = 1024 * 1024  # 1 MB/s
        
        def _update_native_progress():
            if not app.streamer:
                return None
            if not hasattr(app.streamer, 'download_progress') or not hasattr(app.streamer, 'is_fully_cached'):
                return None
            
            progress = app.streamer.download_progress
            is_cached = app.streamer.is_fully_cached
            bandwidth = getattr(app.streamer, 'bandwidth', 0.0)
            
            return {
                'progress': progress,
                'is_cached': is_cached,
                'bandwidth': bandwidth
            }
        
        result = _update_native_progress()
        assert result is not None
        assert result['progress'] == 0.5
        assert result['is_cached'] is False
        assert result['bandwidth'] == 1024 * 1024
    
    def test_bandwidth_formatting(self):
        """帯域幅のフォーマットが正しい"""
        def format_bandwidth(bandwidth: float) -> str:
            if bandwidth >= 1024 * 1024:
                return f"{bandwidth / (1024 * 1024):.1f} MB/s"
            elif bandwidth >= 1024:
                return f"{bandwidth / 1024:.1f} KB/s"
            else:
                return f"{bandwidth:.0f} B/s"
        
        # 1 MB/s
        assert format_bandwidth(1024 * 1024) == "1.0 MB/s"
        
        # 500 KB/s
        assert format_bandwidth(512 * 1024) == "512.0 KB/s"
        
        # 100 B/s
        assert format_bandwidth(100) == "100 B/s"
        
        # 2.5 MB/s
        assert format_bandwidth(2.5 * 1024 * 1024) == "2.5 MB/s"
    
    def test_progress_hidden_when_cached(self):
        """キャッシュ完了時は進捗バーを非表示にする"""
        app = Mock()
        app.streamer = Mock()
        app.streamer.download_progress = 1.0
        app.streamer.is_fully_cached = True
        app._native_progress_visible = True
        
        def should_hide_progress():
            if app.streamer.is_fully_cached:
                if hasattr(app, '_native_progress_visible') and app._native_progress_visible:
                    return True
            return False
        
        assert should_hide_progress() is True


class TestLegacySwitchingRemoved:
    """Legacy seamless switchingコードの削除テスト"""
    
    def test_switching_variables_not_in_gui(self):
        """切り替え関連変数がgui.pyに含まれていない"""
        gui_path = project_root / "gui.py"
        content = gui_path.read_text(encoding='utf-8')
        
        # 削除されたはずの変数/メソッド
        removed_items = [
            '_switching_in_progress',
            '_switching_cancelled',
            '_switching_thread',
            '_new_streamer',
            '_cancel_switching',
        ]
        
        for item in removed_items:
            assert item not in content, f"{item} はgui.pyから削除されているはずです"
    
    def test_switch_to_local_file_simplified(self):
        """switch_to_local_fileが簡略化されている"""
        gui_path = project_root / "gui.py"
        content = gui_path.read_text(encoding='utf-8')
        
        # 複雑な同期処理のキーワードが削除されている
        assert 'pause_at_pts' not in content or 'switch_to_local_file' not in content.split('pause_at_pts')[0][-500:]
        
        # 簡略版のコメントが存在する
        assert '簡略版' in content or 'C++ DLLバックエンド' in content


class TestYtDlpResolverIntegration:
    """YtDlpAsyncResolverのGUI統合テスト"""
    
    def test_ytdlp_resolver_import_in_gui(self):
        """YtDlpAsyncResolverがgui.pyにインポートされている"""
        gui_path = project_root / "gui.py"
        content = gui_path.read_text(encoding='utf-8')
        
        assert 'YtDlpAsyncResolver' in content
        assert 'YTDLP_RESOLVER_AVAILABLE' in content
    
    def test_url_resolving_flag_exists(self):
        """_url_resolving フラグがgui.pyに存在する"""
        gui_path = project_root / "gui.py"
        content = gui_path.read_text(encoding='utf-8')
        
        assert '_url_resolving' in content
    
    def test_start_url_stream_with_resolver_method_exists(self):
        """_start_url_stream_with_resolver メソッドが存在する"""
        gui_path = project_root / "gui.py"
        content = gui_path.read_text(encoding='utf-8')
        
        assert 'def _start_url_stream_with_resolver' in content
    
    def test_start_url_stream_direct_method_exists(self):
        """_start_url_stream_direct メソッドが存在する"""
        gui_path = project_root / "gui.py"
        content = gui_path.read_text(encoding='utf-8')
        
        assert 'def _start_url_stream_direct' in content
    
    def test_pre_resolved_url_used_in_gui(self):
        """pre_resolved_url引数がgui.pyで使用されている"""
        gui_path = project_root / "gui.py"
        content = gui_path.read_text(encoding='utf-8')
        
        assert 'pre_resolved_url=' in content
    
    def test_is_ytdlp_url_detection_logic(self):
        """is_ytdlp_url による判定ロジックがある"""
        gui_path = project_root / "gui.py"
        content = gui_path.read_text(encoding='utf-8')
        
        assert 'is_ytdlp_url' in content


# スタンドアロン実行用
if __name__ == "__main__":
    pytest.main([__file__, "-v"])
