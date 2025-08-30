import subprocess
import threading
import time
import os
import sys
import ssl
from urllib.parse import quote

import cv2
import numpy as np
import SpoutGL
import customtkinter as ctk
import tkinter as tk  # PanedWindow用
from PIL import Image, ImageTk
import yt_dlp
import tkinter.font as tkfont

from ytdlpSpout.core import (
    DEFAULT_VIDEO_URL,
    DEFAULT_SENDER_NAME,
    get_optimal_format_string,
    Streamer
)


class YtdlpLogger:
    """yt-dlp用のカスタムロガー：ログをGUIにリダイレクト"""
    def __init__(self, log_callback):
        self.log_callback = log_callback
        self.last_log_time = 0
        self.log_throttle_interval = 0.05  # 50msに短縮してより詳細にログを取得
        
        # デバッグ用：処理時間測定
        import time
        self.debug_start_time = time.time()
        
    def _should_log(self, msg):
        """ログ出力の制限を判定（デバッグ用に緩和）"""
        import time
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
            'loading youtube-', 'extracting signature', 'from cache'
        ]):
            return False
            
        # 時間制限チェック（より短い間隔で詳細ログ）
        if current_time - self.last_log_time < self.log_throttle_interval:
            return False
            
        self.last_log_time = current_time
        return True
    
    def _log_with_timing(self, level, msg):
        """タイミング情報付きでログ出力"""
        if self.log_callback and msg:
            import time
            current_time = time.time()
            elapsed = current_time - self.debug_start_time
            
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

# SSL証明書の設定（Windows環境での証明書問題を回避）
try:
    import ssl
    ssl._create_default_https_context = ssl._create_unverified_context
except Exception:
    pass


def get_best_japanese_font():
    """システムで利用可能な最適な日本語フォントを取得"""
    # 優先順位の高い順に日本語フォントをリスト
    preferred_fonts = [
        "Yu Gothic UI",      # Windows 10/11の美しいフォント
        "BIZ UDPGothic",     # Windows 11の新しいフォント
        "Noto Sans CJK JP",  # Google Noto フォント（美しい）
        "Noto Sans JP",      # Google Noto フォント（別名）
        "Hiragino Sans",     # macOS用（念のため）
        "Yu Gothic",         # フォールバック
        "Meiryo UI",         # 優先度を下げた
        "Meiryo",           # 優先度を下げた
        "MS Gothic",        # 最終フォールバック
    ]
    
    # システムで利用可能なフォントを取得
    available_fonts = tkfont.families()
    
    # 優先順位に従って利用可能なフォントを選択
    for font in preferred_fonts:
        if font in available_fonts:
            return font
    
    # どれも見つからない場合はデフォルト
    return "system"


def get_best_monospace_font():
    """システムで利用可能な最適な等幅フォントを取得（日本語対応優先）"""
    preferred_fonts = [
        "BIZ UDGothic",      # Windows 11の美しい日本語等幅フォント
        "MS Gothic",         # 日本語対応等幅フォント（意外と悪くない）
        "Noto Sans Mono CJK JP",  # Google Noto等幅フォント
        "Source Han Code JP", # Adobe製日本語等幅フォント
        "Cascadia Code",     # Windows Terminal用（英数字は美しい）
        "Consolas",          # Windows標準（英数字は美しい）
        "Courier New",       # フォールバック
        "monospace",         # 最終フォールバック
    ]
    
    available_fonts = tkfont.families()
    
    for font in preferred_fonts:
        if font in available_fonts:
            return font
    
    return "monospace"


