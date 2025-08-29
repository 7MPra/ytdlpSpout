
import subprocess
import threading
import time
import os
import sys
import ssl
import cv2
import av
import numpy as np
import SpoutGL
import yt_dlp
import json
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

def find_ffmpeg_path(tool: str = 'ffmpeg') -> str:
    """ffmpegまたはffprobeのパスを検索"""
    exe_dir = get_executable_dir()
    tool_name = f"{tool}.exe" if sys.platform == "win32" else tool

    # 1. exe同階層のbinディレクトリを確認
    bin_dir = os.path.join(exe_dir, 'bin')
    tool_in_bin = os.path.join(bin_dir, tool_name)
    if os.path.exists(tool_in_bin):
        return tool_in_bin
    
    # 2. exe同階層を確認
    tool_in_exe_dir = os.path.join(exe_dir, tool_name)
    if os.path.exists(tool_in_exe_dir):
        return tool_in_exe_dir
    
    # 3. システムPATHから検索
    return tool # 見つからなければ名前だけ返す

def check_av1_support() -> Tuple[bool, list[str]]:
    """ffmpegでAV1デコードがサポートされているかチェック"""
    try:
        ffmpeg_path = find_ffmpeg_path('ffmpeg')
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
            'bestvideo[height<=2160][height>=720]+bestaudio/ப்புகளை'
            'bestvideo[vcodec!*=av01][height<=1440][height>=720]+bestaudio/ப்புகளை'  # AV1が重い場合のフォールバック
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
    def __init__(self, video_url, sender_name, 
                 max_resolution=None, 
                 manual_resolution=None, 
                 loop_vod=False, verbose=True, 
                 log_cb=None, 
                 stop_cb=None,
                 init_ok_cb=None):
        self.video_url = video_url
        self.sender_name = sender_name
        self.proc = None
        self.container = None  # PyAV用
        self.video_stream = None
        self.av_frame_gen = None
        self.stop_event = threading.Event()
        self.thread = None
        self.frame_lock = threading.Lock()
        self.latest_frame_bgr = None
        self.width = DEFAULT_WIDTH
        self.height = DEFAULT_HEIGHT
        self.detected_fps = DEFAULT_FPS
        self.is_live = False
        self.is_vod = False
        self.duration = 0.0
        self.playback_time = 0.0
        self.http_headers = {}
        self.stream_url = None
        self.max_resolution = max_resolution
        self.manual_resolution = manual_resolution
        self.loop_vod = loop_vod
        self._log_cb = log_cb
        self._stop_cb = stop_cb
        self._init_ok_cb = init_ok_cb
        self.verbose = verbose
        self.console_log = False
        self.seek_lock = threading.Lock()
        self.seek_request = -1.0
        self.is_local_file = os.path.exists(video_url) and os.path.isfile(video_url)

    def log(self, msg: str):
        try:
            if self._log_cb:
                self._log_cb(msg)
            else:
                print(msg)
        except Exception:
            pass


    def _get_local_file_info(self) -> bool:
        """ffprobeを使ってローカルファイルの情報を取得"""
        try:
            ffprobe_path = find_ffmpeg_path('ffprobe')
            cmd = [
                ffprobe_path,
                '-v', 'quiet',
                '-print_format', 'json',
                '-show_format', '-show_streams',
                self.video_url
            ]
            self.log(f"ffprobeでファイル情報を取得: {' '.join(cmd)}")
            result = subprocess.run(cmd, capture_output=True, text=True, check=True, creationflags=subprocess.CREATE_NO_WINDOW)
            info = json.loads(result.stdout)

            video_stream = next((s for s in info['streams'] if s['codec_type'] == 'video'), None)
            if not video_stream:
                self.log("エラー: ファイル内に映像ストリームが見つかりません。")
                return False

            # 解像度
            self.width = int(video_stream['width'])
            self.height = int(video_stream['height'])

            # FPS
            fps_str = video_stream.get('avg_frame_rate', '30/1')
            if '/' in fps_str:
                num, den = fps_str.split('/')
                try:
                    num = float(num)
                    den = float(den)
                    self.detected_fps = round(num / den) if den != 0 else 30
                except Exception:
                    self.detected_fps = 30
            else:
                try:
                    self.detected_fps = round(float(fps_str))
                except Exception:
                    self.detected_fps = 30
            self.detected_fps = max(MIN_FPS, min(MAX_FPS, self.detected_fps))

            # 長さ
            self.duration = float(info['format'].get('duration', 0.0))

            self.is_vod = True
            self.is_live = False
            self.stream_url = self.video_url # ストリームURLはファイルパスそのもの

            self.log(f"ファイル情報: {self.width}x{self.height} @ {self.detected_fps}fps, 長さ: {self.duration:.2f}s")
            return True

        except Exception as e:
            self.log(f"ffprobeでのファイル情報取得に失敗: {e}")
            if self._stop_cb:
                self._stop_cb()
            return False


    def _yt_refresh(self) -> bool:
        # ... (このメソッドは変更なし)
        os.makedirs("data", exist_ok=True)
        cookie_file = os.path.join("data", "cookies.txt")
        format_str, codec_info = get_optimal_format_string()
        if self.verbose:
            self.log(f"コーデック対応状況: {codec_info}")
            self.log(f"Cookieファイルとして'{cookie_file}'を使用します。")

        class YtDlpLogger:
            def debug(self, msg):
                if msg.startswith('[debug]'): return
                self.log(f"yt-dlp: {msg}")
            def info(self, msg): self.log(f"yt-dlp: {msg}")
            def warning(self, msg): self.log(f"yt-dlp 警告: {msg}")
            def error(self, msg): self.log(f"yt-dlp エラー: {msg}")

        logger = YtDlpLogger()
        logger.log = self.log

        ydl_opts = {
            'format': format_str,
            'noplaylist': True,
            'logger': logger,
            'quiet': not self.verbose,
            'nocheckcertificate': True,
            'socket_timeout': 30,
            'retries': 3,
            'user_agent': 'Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36',
            'cookiefile': cookie_file,
        }

        try:
            with yt_dlp.YoutubeDL(ydl_opts) as ydl:
                info = ydl.extract_info(self.video_url, download=False)
        except Exception as e:
            self.log(f"yt-dlp 取得失敗: {e}")
            return False

        self.stream_url = info.get("url")
        self.http_headers = info.get("http_headers", {})

        if not self.stream_url:
            rf = info.get("requested_formats")
            if isinstance(rf, list):
                for f in rf:
                    if f and f.get("vcodec") not in (None, "none") and f.get("url"):
                        self.stream_url = f.get("url")
                        self.http_headers = f.get("http_headers", {}) or self.http_headers
                        break

        self.is_live = bool(info.get("is_live"))
        self.duration = info.get('duration', 0.0)
        self.is_vod = not self.is_live and self.duration > 0

        fps = detect_fps(info)
        if fps:
            self.detected_fps = max(MIN_FPS, min(MAX_FPS, fps))

        selected_format = info.get('format_id', 'unknown')
        vcodec = info.get('vcodec', 'unknown')
        resolution = f"{info.get('width', '?')}x{info.get('height', '?')}"
        self.log(f"選択されたフォーマット: {selected_format}, コーデック: {vcodec}, 解像度: {resolution}")

        wh = detect_max_resolution(info)
        if wh:
            w, h = wh
            if self.max_resolution:
                maxw, maxh = self.max_resolution
                if w > maxw or h > maxh:
                    scale = min(maxw / w, maxh / h)
                    w, h = int(w * scale), int(h * scale)
            if self.manual_resolution:
                mw, mh = self.manual_resolution
                if mw and mh: w, h = int(mw), int(mh)
            self.width, self.height = w, h
        return self.stream_url is not None

    def _start_ffmpeg(self, start_time_sec: float = 0.0):
        ffmpeg_path = find_ffmpeg_path('ffmpeg')
        cmd = [
            ffmpeg_path,
            "-loglevel", "warning" if not (self.verbose and self.console_log) else "info",
        ]

        # --- 入力設定 ---
        if start_time_sec > 0:
            cmd += ["-ss", str(start_time_sec)]

        if self.is_local_file:
            if self.loop_vod and start_time_sec == 0: # シーク中はループを無効
                cmd += ["-stream_loop", "-1"]
        else: # ネットワークストリームの場合
            if not self.is_live and self.loop_vod and start_time_sec == 0:
                cmd += ["-stream_loop", "-1"]
                cmd += ["-rw_timeout", "10000000"]
            elif self.is_live:
                cmd += [
                    "-reconnect", "1", "-reconnect_at_eof", "1",
                    "-reconnect_streamed", "1", "-reconnect_on_network_error", "1",
                    "-rw_timeout", "10000000", "-reconnect_delay_max", "5",
                ]
            else: # VOD (ループなし)
                cmd += ["-rw_timeout", "5000000"]

            cmd += [
                "-fflags", "+genpts+discardcorrupt+igndts",
                "-avoid_negative_ts", "make_zero",
                "-protocol_whitelist", "file,crypto,data,concat,subfile,http,https,tcp,tls,pipe",
                "-probesize", "32M",
                "-analyzeduration", "10M",
            ]
            user_agent = self.http_headers.get("User-Agent") or self.http_headers.get("user-agent")
            if user_agent:
                cmd += ["-user_agent", user_agent]
            cmd += build_ffmpeg_header_args(self.http_headers)

        cmd += ["-i", self.stream_url]
        
        # --- 出力設定 ---
        cmd += [
            "-err_detect", "ignore_err",
            "-ignore_unknown",
            "-max_muxing_queue_size", "1024",
            "-threads", "0",
            "-vf", f"scale={self.width}:{self.height}:flags=lanczos",
            "-r", str(self.detected_fps),
            "-f", "rawvideo",
            "-pix_fmt", "bgr24",
            "pipe:1",
        ]
        
        bufsize = self.width * self.height * 3
        startupinfo = None
        creationflags = 0
        if sys.platform == "win32":
            startupinfo = subprocess.STARTUPINFO()
            startupinfo.dwFlags |= subprocess.STARTF_USESHOWWINDOW
            creationflags = subprocess.CREATE_NO_WINDOW
        
        if self.verbose:
            cmd_str = ' '.join([f'"{arg}"' if ' ' in arg else arg for arg in cmd])
            self.log(f"ffmpegコマンド: {cmd_str}")
            
        return subprocess.Popen(
            cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, stdin=subprocess.DEVNULL,
            bufsize=bufsize, startupinfo=startupinfo, creationflags=creationflags
        )

    def start(self):
        if self.thread and self.thread.is_alive():
            return
        self.stop_event.clear()
        self.thread = threading.Thread(target=self.run, daemon=True)
        self.thread.start()

    def stop(self):
        self.stop_event.set()
        if self.thread:
            self.thread.join(timeout=2)
        self.thread = None

    def run(self):
        """メインループ"""
        # 情報取得
        if self.is_local_file:
            if not self._get_local_file_info():
                self.log("ローカルファイルのメタデータ取得に失敗しました。")
                if self._stop_cb:
                    self._stop_cb()
                return False
            try:
                import av
                self.container = av.open(self.video_url)
                self.video_stream = self.container.streams.video[0]
                self.av_frame_gen = self.container.decode(self.video_stream)
            except Exception as e:
                self.log(f"PyAV初期化失敗: {e}")
                if self._stop_cb:
                    self._stop_cb()
                return False
            if self.av_frame_gen is None:
                self.log("PyAVフレームデコーダ初期化失敗: av_frame_gen is None")
                if self._stop_cb:
                    self._stop_cb()
                return False
            self.spout = SpoutGL.SpoutSender()
            self.spout.createOpenGL()
            self.spout.setSenderName(self.sender_name)
            self.log(f"{self.sender_name} で送信を開始しました。({self.width}x{self.height}) @ {self.detected_fps}fps")
            if self._init_ok_cb: self._init_ok_cb()
            self.log(f"VOD を検出しました。{'ループ再生' if self.loop_vod else '1回再生'}します。")
            frame_interval = 1.0 / self.detected_fps
            self.playback_time = 0.0
            last_frame_time = time.perf_counter()
            try:
                while not self.stop_event.is_set():
                    with self.seek_lock:
                        if self.seek_request >= 0:
                            seek_pos = self.seek_request
                            self.seek_request = -1.0
                            self.log(f"{seek_pos:.2f}秒へシークします... (PyAV)")
                            try:
                                pts = int(seek_pos / float(self.video_stream.time_base))
                                self.container.seek(pts, any_frame=False, backward=True, stream=self.video_stream)
                                self.av_frame_gen = self.container.decode(self.video_stream)
                                # シーク後、目的の時刻に到達するまでフレームをスキップ
                                # 最大30フレームだけ先読みして、seek_posに最も近いフレームを選ぶ
                                best_frame = None
                                best_time_diff = None
                                first_frame = None
                                for i, f in enumerate(self.av_frame_gen):
                                    if i == 0:
                                        first_frame = f
                                    if hasattr(f, 'time') and f.time is not None:
                                        diff = abs(f.time - seek_pos)
                                        if best_time_diff is None or diff < best_time_diff:
                                            best_time_diff = diff
                                            best_frame = f
                                        # 完全一致なら即決
                                        if diff < 0.01:
                                            break
                                    if i >= 30:
                                        break
                                frame = best_frame if best_frame is not None else first_frame
                                if frame is None:
                                    self.log("シーク後に有効なフレームが見つかりませんでした。")
                                    continue
                                self.playback_time = frame.time if hasattr(frame, 'time') and frame.time is not None else seek_pos
                                img = frame.to_ndarray(format='bgr24')
                                with self.frame_lock:
                                    self.latest_frame_bgr = img
                                self.spout.sendImage(img.tobytes(), self.width, self.height, SpoutGL.enums.GL_BGR_EXT, False, 3)
                                last_frame_time = time.perf_counter()
                                continue  # ループ先頭に戻る（以降の通常再生へ）
                            except Exception as e:
                                self.log(f"PyAVシーク失敗: {e}")
                    frame = None
                    try:
                        frame = next(self.av_frame_gen)
                    except StopIteration:
                        frame = None
                    except Exception as e:
                        self.log(f"PyAVフレーム取得失敗: {e}")
                        frame = None
                    if frame is None:
                        self.log("動画の終端または読み込み失敗。ループ再生判定...")
                        if self.loop_vod:
                            try:
                                self.container.seek(0, any_frame=False, backward=True, stream=self.video_stream)
                                self.av_frame_gen = self.container.decode(self.video_stream)
                                self.playback_time = 0.0
                                continue
                            except Exception as e:
                                self.log(f"PyAVループ失敗: {e}")
                                if self._stop_cb: self._stop_cb()
                                break
                        else:
                            if self._stop_cb: self._stop_cb()
                            break
                    img = frame.to_ndarray(format='bgr24')
                    now = time.perf_counter()
                    # 再生位置はフレームのタイムスタンプを優先
                    self.playback_time = frame.time if hasattr(frame, 'time') and frame.time is not None else self.playback_time + frame_interval
                    with self.frame_lock:
                        self.latest_frame_bgr = img
                    self.spout.sendImage(img.tobytes(), self.width, self.height, SpoutGL.enums.GL_BGR_EXT, False, 3)
                    elapsed = time.perf_counter() - last_frame_time
                    sleep_time = frame_interval - elapsed
                    if sleep_time > 0:
                        time.sleep(sleep_time)
                    last_frame_time = time.perf_counter()
            except KeyboardInterrupt:
                self.log("終了します。")
            finally:
                self.cleanup()
        else:
            if not self._yt_refresh():
                self.log("ストリームURL取得に失敗しました。")
                if self._stop_cb: self._stop_cb()
                return False
            self.proc = self._start_ffmpeg()
            if not self.proc or not self.proc.stdout:
                self.log("ffmpeg の起動に失敗しました。")
                if self._stop_cb: self._stop_cb()
                return False
            time.sleep(0.1)
            if self.proc.poll() is not None:
                exit_code = self.proc.poll()
                self.log(f"ffmpegプロセスが即座に終了しました。終了コード: {exit_code}")
                if self.proc.stderr:
                    try:
                        stderr_output = self.proc.stderr.read().decode('utf-8', errors='ignore')
                        if stderr_output.strip(): self.log(f"ffmpegエラー詳細: {stderr_output}")
                    except Exception: pass
                if self._stop_cb: self._stop_cb()
                return False
            # Spout init
            self.spout = SpoutGL.SpoutSender()
            self.spout.createOpenGL()
            self.spout.setSenderName(self.sender_name)
            self.log(f"{self.sender_name} で送信を開始しました。({self.width}x{self.height}) @ {self.detected_fps}fps")
            if self._init_ok_cb: self._init_ok_cb()
            if self.is_live:
                self.log("ライブストリームを検出しました。")
            else:
                self.log(f"VOD を検出しました。{'ループ再生' if self.loop_vod else '1回再生'}します。")
            frame_size = self.width * self.height * 3
            last_frame_time = time.perf_counter()
            frame_interval = 1.0 / self.detected_fps
            self.playback_time = 0.0
            start_time = time.perf_counter()
            def _read_stderr(proc, cb):
                try:
                    while proc and proc.stderr and not self.stop_event.is_set():
                        line = proc.stderr.readline()
                        if not line: break
                        txt = line.decode('utf-8', errors='ignore').strip()
                        if txt: cb(f"ffmpeg: {txt}")
                except Exception: pass
            stderr_thread = threading.Thread(target=_read_stderr, args=(self.proc, self.log), daemon=True)
            stderr_thread.start()
            try:
                while not self.stop_event.is_set():
                    with self.seek_lock:
                        if self.seek_request >= 0 and self.is_vod:
                            seek_pos = self.seek_request
                            self.seek_request = -1.0
                            self.log(f"{seek_pos:.2f}秒へシークします...")
                            if self.proc: 
                                self.proc.kill()
                                self.proc.wait()
                            self.proc = self._start_ffmpeg(start_time_sec=seek_pos)
                            if not self.proc or not self.proc.stdout:
                                self.log("シーク後のffmpeg再起動に失敗しました。")
                                break
                            start_time = time.perf_counter() - seek_pos
                            stderr_thread = threading.Thread(target=_read_stderr, args=(self.proc, self.log), daemon=True)
                            stderr_thread.start()
                            continue # ループの先頭に戻る
                    data = self.proc.stdout.read(frame_size)
                    if not data or len(data) < frame_size:
                        self.log("ストリームが終了または中断しました。")
                        if not self.is_live and not self.loop_vod:
                            if self._stop_cb: self._stop_cb()
                            break
                        else:
                            time.sleep(0.5)
                            continue
                    now = time.perf_counter()
                    self.playback_time = now - start_time
                    frame = np.frombuffer(data, dtype=np.uint8).reshape((self.height, self.width, 3))
                    with self.frame_lock:
                        self.latest_frame_bgr = frame
                    self.spout.sendImage(frame.tobytes(), self.width, self.height, SpoutGL.enums.GL_BGR_EXT, False, 3)
                    elapsed = time.perf_counter() - last_frame_time
                    sleep_time = frame_interval - elapsed
                    if sleep_time > 0:
                        time.sleep(sleep_time)
                    last_frame_time = time.perf_counter()
            except KeyboardInterrupt:
                self.log("終了します。")
            finally:
                self.cleanup()

    def seek(self, time_seconds: float):
        if self.is_vod:
            with self.seek_lock:
                self.seek_request = time_seconds

    def cleanup(self):
        """リソースのクリーンアップ"""
        self.stop_event.set()
        try:
            if self.proc:
                self.proc.kill()
                self.proc.wait(timeout=1)
        except Exception as e:
            self.log(f"ffmpegプロセスの終了に失敗: {e}")
        if self.container:
            try:
                self.container.close()
            except Exception as e:
                self.log(f"PyAVの解放に失敗: {e}")
        self.container = None
        self.video_stream = None
        self.av_frame_gen = None
        if self.spout:
            try:
                self.spout.releaseSender()
            except Exception as e:
                self.log(f"Spoutの解放に失敗: {e}")