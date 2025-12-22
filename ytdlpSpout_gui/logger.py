"""yt-dlp用カスタムロガー"""

from __future__ import annotations

import time
from typing import Callable


class YtdlpLogger:
    """yt-dlp用のカスタムロガー：ログをGUIにリダイレクト"""
    
    def __init__(self, log_callback: Callable[[str], None] | None) -> None:
        self.log_callback = log_callback
        self.last_log_time: float = 0
        self.log_throttle_interval: float = 0.05  # 50ms間隔
        self.debug_start_time: float = time.time()
        
    def _should_log(self, msg: str | None) -> bool:
        """ログ出力の制限を判定"""
        current_time = time.time()
        
        # デバッグ用：重要なキーワードは必ず通す
        if msg and any(keyword in msg.lower() for keyword in [
            'downloading', 'format', 'merging', 'writing', 'finished',
            'error', 'warning', 'connection', 'timeout'
        ]):
            return True
        
        # 署名関数やキャッシュ関連のデバッグログは抑制
        if msg and any(keyword in msg.lower() for keyword in [
            'signature function', 'sigfuncs', 'nsig', 'decrypted nsig',
            'loading youtube-', 'extracting signature', 'from cache',
            'no supported javascript runtime', 'some web_safari client', 'some web client'
        ]):
            return False
            
        # 時間制限チェック（より短い間隔で詳細ログ）
        if current_time - self.last_log_time < self.log_throttle_interval:
            return False
            
        self.last_log_time = current_time
        return True
    
    def _log_with_timing(self, level: str, msg: str | None) -> bool:
        """タイミング情報付きでログ出力"""
        if self.log_callback and msg:
            elapsed = time.time() - self.debug_start_time
            
            # より多くのメッセージをタイミング付きで出力
            if any(keyword in msg.lower() for keyword in [
                'downloading tv simply player api json',
                'downloading 1 format(s)',
                'format selection',
                'merged format',
                'writing',
                'merging',
                'finished',
                'post-processing'
            ]):
                self.log_callback(f"[{elapsed:.3f}s] [{level}] {msg}")
                return True
                
        return False
    
    def debug(self, msg):
        # タイミング重要なメッセージは必ず出力
        if self._log_with_timing("DEBUG", msg):
            return
            
        # より多くのデバッグメッセージを表示
        if self.log_callback and msg and self._should_log(msg):
            self.log_callback(f"[yt-dlp DEBUG] {msg}")
    
    def info(self, msg):
        if self._log_with_timing("INFO", msg):
            return
            
        if self.log_callback and msg and self._should_log(msg):
            self.log_callback(f"[yt-dlp] {msg}")
    
    def warning(self, msg):
        if self.log_callback and msg:
            self.log_callback(f"[yt-dlp WARNING] {msg}")
    
    def error(self, msg):
        if self.log_callback and msg:
            self.log_callback(f"[yt-dlp ERROR] {msg}")
    
    # yt-dlpが期待する可能性のある追加メソッド
    def critical(self, msg):
        if self.log_callback and msg:
            self.log_callback(f"[yt-dlp CRITICAL] {msg}")
    
    def log(self, level, msg):
        """汎用ログメソッド"""
        if self._log_with_timing(f"L{level}", msg):
            return
            
        if self.log_callback and msg and self._should_log(msg):
            self.log_callback(f"[yt-dlp L{level}] {msg}")
