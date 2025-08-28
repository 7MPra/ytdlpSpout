
import subprocess
import threading
import time
import os
import sys
import ssl
import cv2
import numpy as np
import SpoutGL
import yt_dlp
from typing import Optional, Tuple, Callable, Dict, Any

# SSL証明書の設定（Windows環境での証明書問題を回避）
try:
    ssl._create_default_https_context = ssl._create_unverified_context
except Exception:
    pass

# デフォルト設定
DEFAULT_VIDEO_URL = "https://www.youtube.com/watch?v=dQw4w9WgXcQ"
DEFAULT_SENDER_NAME = "ytdlpSpoutSender"
DEFAULT_WIDTH = 1920
DEFAULT_HEIGHT = 1080
DEFAULT_FPS = 30
MAX_CONSECUTIVE_FAILURES = 3
MIN_FPS, MAX_FPS = 15, 120

def get_executable_dir() -> str:
    """実行ファイルのディレクトリを取得"""
    if getattr(sys, 'frozen', False):
        return os.path.dirname(sys.executable)
    else:
        # __file__ がない場合（インタラクティブモードなど）を考慮
        try:
            return os.path.dirname(os.path.abspath(__file__))
        except NameError:
            return os.getcwd()

def find_ffmpeg_path() -> str:
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

def check_av1_support() -> Tuple[bool, list[str]]:
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
            decoders_output = result.stdout.lower()
            av1_decoders = ['libdav1d', 'libaom-av1', 'av1']
            supported_decoders = [decoder for decoder in av1_decoders if decoder in decoders_output]
            
            if supported_decoders:
                return True, supported_decoders
            else:
                return False, []
        else:
            return False, []
            
    except Exception:
        return False, []

def get_optimal_format_string() -> Tuple[str, str]:
    """環境に応じた最適なフォーマット文字列を生成"""
    av1_supported, av1_decoders = check_av1_support()
    
    fallback_formats = 'bestvideo+bestaudio/best'

    if av1_supported:
        format_str = (
            'bestvideo[height<=2160][height>=720]+bestaudio/'
            'bestvideo[vcodec!*=av01][height<=1440][height>=720]+bestaudio/'  # AV1が重い場合のフォールバック
            'best[height<=2160][height>=720]/'
            'bestvideo[height>=720]+bestaudio/'
            f'{fallback_formats}'
        )
        codec_info = f"AV1 supported (decoders: {', '.join(av1_decoders)}, up to 2160p)"
    else:
        format_str = (
            'bestvideo[vcodec!*=av01][height<=2160][height>=720]+bestaudio/'
            'bestvideo[vcodec!*=av01][height>=720]+bestaudio/'
            'best[vcodec!*=av01][height>=720]/'
            'bestvideo[height>=720]+bestaudio/'
            f'{fallback_formats}'
        )
        codec_info = "AV1 not supported (H.264/VP9 preferred, up to 2160p)"
    
    return format_str, codec_info

def build_ffmpeg_header_args(headers: dict) -> list[str]:
    """ffmpegに渡すHTTPヘッダーを構築する"""
    if not headers:
        return []
    
    header_lines = [f"{k}: {v}" for k, v in headers.items()]
    header_string = "\r\n".join(header_lines) + "\r\n"
    
    return ["-headers", header_string]

def detect_fps(info: Dict[str, Any]) -> Optional[int]:
    """yt-dlpの情報からFPSを検出する"""
    rf = info.get("requested_formats")
    if isinstance(rf, list):
        for f in rf:
            if f and f.get("vcodec") not in (None, "none") and f.get("fps"):
                return int(round(f["fps"]))
    if info.get("fps"):
        return int(round(info["fps"]))
    fmts = info.get("formats")
    if isinstance(fmts, list):
        fps_vals = [f.get("fps") for f in fmts if f and f.get("fps") ]
        if fps_vals:
            return int(round(max(fps_vals)))
    return None

