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


# Defaults
DEFAULT_VIDEO_URL = "https://www.youtube.com/watch?v=dQw4w9WgXcQ"
DEFAULT_SENDER_NAME = "ytdlpSpoutSender"
MAX_CONSECUTIVE_FAILURES = 3
MIN_FPS, MAX_FPS = 15, 120


def get_executable_dir():
    """実行ファイルのディレクトリを取得"""
    if getattr(sys, 'frozen', False):
        # PyInstallerでビルドされた場合
        return os.path.dirname(sys.executable)
    else:
        # 開発環境の場合
        return os.path.dirname(os.path.abspath(__file__))

def find_ffmpeg_path():
    """ffmpegのパスを検索"""
    exe_dir = get_executable_dir()
    
    # 1. exe同階層のbinディレクトリを確認
    bin_dir = os.path.join(exe_dir, 'bin')
    ffmpeg_in_bin = os.path.join(bin_dir, 'ffmpeg.exe')
    if os.path.exists(ffmpeg_in_bin):
        return ffmpeg_in_bin
    
    # 2. exe同階層を確認
    ffmpeg_in_exe_dir = os.path.join(exe_dir, 'ffmpeg.exe')
    if os.path.exists(ffmpeg_in_exe_dir):
        return ffmpeg_in_exe_dir
    
    # 3. システムPATHから検索
    return 'ffmpeg'


def check_av1_support():
    """ffmpegでAV1デコードがサポートされているかチェック"""
    try:
        ffmpeg_path = find_ffmpeg_path()
        result = subprocess.run(
            [ffmpeg_path, '-decoders'],
            capture_output=True,
            text=True,
            timeout=10,
            creationflags=subprocess.CREATE_NO_WINDOW if hasattr(subprocess, 'CREATE_NO_WINDOW') else 0
        )
        
        if result.returncode == 0:
            # AV1デコーダーの存在をチェック
            decoders_output = result.stdout.lower()
            av1_decoders = ['libdav1d', 'libaom-av1', 'av1']
            supported_decoders = [decoder for decoder in av1_decoders if decoder in decoders_output]
            
            if supported_decoders:
                return True, supported_decoders
            else:
                return False, []
        else:
            return False, []
            
    except Exception as e:
        # エラーが発生した場合は安全側に倒してAV1を無効にする
        return False, []


def get_optimal_format_string():
    """環境に応じた最適なフォーマット文字列を生成"""
    av1_supported, av1_decoders = check_av1_support()
    
    if av1_supported:
        # AV1対応環境: パフォーマンスを考慮して2160p以下に制限
        format_str = (
            'bestvideo[height<=2160][height>=720]+bestaudio/'
            'bestvideo[vcodec!*=av01][height<=1440][height>=720]+bestaudio/'  # AV1が重い場合のフォールバック
            'best[height<=2160][height>=720]/'
            'bestvideo[height>=720]+bestaudio/'
            'best'
        )
        codec_info = f"AV1対応 (デコーダー: {', '.join(av1_decoders)}, 2160p以下)"
    else:
        # AV1非対応環境: AV1を避けて高解像度を選択
        format_str = (
            'bestvideo[vcodec!*=av01][height<=2160][height>=720]+bestaudio/'
            'bestvideo[vcodec!*=av01][height>=720]+bestaudio/'
            'best[vcodec!*=av01][height>=720]/'
            'bestvideo[height>=720]+bestaudio/'
            'best'
        )
        codec_info = "AV1非対応 (H.264/VP9を優先, 2160p以下)"
    
    return format_str, codec_info

def build_ffmpeg_header_args(headers: dict) -> list:
    args = []
    if not headers:
        return args
    for k, v in headers.items():
        args += ["-headers", f"{k}: {v}"]
    return args


def detect_fps(info: dict) -> int | None:
    rf = info.get("requested_formats")
    if isinstance(rf, list):
        for f in rf:
            if f and f.get("vcodec") not in (None, "none") and f.get("fps"):
                return int(round(f["fps"]))
    if info.get("fps"):
        return int(round(info["fps"]))
    fmts = info.get("formats")
    if isinstance(fmts, list):
        fps_vals = [f.get("fps") for f in fmts if f and f.get("fps")]
        if fps_vals:
            return int(round(max(fps_vals)))
    return None