class App:
    def __init__(self, root: ctk.CTk):
        self.root = root
        self.root.title("ytdlpSpout GUI")
        self.streamer: Streamer | None = None
        self._seeking = False
        
        # メインスレッド監視用変数
        self.main_thread_monitor_active = False
        self.main_thread_last_heartbeat = None
        self.main_thread_monitor_thread = None
        
        # 設定フラグ
        self.use_subprocess_download = True  # 別プロセスダウンロードを有効化
        self.seek_value = 0.0
        self.duration_cache = 0.0 # 動画の長さをキャッシュ
        self.local_video_path: str | None = None
        self.download_in_progress = False
        
        # ログキューを追加（ログ処理の非同期化）
        import queue
        self.log_queue = queue.Queue()
        self.log_processing = False
        
        # デバッグ用：プレビュー更新を一時停止する機能
        self.preview_update_disabled = False
        
        # CustomTkinterの外観設定
        ctk.set_appearance_mode("dark")  # "dark" or "light"
        ctk.set_default_color_theme("blue")  # "blue", "green", "dark-blue"
        
        # 美しい日本語フォントを設定
        self.japanese_font = get_best_japanese_font()
        self.monospace_font = get_best_monospace_font()
        
        # ログ用フォント：日本語が多い場合は日本語フォントを優先
        self.log_font = self.japanese_font  # 日本語ログが多いので日本語フォントを使用

        
        # ウィンドウの初期サイズを設定
        self.root.geometry("1000x700")
        self.root.minsize(800, 600)

        frm = ctk.CTkFrame(root)
        frm.pack(fill="x", padx=8, pady=8)
        
        # グリッドの列の重みを設定
        frm.grid_columnconfigure(1, weight=1)

        ctk.CTkLabel(frm, text="URL", font=ctk.CTkFont(family=self.japanese_font, size=12)).grid(row=0, column=0, sticky="w", padx=6, pady=6)
        self.url_var = ctk.StringVar(value=DEFAULT_VIDEO_URL)
        ctk.CTkEntry(frm, textvariable=self.url_var, width=400, font=ctk.CTkFont(family=self.japanese_font, size=11)).grid(
            row=0, column=1, columnspan=4, sticky="ew", padx=6, pady=6)

        ctk.CTkLabel(frm, text="Sender", font=ctk.CTkFont(family=self.japanese_font, size=12)).grid(row=1, column=0, sticky="w", padx=6, pady=6)
        self.sender_var = ctk.StringVar(value=DEFAULT_SENDER_NAME)
        ctk.CTkEntry(frm, textvariable=self.sender_var, width=200, font=ctk.CTkFont(family=self.japanese_font, size=11)).grid(
            row=1, column=1, sticky="w", padx=6, pady=6)

        # Resolution options
        ctk.CTkLabel(frm, text="Max W", font=ctk.CTkFont(family=self.japanese_font, size=12)).grid(row=2, column=0, sticky="w", padx=6, pady=6)
        self.maxw_var = ctk.StringVar(value="1920")
        ctk.CTkEntry(frm, textvariable=self.maxw_var, width=80, font=ctk.CTkFont(family=self.monospace_font, size=11)).grid(
            row=2, column=1, sticky="w", padx=6, pady=6)
        ctk.CTkLabel(frm, text="Max H", font=ctk.CTkFont(family=self.japanese_font, size=12)).grid(row=2, column=2, sticky="w", padx=6, pady=6)
        self.maxh_var = ctk.StringVar(value="1080")
        ctk.CTkEntry(frm, textvariable=self.maxh_var, width=80, font=ctk.CTkFont(family=self.monospace_font, size=11)).grid(
            row=2, column=3, sticky="w", padx=6, pady=6)
        self.max_enable = ctk.BooleanVar(value=False)
        ctk.CTkCheckBox(frm, text="Use Max Cap", variable=self.max_enable, font=ctk.CTkFont(family=self.japanese_font, size=12)).grid(
            row=2, column=4, sticky="w", padx=6, pady=6)

        ctk.CTkLabel(frm, text="Manual W", font=ctk.CTkFont(family=self.japanese_font, size=12)).grid(row=3, column=0, sticky="w", padx=6, pady=6)
        self.manw_var = ctk.StringVar(value="")
        ctk.CTkEntry(frm, textvariable=self.manw_var, width=80, font=ctk.CTkFont(family=self.monospace_font, size=11)).grid(
            row=3, column=1, sticky="w", padx=6, pady=6)
        ctk.CTkLabel(frm, text="Manual H", font=ctk.CTkFont(family=self.japanese_font, size=12)).grid(row=3, column=2, sticky="w", padx=6, pady=6)
        self.manh_var = ctk.StringVar(value="")
        ctk.CTkEntry(frm, textvariable=self.manh_var, width=80, font=ctk.CTkFont(family=self.monospace_font, size=11)).grid(
            row=3, column=3, sticky="w", padx=6, pady=6)
        self.manual_enable = ctk.BooleanVar(value=False)
        ctk.CTkCheckBox(frm, text="Use Manual", variable=self.manual_enable, font=ctk.CTkFont(family=self.japanese_font, size=12)).grid(
            row=3, column=4, sticky="w", padx=6, pady=6)

        # Performance options
        self.perf_limit = ctk.BooleanVar(value=True)  # デフォルトで有効
        perf_cb = ctk.CTkCheckBox(frm, text="1440p Limit (Performance)", variable=self.perf_limit, 
                                 font=ctk.CTkFont(family=self.japanese_font, size=12))
        perf_cb.grid(row=4, column=0, columnspan=2, sticky="w", padx=6, pady=6)
        
        # ツールチップ的な説明ラベル
        perf_info = ctk.CTkLabel(frm, text="※ 4K動画を1440pに制限してパフォーマンスを向上", 
                               font=ctk.CTkFont(family=self.japanese_font, size=12), text_color="gray")
        perf_info.grid(row=5, column=0, columnspan=3, sticky="w", padx=20, pady=2)
        
        # VOD loop option
        self.vod_loop = ctk.BooleanVar(value=False)
        ctk.CTkCheckBox(frm, text="Loop VOD", variable=self.vod_loop, 
                       font=ctk.CTkFont(family=self.japanese_font, size=12)).grid(
            row=6, column=0, sticky="w", padx=6, pady=6)

        # **別プロセスダウンロード（プチフリ解決）**
        self.subprocess_download = ctk.BooleanVar(value=True)  # デフォルトで別プロセス有効
        subprocess_cb = ctk.CTkCheckBox(frm, text="Subprocess Download (No Freeze)", variable=self.subprocess_download, 
                                  command=self.toggle_subprocess_download,
                                  font=ctk.CTkFont(family=self.japanese_font, size=12))
        subprocess_cb.grid(row=6, column=0, sticky="w", padx=6, pady=6)

        # --- ボタン定義 ---
        self.btn_start = ctk.CTkButton(frm, text="Start", command=self.on_start,
                                      font=ctk.CTkFont(family=self.japanese_font, size=12, weight="bold"))
        self.btn_stream = ctk.CTkButton(frm, text="Stream", command=self.on_stream, state="normal",
                                       font=ctk.CTkFont(family=self.japanese_font, size=12, weight="bold"))
        self.btn_stop = ctk.CTkButton(frm, text="Stop", command=self.on_stop, state="disabled",
                                     font=ctk.CTkFont(family=self.japanese_font, size=12, weight="bold"))

        self.btn_start.grid(row=7, column=2, padx=6, pady=6)
        self.btn_stream.grid(row=7, column=3, padx=6, pady=6)
        self.btn_stop.grid(row=7, column=4, padx=6, pady=6)


        self.info_label = ctk.CTkLabel(root, text="", font=ctk.CTkFont(family=self.japanese_font, size=14))
        self.info_label.pack(padx=8, pady=4)
        
        # コーデック対応状況を表示
        format_str, codec_info = get_optimal_format_string()
        self.codec_label = ctk.CTkLabel(root, text=f"コーデック対応: {codec_info}", 
                                       font=ctk.CTkFont(family=self.japanese_font, size=12), text_color="cyan")
        self.codec_label.pack(padx=8, pady=2)

        # メインコンテンツエリア（プレビューとログを分割可能なPanedWindow）
        main_paned = tk.PanedWindow(root, orient=tk.VERTICAL, sashwidth=5, sashrelief=tk.RAISED, bg="#212121")
        main_paned.pack(fill="both", expand=True, padx=8, pady=8)
        
        # プレビューエリア（上部）- CustomTkinterフレームをPanedWindowに追加
        preview_frame = ctk.CTkFrame(main_paned, fg_color="black")
        main_paned.add(preview_frame, minsize=200)  # 最小高さ200px
        
        self.preview_label = ctk.CTkLabel(preview_frame, text="No Signal", 
                                         text_color="white", font=ctk.CTkFont(family=self.japanese_font, size=16))
        self.preview_label.pack(fill="both", expand=True)
        self.preview_imgtk = None
        self._no_signal_shown = True

        # --- シークバー関連 ---
        seek_frame = ctk.CTkFrame(preview_frame, fg_color="transparent")
        seek_frame.pack(fill="x", padx=10, pady=5, side="bottom")

        self.seek_slider = ctk.CTkSlider(seek_frame, from_=0, to=100, state="disabled", command=self.on_seek_drag)
        self.seek_slider.pack(fill="x", expand=True, side="left", padx=(0, 10))
        self.seek_slider.bind("<ButtonPress-1>", self.on_seek_press)
        self.seek_slider.bind("<ButtonRelease-1>", self.on_seek_release)

        self.time_label = ctk.CTkLabel(seek_frame, text="--:-- / --:--", font=ctk.CTkFont(family=self.monospace_font, size=12))
        self.time_label.pack(side="right")
        # --------------------
        
        # ログエリア（下部）- CustomTkinterフレームをPanedWindowに追加
        log_frame = ctk.CTkFrame(main_paned)
        main_paned.add(log_frame, minsize=150)  # 最小高さ150px
        
        # ログエリアのタイトル
        log_title = ctk.CTkLabel(log_frame, text="ログ出力:", font=ctk.CTkFont(family=self.japanese_font, size=12, weight="bold"))
        log_title.pack(anchor="w", padx=5, pady=(5, 0))
        
        # ログテキストエリア - CTkTextboxに戻す（日本語フォント使用）
        self.log_text = ctk.CTkTextbox(
            log_frame, 
            height=150, 
            font=ctk.CTkFont(family=self.log_font, size=12)
        )
        self.log_text.pack(fill="both", expand=True, padx=5, pady=5)
        
        # 初期の分割比率を設定（プレビュー70%、ログ30%）
        self.root.after(100, lambda: main_paned.sash_place(0, 0, int(self.root.winfo_height() * 0.7)))
        
        self.root.protocol("WM_DELETE_WINDOW", self.on_close)
        # プレビュー更新頻度を下げて負荷軽減（33ms → 50ms, 20fps）
        self.root.after(50, self.update_preview)

    def log(self, msg: str):
        """ログメッセージを完全に非同期で処理"""
        try:
            # メインスレッドからの呼び出しかチェック
            import threading
            if threading.current_thread() == threading.main_thread():
                # メインスレッドからの場合は直接処理
                self._log_direct(msg)
            else:
                # 別スレッドからの場合はキューに追加
                self.log_queue.put(msg)
                # ログ処理が実行中でなければ開始
                if not self.log_processing:
                    self.root.after(0, self.process_log_queue)
        except Exception:
            pass
    
    def debug_log(self, msg: str):
        """デバッグ専用ログ（常に出力）"""
        try:
            import time
            timestamp = time.strftime("%H:%M:%S.%f")[:-3]  # ミリ秒まで表示
            debug_msg = f"[{timestamp}] {msg}"
            self._log_direct(debug_msg)
        except Exception:
            pass
    
    def _start_main_thread_monitor(self):
        """メインスレッドのブロッキングを監視"""
        if self.main_thread_monitor_active:
            return
            
        self.main_thread_monitor_active = True
        import time
        import threading
        
        def monitor_main_thread():
            """メインスレッドのハートビートとフレーム送信を監視"""
            last_frame_check = 0
            last_frame_count = 0
            
            while self.main_thread_monitor_active:
                current_time = time.time()
                
                # ハートビート更新をメインスレッドに依頼
                heartbeat_start = current_time
                self.root.after(0, self._main_thread_heartbeat)
                
                # Spout送信状態を定期的にログ
                if current_time - last_frame_check > 2.0:  # 2秒に1回
                    try:
                        if self.streamer and hasattr(self.streamer, 'latest_frame_bgr'):
                            frame_info = "フレーム有" if self.streamer.latest_frame_bgr is not None else "フレーム無"
                            playback_time = getattr(self.streamer, 'playback_time', 0)
                            self.root.after(0, self.log, f"[SPOUT] {frame_info}, 再生時間: {playback_time:.1f}秒")
                        last_frame_check = current_time
                    except Exception:
                        pass
                
                time.sleep(0.2)  # 200ms間隔でチェック
                
                # ハートビートが更新されているかチェック
                if hasattr(self, '_last_heartbeat_time'):
                    elapsed = current_time - self._last_heartbeat_time
                    if elapsed > 1.0:  # 1秒以上応答がない場合
                        if elapsed > 3.0:
                            self.root.after(0, self.log, f"[CRITICAL] メインスレッドが {elapsed:.3f}秒間ブロックされています！")
                        else:
                            self.root.after(0, self.log, f"[WARNING] メインスレッドブロック: {elapsed:.3f}秒")
        
        self.main_thread_monitor_thread = threading.Thread(target=monitor_main_thread, daemon=True)
        self.main_thread_monitor_thread.start()
        self.log("[DEBUG] メインスレッド＋Spout監視を開始しました")
        
    def _main_thread_heartbeat(self):
        """メインスレッドのハートビート（GUI スレッドで実行される）"""
        import time
        self._last_heartbeat_time = time.time()
        
    def _stop_main_thread_monitor(self):
        """メインスレッド監視を停止"""
        self.main_thread_monitor_active = False
        if self.main_thread_monitor_thread:
            self.main_thread_monitor_thread = None
        self.log("[DEBUG] メインスレッド監視を停止しました")
    
    def toggle_subprocess_download(self):
        """別プロセスダウンロードの有効/無効を切り替え"""
        self.use_subprocess_download = self.subprocess_download.get()
        if self.use_subprocess_download:
            self.debug_log("別プロセスダウンロード有効：プチフリーズを回避します")
        else:
            self.debug_log("通常ダウンロード：メインプロセスで実行します")

    def start_subprocess_download(self, url: str):
        """別プロセスでダウンロードを実行"""
        def run_subprocess():
            try:
                import subprocess
                import sys
                import json
                
                self.root.after(0, self.log, f"別プロセスダウンロード開始: {url}")
                
                # Python スクリプトとして実行するためのコード
                download_script = f"""
import yt_dlp
import os
import json
import re
import sys

def download_video():
    try:
        os.makedirs("data", exist_ok=True)
        
        # ダウンロード完了ファイルパスを格納
        class DownloadResult:
            def __init__(self):
                self.downloaded_file = None
        
        result_obj = DownloadResult()
        
        def progress_hook(d):
            if d['status'] == 'finished':
                result_obj.downloaded_file = d.get('filename')
        
        # 標準出力を一時的に無効化
        original_stdout = sys.stdout
        sys.stdout = open(os.devnull, 'w')
        
        try:
            ydl_opts = {{
                'format': 'best',
                'outtmpl': 'data/%(title)s.%(ext)s',
                'cookiefile': 'data/cookies.txt',
                'quiet': True,
                'no_warnings': True,
                'no_color': True,
                'extract_flat': False,
                'writethumbnail': False,
                'writeinfojson': False,
                'progress_hooks': [progress_hook],
                'noprogress': True,  # プログレス出力を完全に無効化
            }}
            
            with yt_dlp.YoutubeDL(ydl_opts) as ydl:
                info = ydl.extract_info('{url}', download=True)
                title = info.get('title', 'Unknown')
                
                # ダウンロードされたファイルパスを確実に取得
                if not result_obj.downloaded_file:
                    # progress_hookで取得できなかった場合、dataフォルダから最新ファイルを探す
                    import glob
                    data_files = glob.glob('data/*')
                    if data_files:
                        # 最新のファイルを取得
                        result_obj.downloaded_file = max(data_files, key=os.path.getctime)
        finally:
            # 標準出力を復元
            sys.stdout.close()
            sys.stdout = original_stdout
        
        # 結果をJSONで出力
        result = {{
            'status': 'success',
            'file_path': result_obj.downloaded_file,
            'title': title,
            'message': 'ダウンロード完了'
        }}
        print(json.dumps(result, ensure_ascii=False))
            
    except Exception as e:
        # エラーの場合も標準出力を復元
        if 'original_stdout' in locals():
            if sys.stdout != original_stdout:
                sys.stdout.close()
                sys.stdout = original_stdout
        
        result = {{
            'status': 'error',
            'message': f'ダウンロードエラー: {{e}}'
        }}
        print(json.dumps(result, ensure_ascii=False))

if __name__ == '__main__':
    download_video()
"""
                
                # 一時的なPythonファイルを作成
                script_path = "temp_download.py"
                with open(script_path, 'w', encoding='utf-8') as f:
                    f.write(download_script)
                
                # 別プロセスで実行
                process = subprocess.Popen(
                    [sys.executable, script_path],
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                    text=True,
                    creationflags=subprocess.CREATE_NO_WINDOW if hasattr(subprocess, 'CREATE_NO_WINDOW') else 0
                )
                
                stdout, stderr = process.communicate()
                
                # 一時ファイルを削除
                if os.path.exists(script_path):
                    os.remove(script_path)
                
                if process.returncode == 0 and stdout.strip():
                    try:
                        # 出力の最後の行（JSON）のみを取得
                        lines = stdout.strip().split('\n')
                        json_line = lines[-1].strip()
                        
                        # JSON結果をパース
                        result = json.loads(json_line)
                        self.root.after(0, self.log, f"ダウンロード結果受信: {result}")
                        
                        if result['status'] == 'success' and result.get('file_path'):
                            self.root.after(0, self.log, f"別プロセスダウンロード完了: {result['title']}")
                            self.root.after(0, self.log, f"ファイルパス: {result['file_path']}")
                            # **シームレス切り替えを実行**
                            self.root.after(0, self.switch_to_local_file, result['file_path'])
                        else:
                            self.root.after(0, self.log, f"ダウンロード結果: {result['message']}")
                    except json.JSONDecodeError as e:
                        self.root.after(0, self.log, f"JSON解析エラー: {e}")
                        self.root.after(0, self.log, f"生出力: {stdout.strip()}")
                    except IndexError:
                        self.root.after(0, self.log, f"出力形式エラー: {stdout.strip()}")
                else:
                    self.root.after(0, self.log, f"別プロセスダウンロードエラー: {stderr.strip()}")
                    
            except Exception as e:
                self.root.after(0, self.log, f"別プロセス実行エラー: {e}")
        
        # バックグラウンドで実行
        threading.Thread(target=run_subprocess, daemon=True).start()
    
    def switch_to_local_file(self, file_path: str):
        """ダウンロード完了後にローカルファイルへシームレスに切り替え"""
        try:
            self.log(f"シームレス切り替え開始: {file_path}")
            
            if not os.path.exists(file_path):
                self.log(f"エラー: ローカルファイルが見つかりません: {file_path}")
                return
            
            # ファイルの絶対パスを取得
            abs_file_path = os.path.abspath(file_path)
            self.log(f"絶対パス: {abs_file_path}")
            
            # 現在の再生位置を保存
            current_time = 0.0
            if self.streamer and hasattr(self.streamer, 'playback_time'):
                current_time = self.streamer.playback_time
                self.log(f"現在の再生位置: {current_time:.1f}秒")
            
            self.log(f"現在のストリーマーを停止中...")
            
            # 現在のストリーマーを停止
            if self.streamer:
                self.streamer.stop()
                self.streamer = None
                self.log("ストリーマー停止完了")
            
            # ローカルファイルパスを設定
            self.local_video_path = abs_file_path
            self.log(f"ローカルビデオパス設定: {self.local_video_path}")
            
            # 少し待機してからローカルファイルでストリーミング再開
            def restart_with_local():
                try:
                    self.log("ローカルファイルでストリーミング再開中...")
                    
                    # URLフィールドをローカルファイルパスに更新
                    self.url_var.set(abs_file_path)
                    self.log(f"URL更新: {abs_file_path}")
                    
                    # ローカルファイルでストリーミング開始
                    self.on_stream()
                    self.log("ストリーミング再開コマンド実行")
                    
                    # UI状態を更新
                    self.info_label.configure(text="ローカルファイル再生中")
                    
                    # 再生位置を復元（少し遅れて実行）
                    if current_time > 5.0:  # 5秒以上再生していた場合のみ復元
                        def restore_position():
                            if self.streamer and hasattr(self.streamer, 'seek'):
                                # 少し手前から再開（スムーズな切り替えのため）
                                seek_time = max(0, current_time - 2.0)
                                self.streamer.seek(seek_time)
                                self.log(f"再生位置を復元: {seek_time:.1f}秒 (元位置: {current_time:.1f}秒)")
                            else:
                                self.log("警告: ストリーマーにseek機能がありません")
                        
                        self.root.after(3000, restore_position)  # 3秒後に位置復元
                    
                    self.log("シームレス切り替え完了: ローカルファイルからストリーミング中")
                    
                except Exception as e:
                    self.log(f"ローカルファイル切り替えエラー: {e}")
            
            # 1秒後にローカルファイルで再開
            self.root.after(1000, restart_with_local)
            
        except Exception as e:
            self.log(f"シームレス切り替えエラー: {e}")
    
    def _log_direct(self, msg: str):
        """メインスレッドからの直接ログ処理"""
        try:
            self.log_text.insert("end", msg + "\n")
            self.log_text.see("end")
        except Exception:
            pass
    
    def process_log_queue(self):
        """ログキューを処理"""
        try:
            self.log_processing = True
            processed_count = 0
            max_process_per_batch = 5  # バッチサイズを小さくして応答性向上
            
            while not self.log_queue.empty() and processed_count < max_process_per_batch:
                try:
                    msg = self.log_queue.get_nowait()
                    self.log_text.insert("end", msg + "\n")
                    processed_count += 1
                except:
                    break
            
            if processed_count > 0:
                self.log_text.see("end")
            
            # まだログが残っている場合は次のバッチを予約
            if not self.log_queue.empty():
                self.root.after(25, self.process_log_queue)  # より短い間隔で処理
            else:
                self.log_processing = False
                
        except Exception:
            self.log_processing = False
    
    def format_time(self, seconds: float) -> str:
        """秒を HH:MM:SS 形式の文字列に変換"""
        if not isinstance(seconds, (int, float)) or seconds < 0:
            return "--:--"
        seconds = int(seconds)
        h = seconds // 3600
        m = (seconds % 3600) // 60
        s = seconds % 60
        if h > 0:
            return f"{h:02d}:{m:02d}:{s:02d}"
        else:
            return f"{m:02d}:{s:02d}"

    def on_seek_drag(self, value):
        if self._seeking:
            self.seek_value = value
            current_t = self.format_time(value)
            total_t = self.format_time(self.duration_cache)
            self.time_label.configure(text=f"{current_t} / {total_t}")

    def on_seek_press(self, event):
        if self.streamer and self.streamer.is_vod:
            self._seeking = True
            # マウスのクリック位置からスライダーの値を計算して設定
            slider_width = self.seek_slider.winfo_width()
            if slider_width == 0: return

            slider_range = self.seek_slider.cget("to") - self.seek_slider.cget("from_")
            
            click_x = event.x
            if click_x < 0:
                click_x = 0
            if click_x > slider_width:
                click_x = slider_width
            
            percentage = click_x / slider_width
            new_value = self.seek_slider.cget("from_") + (percentage * slider_range)
            
            self.seek_slider.set(new_value)

    def on_seek_release(self, event):
        if self.streamer and self.streamer.is_vod and self._seeking:
            self._seeking = False
            # マウスリリース時の最終的な値を元にシーク
            final_seek_value = self.seek_slider.get()
            self.streamer.seek(final_seek_value)

    def on_start(self):
        """「Start」ボタン：Streamボタンと同じ挙動に統一"""
        # Streamボタンと同じ処理を呼び出し
        self.on_stream()

    def on_download_complete(self):
        """ダウンロード完了処理（Streamボタンに統合済みのため、簡素化）"""
        self.log(f"動画をローカルに保存しました: {self.local_video_path}")
        self.btn_stream.configure(state="normal")
        self.btn_start.configure(state="normal") 
        self.info_label.configure(text="準備完了。")
        # 注意：自動再生はon_streamで処理されるため、ここでは実行しない


    def _start_spout_stream(self, video_source_url: str):
        """Spoutストリームを開始する内部ヘルパー関数"""
        if self.streamer:
            self.log("エラー: 既にストリームがアクティブです。")
            return
        
        # UIを即座に更新
        self.btn_start.configure(state="disabled")
        self.btn_stream.configure(state="disabled")
        self.btn_stop.configure(state="normal")
        self.info_label.configure(text="ストリーミング準備中...")
        self.log(f"{video_source_url} からストリーミングを開始します...")
        
        # プレビュー表示をリセット
        if hasattr(self, '_no_signal_shown'):
            delattr(self, '_no_signal_shown')
        self.preview_label.configure(text="")
        
        # **ローカルファイルかどうかを判定**
        # ローカルファイルかどうか判定
        is_local_file = os.path.exists(video_source_url) and os.path.isfile(video_source_url)
        
        if is_local_file:
            self.log(f"ローカルファイルを検出: {video_source_url}")
            # ローカルファイルの場合は直接PyAVでストリーミング
            self._start_local_file_stream(video_source_url)
        else:
            # URL の場合は従来通りyt-dlpを使用
            self._start_url_stream(video_source_url)
    
    def _start_local_file_stream(self, file_path: str):
        """ローカルファイルからのストリーミングを開始"""
        try:
            # パラメータを取得
            sender = self.sender_var.get().strip() or DEFAULT_SENDER_NAME
            
            # Streamerを作成（ローカルファイル用）
            self.streamer = Streamer(
                file_path, sender,
                max_resolution=None,
                manual_resolution=None,
                loop_vod=self.vod_loop.get(),
                verbose=True,
                log_cb=self._log_direct,
                stop_cb=self.on_auto_stop,
                init_ok_cb=self.on_stream_start_success
            )
            
            # ストリーミング開始
            self.streamer.start()
            
        except Exception as e:
            self.log(f"ローカルファイルストリーミング開始エラー: {e}")
            self.on_auto_stop()
    
    def _start_url_stream(self, video_source_url: str):
        """URLからのストリーミングを開始（従来の処理）"""
        maxw = self.maxw_var.get().strip()
        maxh = self.maxh_var.get().strip()
        manw = self.manw_var.get().strip()
        manh = self.manh_var.get().strip()
        sender = self.sender_var.get().strip() or DEFAULT_SENDER_NAME
        max_res = None
        manual_res = None
        
        if self.perf_limit.get() and not self.max_enable.get() and not self.manual_enable.get():
            max_res = (2560, 1440)
        
        try:
            if self.max_enable.get() and maxw and maxh:
                max_res = (int(maxw), int(maxh))
        except Exception:
            max_res = None
        try:
            if self.manual_enable.get() and manw and manh:
                manual_res = (int(manw), int(manh))
        except Exception:
            manual_res = None
        
        # 非同期でStreamerを初期化・開始
        def start_streaming_thread(url, sender_name, max_resolution, manual_resolution):
            try:
                self.streamer = Streamer(
                    url,
                    sender_name,
                    max_resolution=max_resolution,
                    manual_resolution=manual_resolution,
                    loop_vod=self.vod_loop.get(),
                    log_cb=lambda m: self.root.after(0, self.log, m),
                    stop_cb=lambda: self.root.after(0, self.on_auto_stop),
                    init_ok_cb=lambda: self.root.after(0, self.on_stream_start_success)
                )
                self.streamer.start()
            except Exception as e:
                self.root.after(0, self._handle_start_error, f"ストリーミング開始エラー: {e}")
        
        threading.Thread(target=start_streaming_thread, args=(video_source_url, sender, max_res, manual_res), daemon=True).start()

    def on_stream(self):
        """「Stream」ボタン：VODならストリーミング再生＋バックグラウンドダウンロード"""
        url = self.url_var.get().strip()
        if not url:
            self.log("エラー: ストリーミングするURLが入力されていません。")
            return

        # メインスレッド監視を開始（ブロッキング検出のため）
        self._start_main_thread_monitor()

        # まずストリーミング再生
        self._start_spout_stream(url)

        # VOD判定とダウンロード開始を非同期で行う（プチフリを防ぐ）
        def check_and_start_download():
            import time
            # ローカルファイルの場合はダウンロード処理をスキップ
            if os.path.exists(url):
                self.log("ローカルファイル再生中：ダウンロード処理はスキップします")
                return
                
            # より短い間隔で効率的にチェック
            for _ in range(50):  # 最大5秒間（0.1秒 × 50回）
                time.sleep(0.1)
                try:
                    if self.streamer and hasattr(self.streamer, 'is_vod') and self.streamer.is_vod:
                        if not self.download_in_progress:
                            # 別プロセスダウンロードを使用する場合
                            if getattr(self, 'use_subprocess_download', False):
                                self.root.after(0, self.log, "VOD検出: 別プロセスでバックグラウンドダウンロードを開始します")
                                self.start_subprocess_download(url)
                                return
                            
                            # **通常のバックグラウンドダウンロード開始**
                            self.root.after(0, self.log, "VOD検出: 最適化されたダウンロード+ストリーミングを開始します。")
                            self.root.after(0, self.debug_log, "VOD検出: バックグラウンドダウンロード開始判定")
                            
                            # **Step 1**: まずダウンロードの重い部分（メタデータ取得）を実行
                            def start_optimized_download():
                                import time
                                download_start_time = time.time()
                                self.root.after(0, self.log, "Step 1: ダウンロードメタデータ取得中...")
                                
                                # バックグラウンドダウンロードを開始（最初の重い処理）
                                self._start_background_download(url)
                                
                                # **Step 2**: 重い処理完了後、少し待ってからSpout最適化
                                def optimize_spout_after_download():
                                    time.sleep(1.0)  # 1秒待機してyt-dlpの重い処理が落ち着くのを待つ
                                    self.root.after(0, self.log, "Step 2: Spout送信を最適化中...")
                                    # 追加の最適化処理があればここに実装
                                
                                threading.Thread(target=optimize_spout_after_download, daemon=True).start()
                            
                            threading.Thread(target=start_optimized_download, daemon=True).start()
                            
                            # プレビューが無効化されているかチェック
                            if self.preview_update_disabled:
                                self.root.after(0, self.debug_log, "プレビュー無効モードでダウンロード開始")
                            
                            # ダウンロード開始も別スレッドで実行（メインスレッドをブロックしない）
                            import time
                            thread_start_time = time.time()
                            self.root.after(0, self.debug_log, f"スレッド開始時刻: {thread_start_time:.3f}")
                            threading.Thread(target=lambda: self._start_background_download(url), daemon=True).start()
                        return
                except Exception as e:
                    # streamerのアクセスでエラーが発生した場合は継続
                    continue
            
            # タイムアウトした場合
            self.root.after(0, self.log, "VOD判定がタイムアウトしました。ライブストリームまたは判定不可。")
        
        # バックグラウンドスレッドで実行
        threading.Thread(target=check_and_start_download, daemon=True).start()

    def _start_background_download(self, url):
        """VOD用: ストリーミング再生中にバックグラウンドでダウンロード"""
        if self.download_in_progress:
            self.root.after(0, self.log, "すでにバックグラウンドダウンロード中です。")
            return
        
        # ダウンロード開始の表示を非同期で行う
        self.download_in_progress = True
        self.root.after(0, self.log, "バックグラウンドダウンロードを準備中...")
        
        def download_video():
            try:
                import os
                import yt_dlp
                import subprocess
                import json
                
                # Windowsでスレッド優先度を下げてSpout送信への影響を軽減
                try:
                    import ctypes
                    kernel32 = ctypes.windll.kernel32
                    handle = kernel32.GetCurrentThread()
                    kernel32.SetThreadPriority(handle, -1)  # THREAD_PRIORITY_BELOW_NORMAL
                    self.root.after(0, self.debug_log, "ダウンロードスレッドの優先度を下げました")
                except Exception as e:
                    self.root.after(0, self.debug_log, f"スレッド優先度調整失敗: {e}")
                
                # すべてのGUI更新を非同期で実行
                self.root.after(0, self.log, "バックグラウンドダウンロード処理を開始します...")
                os.makedirs("data", exist_ok=True)
                
                # **新しいアプローチ**: 別プロセスでメタデータ取得を実行
                self.root.after(0, self.debug_log, "別プロセスでメタデータ取得を実行中...")
                
                # **より簡単で効果的な解決策**: 
                # 1. プロセス優先度を最低に設定
                # 2. 強力な速度制限
                # 3. 処理を細かく分散
                
                import time
                
                # **Step 1**: 短時間待機でCPUリソースを他に譲る
                for i in range(10):
                    time.sleep(0.1)  # 100msずつ待機
                    if i % 3 == 0:  # 300msごとにログ
                        self.root.after(0, self.debug_log, f"メタデータ取得準備中... ({i+1}/10)")
                
                # **Step 2**: 極めて制限的なオプションでyt-dlp実行
                
                # カスタムロガーを作成（完全に非同期）
                def async_log(msg):
                    self.root.after(0, self.log, msg)
                custom_logger = YtdlpLogger(async_log)
                
                # まず動画情報を軽量に取得
                ydl_opts_info = {
                    'format': 'bestvideo[ext=mp4]+bestaudio[ext=m4a]/best[ext=mp4]/best',
                    'outtmpl': 'data/%(id)s.%(ext)s',
                    'nocheckcertificate': True,
                    'logger': custom_logger,
                    'quiet': False,  # ログを表示するためFalseに変更
                    'no_warnings': False,
                    'extract_flat': False,  # 詳細情報が必要
                    'verbose': False,  # 詳細ログを無効化（プレビューフリーズを防ぐ）
                }
                
                with yt_dlp.YoutubeDL(ydl_opts_info) as ydl:
                    info = ydl.extract_info(url, download=False)
                    final_filename = ydl.prepare_filename(info)
                    # 動画情報をログに出力
                    title = info.get('title', 'Unknown')
                    duration = info.get('duration', 0)
                    self.root.after(0, self.log, f"動画情報取得完了: {title} (長さ: {duration}秒)")
                
                # ダウンロード開始前に少し待機（プチフリ軽減）
                import time
                time.sleep(0.1)
                
                # プログレスフックの定義（最適化版）
                def progress_hook(d):
                    try:
                        if d['status'] == 'downloading':
                            percent = d.get('_percent_str', '')
                            speed = d.get('_speed_str', '')
                            eta = d.get('_eta_str', '')
                            # GUI更新頻度を制限（1秒間隔）
                            if hasattr(progress_hook, 'last_log_time'):
                                import time
                                current_time = time.time()
                                if current_time - progress_hook.last_log_time < 1.0:  # 1秒間隔で制限
                                    return
                                progress_hook.last_log_time = current_time
                            else:
                                import time
                                progress_hook.last_log_time = time.time()
                            
                            self.root.after(0, self.log, f"BGダウンロード中: {percent} ({speed}, ETA: {eta})")
                        elif d['status'] == 'finished':
                            self.root.after(0, self.log, "BGダウンロード完了")
                        elif d['status'] == 'error':
                            self.root.after(0, self.log, f"BGダウンロードエラー: {d.get('_error_str', 'Unknown error')}")
                    except Exception:
                        pass  # プログレスフックのエラーでダウンロードを停止させない
                
                # ダウンロード開始の明示的なログ
                self.root.after(0, self.log, f"ダウンロードを開始します: {final_filename}")
                
                # ダウンロード実行
                ydl_opts_dl = {
                    'format': 'bestvideo[ext=mp4]+bestaudio[ext=m4a]/best[ext=mp4]/best',
                    'outtmpl': 'data/%(id)s.%(ext)s',
                    'progress_hooks': [progress_hook],
                    'nocheckcertificate': True,
                    'logger': custom_logger,
                    'quiet': False,  # ログを表示するためFalseに変更
                    'no_warnings': False,
                    'verbose': False,  # 詳細ログを無効化（プレビューフリーズを防ぐ）
                    'retries': 3,  # リトライ回数を制限
                    'fragment_retries': 3,  # フラグメントリトライも制限
                    'extractor_retries': 1,  # エクストラクターリトライを制限
                    'file_access_retries': 3,  # ファイルアクセスリトライを制限
                }
                
                with yt_dlp.YoutubeDL(ydl_opts_dl) as ydl:
                    import time
                    bg_download_start_time = time.time()
                    self.root.after(0, self.log, f"[DEBUG] BGダウンロード開始: {bg_download_start_time:.3f}")
                    self.root.after(0, self.log, "yt-dlpダウンロード処理を実行中...")
                    
                    ydl.download([url])
                    
                    bg_download_end_time = time.time()
                    bg_download_duration = bg_download_end_time - bg_download_start_time
                    self.root.after(0, self.log, f"[DEBUG] BGダウンロード完了: 処理時間 {bg_download_duration:.3f}秒")
                    self.root.after(0, self.log, "yt-dlpダウンロード処理が完了しました")
                
                self.local_video_path = final_filename
                self.root.after(0, self.log, f"ローカルファイルパスを設定: {final_filename}")
                self.root.after(0, self.on_background_download_complete)
                
            except Exception as e:
                error_msg = f"バックグラウンドダウンロード失敗: {e}"
                self.root.after(0, self.log, error_msg)
                self.root.after(0, self._handle_start_error, error_msg)
            finally:
                self.download_in_progress = False
        
        # バックグラウンドスレッドで実行（プチフリを防ぐ）
        threading.Thread(target=download_video, daemon=True).start()

    def on_background_download_complete(self):
        self.log(f"バックグラウンドで動画をローカル保存しました: {self.local_video_path}")
        import os
        # ストリーミング再生中ならローカルファイルへシームレスに切り替え
        if self.streamer and not self.streamer.is_local_file:
            if not self.local_video_path or not os.path.exists(self.local_video_path):
                self.log(f"ローカルファイル切り替え失敗: ファイルが存在しません ({self.local_video_path})")
                return
            self.log("ローカルファイルへシームレスに切り替えます...")
            # 現在の再生位置を取得
            current_time = 0.0
            try:
                current_time = self.streamer.playback_time
            except Exception:
                pass
            self.on_stop(delete_local_file=False)
            # 少し待ってからローカル再生
            def start_local():
                import time
                time.sleep(0.5)
                # 再度存在確認
                if not self.local_video_path or not os.path.exists(self.local_video_path):
                    self.log(f"ローカルファイル切り替え失敗: ファイルが存在しません ({self.local_video_path})")
                    return
                self._start_spout_stream(self.local_video_path)
                # 再生位置を復元（必要なら）
                if current_time > 0:
                    try:
                        self.streamer.seek(current_time)
                    except Exception:
                        pass
            import threading
            threading.Thread(target=start_local, daemon=True).start()
        else:
            self.btn_stream.configure(state="normal")
            self.btn_start.configure(state="normal")
            self.info_label.configure(text="準備完了。Streamボタンで再生を開始できます。")

    def on_stream_start_success(self):
        self.info_label.configure(text="ストリーミング開始")
        if self.streamer and self.streamer.is_vod:
            self.duration_cache = self.streamer.duration
            self.log(f"動画の長さを取得しました: {self.duration_cache} 秒")
            self.seek_slider.configure(state="normal", to=self.duration_cache)
            self.time_label.configure(text=f"00:00 / {self.format_time(self.duration_cache)}")

    def on_stop(self, delete_local_file=True, skip_ui_reset=False):
        if self.streamer:
            self.streamer.stop()
            self.streamer = None
        # UIリセット
        if not skip_ui_reset:
            self.btn_start.configure(state="normal")
            self.btn_stream.configure(state="normal") # Streamボタンは常に有効
            self.btn_stop.configure(state="disabled")
            self.info_label.configure(text="停止しました")
            self.seek_slider.configure(state="disabled")
            self.time_label.configure(text="--:-- / --:--")
        # ローカルファイルを削除（明示的な停止時のみ）
        if delete_local_file:
            if self.local_video_path and os.path.exists(self.local_video_path):
                try:
                    os.remove(self.local_video_path)
                    self.log(f"一時ファイルを削除しました: {self.local_video_path}")
                    self.local_video_path = None
                except Exception as e:
                    self.log(f"一時ファイルの削除に失敗: {e}")

    def _handle_start_error(self, error_msg: str):
        """開始エラーの共通処理"""
        self.log(f"エラー: {error_msg}")
        if self.streamer:
            self.streamer.stop()
            self.streamer = None
        
        self.download_in_progress = False
        self.btn_start.configure(state="normal")
        self.btn_stream.configure(state="normal") # Streamボタンは常に有効
        self.btn_stop.configure(state="disabled")
        self.info_label.configure(text="エラーが発生しました")

    def on_auto_stop(self):
        """動画終了時の自動停止処理"""
        self.log("動画の再生が完了しました。")
        self.on_stop() # 共通の停止処理を呼ぶ

    def on_close(self):
        try:
            self.on_stop()
        finally:
            self.root.destroy()

    def update_preview(self):
        # デバッグ用：プレビュー更新を一時的に停止
        if self.preview_update_disabled:
            self.root.after(50, self.update_preview)
            return
            
        update_start_time = None
        try:
            import time
            update_start_time = time.time()
            
            if self.streamer and self.streamer.latest_frame_bgr is not None:
                # フレーム取得を高速化
                with self.streamer.frame_lock:
                    frame = self.streamer.latest_frame_bgr.copy()  # コピーして即座にロック解除
                
                # 重い処理を分割して実行
                self._update_preview_frame_async(frame, update_start_time)
            else:
                if not hasattr(self, '_no_signal_shown') or not self._no_signal_shown:
                    self.preview_label.configure(image="", text="No Signal", text_color="white")
                    self._no_signal_shown = True
                    
            # ストリーマー情報更新（軽量処理のみ）
            if self.streamer:
                if self.streamer.is_vod:
                    if not self._seeking:
                        self.seek_slider.set(self.streamer.playback_time)
                        current_t = self.format_time(self.streamer.playback_time)
                        total_t = self.format_time(self.duration_cache)
                        self.time_label.configure(text=f"{current_t} / {total_t}")

                    self.info_label.configure(
                        text=f"Resolution: {self.streamer.width}x{self.streamer.height} @ {self.streamer.detected_fps}fps")
                else: # ライブの場合
                    self.info_label.configure(
                        text=f"(LIVE) Resolution: {self.streamer.width}x{self.streamer.height} @ {self.streamer.detected_fps}fps")
            elif not self.download_in_progress:
                self.info_label.configure(text="No stream active")
                
            # 軽量処理時間を測定
            if update_start_time:
                update_end_time = time.time()
                update_duration = update_end_time - update_start_time
                if update_duration > 0.01:  # 10ms以上の場合のみログ出力
                    self.debug_log(f"プレビュー軽量更新時間: {update_duration:.3f}秒")
                    
        except Exception as e:
            if update_start_time:
                import time
                update_end_time = time.time()
                update_duration = update_end_time - update_start_time
                self.debug_log(f"プレビュー更新エラー: {e} (時間: {update_duration:.3f}秒)")
        finally:
            # プレビュー更新頻度を下げて負荷軽減（33ms → 50ms, 20fps）
            self.root.after(50, self.update_preview)
    
    def _update_preview_frame_async(self, frame, start_time):
        """プレビューフレーム更新を非同期で処理"""
        def process_frame():
            try:
                import time
                
                # 画像処理を別スレッドで実行
                rgb = cv2.cvtColor(frame, cv2.COLOR_BGR2RGB)
                
                # ラベルサイズ取得をメインスレッドに委譲
                self.root.after(0, lambda: self._finalize_preview_update(rgb, start_time))
                
            except Exception as e:
                self.root.after(0, self.debug_log, f"フレーム処理エラー: {e}")
        
        # 画像処理を別スレッドで実行（メインスレッドをブロックしない）
        threading.Thread(target=process_frame, daemon=True).start()
    
    def _finalize_preview_update(self, rgb, start_time):
        """プレビュー更新の最終処理（メインスレッド）"""
        try:
            import time
            
            self.preview_label.update_idletasks()
            label_width = self.preview_label.winfo_width()
            label_height = self.preview_label.winfo_height()
            
            if label_width < 100: label_width = 640
            if label_height < 100: label_height = 360
            
            h, w, _ = rgb.shape
            scale = min(label_width / float(w), label_height / float(h))
            new_w, new_h = int(w * scale), int(h * scale)
            
            if new_w > 0 and new_h > 0:
                # リサイズと画像作成を最適化
                dst = cv2.resize(rgb, (new_w, new_h), interpolation=cv2.INTER_LINEAR)  # より高速な補間
                img = Image.fromarray(dst)
                self.preview_imgtk = ctk.CTkImage(light_image=img, dark_image=img, size=(new_w, new_h))
                self.preview_label.configure(image=self.preview_imgtk, text="")
                self._no_signal_shown = False
                
            # 全体の処理時間を測定
            if start_time:
                end_time = time.time()
                total_duration = end_time - start_time
                if total_duration > 0.05:  # 50ms以上の場合は警告
                    self.debug_log(f"プレビュー全体処理時間: {total_duration:.3f}秒 (長時間)")
                
        except Exception as e:
            self.debug_log(f"プレビュー最終処理エラー: {e}")


if __name__ == "__main__":
    root = ctk.CTk()
    app = App(root)
    root.mainloop()