def detect_max_resolution(info: Dict[str, Any]) -> Optional[Tuple[int, int]]:
    """yt-dlpの情報から最大解像度を検出する"""
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
    def __init__(self, video_url: str, sender_name: str, max_resolution: tuple[int, int] | None = None, manual_resolution: tuple[int, int] | None = None, loop_vod: bool = False, verbose = True, log_cb=None, stop_cb=None):
        self.video_url = video_url
        self.sender_name = sender_name
        self.proc: subprocess.Popen | None = None
        self.stop_event = threading.Event()
        self.thread: threading.Thread | None = None
        self.frame_lock = threading.Lock()
        self.latest_frame_bgr: np.ndarray | None = None
        self.width = DEFAULT_WIDTH
        self.height = DEFAULT_HEIGHT
        self.detected_fps = DEFAULT_FPS
        self.is_live = False
        self.is_vod = False
        self.duration = 0
        self.playback_time = 0
        self.http_headers = {}
        self.stream_url = None
        self.spout: SpoutGL.SpoutSender | None = None
        self.max_resolution = max_resolution
        self.manual_resolution = manual_resolution
        self.loop_vod = loop_vod
        self._log_cb = log_cb
        self._stop_cb = stop_cb
        self.verbose = verbose
        self.console_log = False
        self.seek_lock = threading.Lock()
        self.seek_request = -1.0


    def log(self, msg: str):
        try:
            if self._log_cb:
                self._log_cb(msg)
            else:
                print(msg)
        except Exception:
            pass

    def _yt_refresh(self) -> bool:
        # dataディレクトリを作成（存在しない場合）
        os.makedirs("data", exist_ok=True)
        cookie_file = os.path.join("data", "cookies.txt")

        # 環境に応じた最適なフォーマット文字列を取得
        format_str, codec_info = get_optimal_format_string()
        if self.verbose:
            self.log(f"コーデック対応状況: {codec_info}")
            self.log(f"Cookieファイルとして'{cookie_file}'を使用します。")
        
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
            'quiet': not self.verbose,
            # SSL証明書問題の回避
            'nocheckcertificate': True,
            # 追加のネットワーク設定
            'socket_timeout': 30,
            'retries': 3,
            # User-Agentを設定してブロック回避
            'user_agent': 'Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36',
            # Cookieファイルを強制的に指定
            'cookiefile': cookie_file,
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
            # PyInstallerでの実行時にyt-dlpの問題を回避
            if getattr(sys, 'frozen', False):
                # 実行ファイル内でのyt-dlp実行時の設定
                ydl_opts['extract_flat'] = False
                ydl_opts['no_warnings'] = True

        try:
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

        # --- Cookieヘッダーの手動挿入 ---
        # yt-dlpがhttp_headersにCookieを含めない問題への対策
        try:
            import http.cookiejar
            import urllib.parse

            cj = http.cookiejar.MozillaCookieJar(cookie_file)
            cj.load(ignore_discard=True, ignore_expires=True)

            if self.stream_url:
                parsed_url = urllib.parse.urlparse(self.stream_url)
                domain = parsed_url.netloc
                
                cookie_dict = {}
                for cookie in cj:
                    if cookie.domain and domain.endswith(cookie.domain):
                        cookie_dict[cookie.name] = cookie.value
                
                if cookie_dict:
                    cookie_header_val = "; ".join([f"{k}={v}" for k, v in cookie_dict.items()])
                    self.http_headers['Cookie'] = cookie_header_val
                    self.log("Cookieを手動で解析し、ffmpegヘッダーに追加しました。")
        except Exception as e:
            if self.verbose:
                self.log(f"Cookieファイルの手動解析に失敗: {e}")
        # --- 手動挿入ここまで ---

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
            "-loglevel", "warning" if not (self.verbose and self.console_log) else "info",
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
        if self.verbose:
            cmd_str = ' '.join([f'"{arg}"' if ' ' in arg else arg for arg in cmd])
            self.log(f"ffmpegコマンド: {cmd_str}")
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
        # _yt_refresh()をrun()内で実行するように変更
        self.thread = threading.Thread(target=self.run, daemon=True)
        self.thread.start()

    def stop(self):
        self.stop_event.set()
        if self.thread:
            self.thread.join(timeout=2)
        self.thread = None

    def run(self):
        """メインループ"""
        # 最初にストリームURL取得
        if not self._yt_refresh():
            self.log("初期のストリームURL取得に失敗しました。")
            if self.stop_cb:
                self.stop_cb()
            return False
        
        self.proc = self._start_ffmpeg()
        if not self.proc or not self.proc.stdout:
            self.log("ffmpeg の起動に失敗しました。")
            return False
        
        # プロセス起動直後の状態確認
        time.sleep(0.1)  # 少し待ってプロセスの状態を確認
        if self.proc.poll() is not None:
            exit_code = self.proc.poll()
            self.log(f"ffmpegプロセスが即座に終了しました。終了コード: {exit_code}")
            # stderrから詳細なエラー情報を取得
            if self.proc.stderr:
                try:
                    stderr_output = self.proc.stderr.read().decode('utf-8', errors='ignore')
                    if stderr_output.strip():
                        self.log(f"ffmpegエラー詳細: {stderr_output}")
                except Exception:
                    pass
            return False
        
        # Spout init
        self.spout = SpoutGL.SpoutSender()
        self.spout.createOpenGL()
        self.spout.setSenderName(self.sender_name)
        self.log(
            f"{self.sender_name} で送信を開始しました。({self.width}x{self.height}) @ {self.detected_fps}fps")

        if self.is_live:
            self.log("ライブストリームを検出しました。")
        else:
            self.log(f"VOD を検出しました。{'ループ再生' if self.loop_vod else '1回再生'}します。")

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
                    # プロセス終了コードをチェック
                    if self.proc.poll() is not None:
                        exit_code = self.proc.poll()
                        self.log(f"ffmpegプロセス終了コード: {exit_code}")
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
        except KeyboardInterrupt:
            self.log("終了します。")
        finally:
            self.cleanup()

    def cleanup(self):
        """リソースのクリーンアップ"""
        try:
            if self.proc:
                self.proc.kill()
                self.proc.wait(timeout=1)
        except Exception:
            pass
        
        if self.spout:
            self.spout.releaseSender()