def detect_max_resolution(info: dict) -> tuple[int, int] | None:
    rf = info.get("requested_formats")
    if isinstance(rf, list):
        best = None
        for f in rf:
            if not f or f.get("vcodec") in (None, "none"):
                continue
            w, h = f.get("width"), f.get("height")
            if w and h:
                wh = (int(w), int(h))
                if best is None or (wh[0] * wh[1]) > (best[0] * best[1]):
                    best = wh
        if best:
            return best
    if info.get("width") and info.get("height"):
        return int(info["width"]), int(info["height"])
    fmts = info.get("formats")
    if isinstance(fmts, list):
        best = None
        for f in fmts:
            if not f or f.get("vcodec") in (None, "none"):
                continue
            w, h = f.get("width"), f.get("height")
            if w and h:
                wh = (int(w), int(h))
                if best is None or (wh[0] * wh[1]) > (best[0] * best[1]):
                    best = wh
        if best:
            return best
    return None


class Streamer:
    def __init__(self, video_url: str, sender_name: str, max_resolution: tuple[int, int] | None = None, manual_resolution: tuple[int, int] | None = None, loop_vod: bool = False, log_cb=None, stop_cb=None):
        self.video_url = video_url
        self.sender_name = sender_name
        self.proc: subprocess.Popen | None = None
        self.stop_event = threading.Event()
        self.thread: threading.Thread | None = None
        self.frame_lock = threading.Lock()
        self.latest_frame_bgr: np.ndarray | None = None
        self.width = 1280
        self.height = 720
        self.detected_fps = 30
        self.is_live = False
        self.http_headers = {}
        self.stream_url = None
        self.spout: SpoutGL.SpoutSender | None = None
        # Resolution options
        self.max_resolution = max_resolution
        self.manual_resolution = manual_resolution
        self.loop_vod = loop_vod
        self._log_cb = log_cb
        self._stop_cb = stop_cb

    def log(self, msg: str):
        try:
            if self._log_cb:
                self._log_cb(msg)
            else:
                print(msg)
        except Exception:
            pass

    def _yt_refresh(self) -> bool:
        # 環境に応じた最適なフォーマット文字列を取得
        format_str, codec_info = get_optimal_format_string()
        self.log(f"コーデック対応状況: {codec_info}")
        
        # カスタムログハンドラーを作成
        class YtDlpLogger:
            def debug(self, msg):
                if msg.startswith('[debug]'):
                    return  # デバッグメッセージは無視
                self.log(f"yt-dlp: {msg}")
            
            def info(self, msg):
                self.log(f"yt-dlp: {msg}")
            
            def warning(self, msg):
                self.log(f"yt-dlp 警告: {msg}")
            
            def error(self, msg):
                self.log(f"yt-dlp エラー: {msg}")
        
        # ログハンドラーのインスタンスを作成し、selfのlogメソッドを設定
        logger = YtDlpLogger()
        logger.log = self.log
        
        ydl_opts = {
            # 環境に応じて動的に決定されたフォーマット
            'format': format_str,
            'noplaylist': True,
            'logger': logger,  # カスタムログハンドラーを設定
            # SSL証明書問題の回避
            'nocheckcertificate': True,
            # 追加のネットワーク設定
            'socket_timeout': 30,
            'retries': 3,
            # User-Agentを設定してブロック回避
            'user_agent': 'Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36',
        }
        
        # certifiパッケージが利用可能な場合は使用
        try:
            import certifi
            ydl_opts['ca_certs'] = certifi.where()
            # nocheckcertificateを無効にして適切な証明書検証を行う
            ydl_opts['nocheckcertificate'] = False
        except ImportError:
            # certifiが無い場合はnocheckcertificateを維持
            pass
        try:
            # PyInstallerでの実行時にyt-dlpの問題を回避
            if getattr(sys, 'frozen', False):
                # 実行ファイル内でのyt-dlp実行時の設定
                ydl_opts['extract_flat'] = False
                ydl_opts['no_warnings'] = True
            
            with yt_dlp.YoutubeDL(ydl_opts) as ydl:
                info = ydl.extract_info(self.video_url, download=False)
        except Exception as e:
            self.log(f"yt-dlp 取得失敗: {e}")
            return False
        # URL は info.url を優先し、無ければ requested_formats の video 側から取得
        self.stream_url = info.get("url")
        self.http_headers = info.get("http_headers", {})
        if not self.stream_url:
            rf = info.get("requested_formats")
            if isinstance(rf, list):
                for f in rf:
                    if f and f.get("vcodec") not in (None, "none") and f.get("url"):
                        self.stream_url = f.get("url")
                        self.http_headers = f.get(
                            "http_headers", {}) or self.http_headers
                        break
        self.is_live = bool(info.get("is_live"))
        fps = detect_fps(info)
        if fps:
            self.detected_fps = max(MIN_FPS, min(MAX_FPS, fps))
        # 選択されたフォーマット情報をログ出力
        selected_format = info.get('format_id', 'unknown')
        vcodec = info.get('vcodec', 'unknown')
        resolution = f"{info.get('width', '?')}x{info.get('height', '?')}"
        self.log(f"選択されたフォーマット: {selected_format}, コーデック: {vcodec}, 解像度: {resolution}")

        wh = detect_max_resolution(info)
        if wh:
            w, h = wh
            # Apply max cap if provided
            if self.max_resolution:
                maxw, maxh = self.max_resolution
                if w > maxw or h > maxh:
                    rw = maxw / w
                    rh = maxh / h
                    scale = min(rw, rh)
                    w = int(w * scale)
                    h = int(h * scale)
            # Apply manual override if provided
            if self.manual_resolution:
                mw, mh = self.manual_resolution
                if mw and mh:
                    w, h = int(mw), int(mh)
            self.width, self.height = w, h
        return self.stream_url is not None

    def _start_ffmpeg(self):
        user_agent = self.http_headers.get(
            "User-Agent") or self.http_headers.get("user-agent")
        ffmpeg_path = find_ffmpeg_path()
        cmd = [
            ffmpeg_path,
            "-loglevel", "warning",
            "-re",
        ]
        # VODループ時はffmpeg側でループ処理
        if not self.is_live and self.loop_vod:
            cmd += [
                "-stream_loop", "-1",  # 無限ループ
                "-rw_timeout", "10000000",  # 10秒に延長
            ]
        # Reconnect flags: live のみ
        elif self.is_live:
            cmd += [
                "-reconnect", "1",
                "-reconnect_at_eof", "1",
                "-reconnect_streamed", "1",
                "-reconnect_on_network_error", "1",
                "-rw_timeout", "10000000",  # 10秒に延長
                "-reconnect_delay_max", "5",
            ]
        else:
            # VOD 時（ループなし）は短い読みタイムアウト
            cmd += [
                "-rw_timeout", "5000000",  # 5秒に延長
            ]
        cmd += [
            # 入力前のグローバル設定
            "-fflags", "+genpts+discardcorrupt+igndts",
            "-avoid_negative_ts", "make_zero",
            # プロトコル設定
            "-protocol_whitelist", "file,crypto,data,concat,subfile,http,https,tcp,tls,pipe",
            # バッファリング設定（入力用）
            "-probesize", "32M",
            "-analyzeduration", "10M",
        ]
        if user_agent:
            cmd += ["-user_agent", user_agent]
        cmd += [
            *build_ffmpeg_header_args(self.http_headers),
            "-i", self.stream_url,
            # 入力後の処理設定
            "-err_detect", "ignore_err",
            "-ignore_unknown",
            # 出力設定
            "-max_muxing_queue_size", "1024",  # 出力オプションなので入力後に配置
            "-threads", "0",  # 自動スレッド数
            # スケーリングとフレームレート設定
            "-vf", f"scale={self.width}:{self.height}:flags=lanczos",
            "-r", str(self.detected_fps),
            # 出力フォーマット
            "-f", "rawvideo",
            "-pix_fmt", "bgr24",
            "pipe:1",
        ]
        bufsize = self.width * self.height * 3
        # Windows で ffmpeg コンソールを非表示
        startupinfo = None
        creationflags = 0
        if hasattr(subprocess, 'STARTUPINFO') and hasattr(subprocess, 'STARTF_USESHOWWINDOW') and hasattr(subprocess, 'CREATE_NO_WINDOW'):
            startupinfo = subprocess.STARTUPINFO()
            startupinfo.dwFlags |= subprocess.STARTF_USESHOWWINDOW
            creationflags = subprocess.CREATE_NO_WINDOW
        return subprocess.Popen(
            cmd,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            stdin=subprocess.DEVNULL,
            bufsize=bufsize,
            startupinfo=startupinfo,
            creationflags=creationflags,
        )

    def start(self):
        if self.thread and self.thread.is_alive():
            return
        self.stop_event.clear()
        # _yt_refresh()を_run()内で実行するように変更
        self.thread = threading.Thread(target=self._run, daemon=True)
        self.thread.start()

    def stop(self):
        self.stop_event.set()
        if self.thread:
            self.thread.join(timeout=2)
        self.thread = None

    def _run(self):
        """メインループ"""
        # 最初にストリームURL取得
        if not self._yt_refresh():
            self.log("初期のストリームURL取得に失敗しました。")
            if self.stop_cb:
                self.stop_cb()
            return
        
        self.proc = self._start_ffmpeg()
        if not self.proc or not self.proc.stdout:
            self.log("ffmpeg の起動に失敗しました。")
            return
        # Spout init
        self.spout = SpoutGL.SpoutSender()
        self.spout.createOpenGL()
        self.spout.setSenderName(self.sender_name)
        self.log(
            f"{self.sender_name} で送信を開始しました。({self.width}x{self.height}) @ {self.detected_fps}fps")

        frame_size = self.width * self.height * 3
        consecutive_failures = 0
        last_frame_time = time.perf_counter()
        frame_interval = 1.0 / self.detected_fps
        
        # URL更新管理
        last_refresh = time.time()
        refresh_interval = 300  # 5分間隔でURL更新

        # ffmpeg stderr reader thread for logs
        def _read_stderr(proc, cb):
            try:
                while proc and proc.stderr and not self.stop_event.is_set():
                    line = proc.stderr.readline()
                    if not line:
                        break
                    txt = line.decode('utf-8', errors='ignore').strip()
                    if txt:
                        cb(f"ffmpeg: {txt}")
            except Exception:
                pass

        def start_stderr_thread():
            t = threading.Thread(target=_read_stderr, args=(
                self.proc, self.log), daemon=True)
            t.start()
            return t

        stderr_thread = start_stderr_thread()
        try:
            while not self.stop_event.is_set():
                # 定期的にストリームURLを更新
                current_time = time.time()
                if current_time - last_refresh > refresh_interval:
                    if not self._yt_refresh():
                        self.log("ストリームURL更新に失敗しました。")
                        break
                    last_refresh = current_time
                
                data = self.proc.stdout.read(frame_size)
                # プロセス終了や EOF を検知
                if (self.proc.poll() is not None) or (not data or len(data) < frame_size):
                    if not self.is_live and self.loop_vod:
                        # ffmpeg側でループしているので、予期しない終了の場合のみ再起動
                        self.log("ffmpeg側ループが予期せず終了。再起動中...")
                        try:
                            self.proc.kill()
                            self.proc.wait(timeout=1)
                        except Exception:
                            pass
                        if not self._yt_refresh():
                            time.sleep(1.0)
                            consecutive_failures += 1
                            if consecutive_failures >= MAX_CONSECUTIVE_FAILURES:
                                self.log("連続失敗で停止します。")
                                break
                            continue
                        self.proc = self._start_ffmpeg()
                        if not self.proc or not self.proc.stdout:
                            time.sleep(1.0)
                            consecutive_failures += 1
                            if consecutive_failures >= MAX_CONSECUTIVE_FAILURES:
                                self.log("ffmpeg 再起動失敗が続いたため停止します。")
                                break
                            continue
                        # 新しい stderr スレッドを開始
                        stderr_thread = start_stderr_thread()
                        last_frame_time = time.perf_counter()
                        continue
                    elif not self.is_live:
                        # VOD終了（ループなし）
                        self.log("VOD 終了のため停止します。")
                        # GUI側に停止を通知
                        if self._stop_cb:
                            try:
                                self._stop_cb()
                            except Exception:
                                pass
                        break
                    else:
                        # ライブストリーム再起動
                        self.log("ライブストリーム再接続中...")
                        self.proc.kill()
                        try:
                            self.proc.wait(timeout=1)
                        except Exception:
                            pass
                        if not self._yt_refresh():
                            time.sleep(1.0)
                            consecutive_failures += 1
                            if consecutive_failures >= MAX_CONSECUTIVE_FAILURES:
                                self.log("連続失敗で停止します。")
                                break
                            continue
                        self.proc = self._start_ffmpeg()
                        if not self.proc or not self.proc.stdout:
                            time.sleep(1.0)
                            consecutive_failures += 1
                            if consecutive_failures >= MAX_CONSECUTIVE_FAILURES:
                                self.log("ffmpeg 再起動失敗が続いたため停止します。")
                                break
                            continue
                        # 新しい stderr スレッドを開始
                        stderr_thread = start_stderr_thread()
                        continue

                if consecutive_failures:
                    consecutive_failures = 0
                now = time.perf_counter()
                elapsed = now - last_frame_time
                if elapsed < frame_interval:
                    time.sleep(frame_interval - elapsed)
                last_frame_time = time.perf_counter()

                frame = np.frombuffer(data, dtype=np.uint8)
                frame = frame.reshape((self.height, self.width, 3))
                with self.frame_lock:
                    self.latest_frame_bgr = frame

                # Spout send
                gl_format = SpoutGL.enums.GL_BGR_EXT
                bpp = SpoutGL.helpers.getBytesPerPixel(gl_format)
                self.spout.sendImage(
                    frame.tobytes(), self.width, self.height, gl_format, False, bpp)
        finally:
            try:
                if self.proc:
                    self.proc.kill()
                    self.proc.wait(timeout=1)
            except Exception:
                pass
            if self.spout:
                self.spout.releaseSender()


class App:
    def __init__(self, root: ctk.CTk):
        self.root = root
        self.root.title("ytdlpSpout GUI")
        self.streamer: Streamer | None = None
        
        # CustomTkinterの外観設定
        ctk.set_appearance_mode("dark")  # "dark" or "light"
        ctk.set_default_color_theme("blue")  # "blue", "green", "dark-blue"
        
        # 美しい日本語フォントを設定
        self.japanese_font = get_best_japanese_font()
        self.monospace_font = get_best_monospace_font()
        
        # ログ用フォント：日本語が多い場合は日本語フォントを優先
        self.log_font = self.japanese_font  # 日本語ログが多いので日本語フォントを使用
        
        # デバッグ用：選択されたフォントを後でログに出力
        self._selected_fonts = {
            'japanese': self.japanese_font,
            'monospace': self.monospace_font,
            'log': self.log_font
        }
        
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

        self.btn_start = ctk.CTkButton(frm, text="Start", command=self.on_start, 
                                      font=ctk.CTkFont(family=self.japanese_font, size=12, weight="bold"))
        self.btn_stop = ctk.CTkButton(
            frm, text="Stop", command=self.on_stop, state="disabled",
            font=ctk.CTkFont(family=self.japanese_font, size=12, weight="bold"))
        self.btn_start.grid(row=1, column=2, padx=6, pady=6)
        self.btn_stop.grid(row=1, column=3, padx=6, pady=6)

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
        self.root.after(33, self.update_preview)
        
        # フォント情報をログに出力
        self.root.after(500, self._log_font_info)

    def log(self, msg: str):
        try:
            self.log_text.insert("end", msg + "\n")
            self.log_text.see("end")
        except Exception:
            pass
    

    def _log_font_info(self):
        """選択されたフォント情報をログに出力"""
        try:
            self.log(f"選択されたフォント - 日本語UI: {self._selected_fonts['japanese']}, ログ: {self._selected_fonts['log']}, 等幅: {self._selected_fonts['monospace']}")
        except Exception:
            pass

    def on_start(self):
        if self.streamer:
            return
        
        # UIを即座に更新
        self.btn_start.configure(state="disabled")
        self.btn_stop.configure(state="normal")
        self.info_label.configure(text="動画情報を取得中...")
        self.log("ストリーミング開始準備中...")
        
        # プレビュー表示をリセット
        if hasattr(self, '_no_signal_shown'):
            delattr(self, '_no_signal_shown')
        self.preview_label.configure(text="")
        
        # パラメータを取得
        url = self.url_var.get().strip()
        sender = self.sender_var.get().strip() or DEFAULT_SENDER_NAME
        maxw = self.maxw_var.get().strip()
        maxh = self.maxh_var.get().strip()
        manw = self.manw_var.get().strip()
        manh = self.manh_var.get().strip()
        max_res = None
        manual_res = None
        
        # 1440p制限の処理（他の設定より優先度低）
        if self.perf_limit.get() and not self.max_enable.get() and not self.manual_enable.get():
            max_res = (2560, 1440)
            self.log("1440p制限を適用しました (パフォーマンス重視)")
        
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
        def start_streaming():
            try:
                self.streamer = Streamer(
                    url,
                    sender,
                    max_resolution=max_res,
                    manual_resolution=manual_res,
                    loop_vod=self.vod_loop.get(),
                    log_cb=lambda m: self.root.after(0, self.log, m),
                    stop_cb=lambda: self.root.after(0, self.on_auto_stop),
                )
                self.streamer.start()
                # 成功時のUI更新
                self.root.after(0, lambda: self.info_label.configure(text="ストリーミング開始"))
            except Exception as e:
                # エラー時のUI更新
                self.root.after(0, lambda: self._handle_start_error(str(e)))
        
        # 別スレッドで実行
        threading.Thread(target=start_streaming, daemon=True).start()

    def on_stop(self):
        if not self.streamer:
            return
        self.streamer.stop()
        self.streamer = None
        self.btn_start.configure(state="normal")
        self.btn_stop.configure(state="disabled")
        self.info_label.configure(text="Stopped")

    def _handle_start_error(self, error_msg: str):
        """ストリーミング開始エラーの処理"""
        self.log(f"ストリーミング開始エラー: {error_msg}")
        self.streamer = None
        self.btn_start.configure(state="normal")
        self.btn_stop.configure(state="disabled")
        self.info_label.configure(text="開始に失敗しました")

    def on_auto_stop(self):
        """動画終了時の自動停止処理"""
        if self.streamer:
            self.streamer.stop()
            self.streamer = None
        self.btn_start.configure(state="normal")
        self.btn_stop.configure(state="disabled")
        self.info_label.configure(text="動画再生完了")

    def on_close(self):
        try:
            self.on_stop()
        finally:
            self.root.destroy()

    def update_preview(self):
        try:
            if self.streamer and self.streamer.latest_frame_bgr is not None:
                with self.streamer.frame_lock:
                    frame = self.streamer.latest_frame_bgr
                rgb = cv2.cvtColor(frame, cv2.COLOR_BGR2RGB)
                
                # プレビューエリアのサイズを取得
                self.preview_label.update_idletasks()
                label_width = self.preview_label.winfo_width()
                label_height = self.preview_label.winfo_height()
                
                # 最小サイズを確保
                if label_width < 100:
                    label_width = 640
                if label_height < 100:
                    label_height = 360
                
                # アスペクト比を維持してリサイズ
                h, w, _ = rgb.shape
                scale_w = label_width / float(w)
                scale_h = label_height / float(h)
                scale = min(scale_w, scale_h)
                
                new_w = int(w * scale)
                new_h = int(h * scale)
                
                if new_w > 0 and new_h > 0:
                    dst = cv2.resize(rgb, (new_w, new_h))
                    img = Image.fromarray(dst)
                    # CustomTkinter用のCTkImageを作成
                    self.preview_imgtk = ctk.CTkImage(light_image=img, dark_image=img, size=(new_w, new_h))
                    self.preview_label.configure(image=self.preview_imgtk, text="")
                    self._no_signal_shown = False
            else:
                # ストリーミング停止時は黒画面を表示
                if not hasattr(self, '_no_signal_shown') or not self._no_signal_shown:
                    self.preview_label.configure(image="", text="No Signal", 
                                               text_color="white")
                    self._no_signal_shown = True
                    
            # 解像度情報の更新
            if self.streamer:
                self.info_label.configure(
                    text=f"Resolution: {self.streamer.width}x{self.streamer.height} @ {self.streamer.detected_fps}fps")
            else:
                self.info_label.configure(text="No stream active")
        finally:
            self.root.after(33, self.update_preview)


if __name__ == "__main__":
    root = ctk.CTk()
    app = App(root)
    root.mainloop()
