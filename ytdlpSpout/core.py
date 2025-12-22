"""ytdlpSpout Streamer コアモジュール"""

import os
import ssl
import subprocess
import sys
import threading
import time
from typing import Callable, Optional

import av
import cv2
import numpy as np
import SpoutGL
import yt_dlp

# 共通モジュールからのインポート
from .constants import (
    DEFAULT_FPS,
    DEFAULT_HEIGHT,
    DEFAULT_SENDER_NAME,
    DEFAULT_VIDEO_URL,
    DEFAULT_WIDTH,
    MAX_FPS,
    MIN_FPS,
)
from .ffmpeg import (
    build_ffmpeg_header_args,
    check_av1_support,
    find_ffmpeg_path,
    get_executable_dir,
    get_optimal_format_string,
)
from .video_info import detect_fps, detect_max_resolution

# SSL証明書の設定（Windows環境での証明書問題を回避）
try:
    ssl._create_default_https_context = ssl._create_unverified_context
except Exception:
    pass

# 後方互換性のためのエイリアス（既存コードのインポート互換）
__all__ = [
    "Streamer",
    "DEFAULT_VIDEO_URL",
    "DEFAULT_SENDER_NAME",
    "DEFAULT_WIDTH",
    "DEFAULT_HEIGHT", 
    "DEFAULT_FPS",
    "MIN_FPS",
    "MAX_FPS",
    "get_executable_dir",
    "find_ffmpeg_path",
    "check_av1_support",
    "get_optimal_format_string",
    "build_ffmpeg_header_args",
    "detect_fps",
    "detect_max_resolution",
]


class Streamer:
    def __init__(self, video_url, sender_name, 
                 max_resolution=None, 
                 manual_resolution=None, 
                 loop_vod=False, verbose=True, 
                 log_cb=None, 
                 stop_cb=None,
                 init_ok_cb=None,
                 external_spout_sender=None):
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
        
        # フレーム送信監視用
        self.frame_count = 0
        self.frames_sent = 0
        self.first_frame_sent_time = None
        self.is_sending_frames = False
        self.spout = external_spout_sender
        self.owns_spout = (external_spout_sender is None) # 外部から渡された場合は所有しない
        
        
        # フレーム同期用（PTS: Presentation Timestamp）
        self.current_frame_pts: float = 0.0  # 現在フレームのPTS（秒）
        self.pts_lock = threading.Lock()  # PTSアクセス用ロック
        
        # フレーム送信制御（dry-run用）
        self.spout_enabled: bool = True  # FalseならSpout送信をスキップ
        self.spout_enabled_lock = threading.Lock()

        # 一時停止制御（待ち伏せ同期用）
        self.pause_request_pts: Optional[float] = None
        self.is_paused = False
        self.pause_lock = threading.Lock()

    def set_spout_enabled(self, enabled: bool):
        """Spout送信の有効/無効を切り替える"""
        with self.spout_enabled_lock:
            self.spout_enabled = enabled
        self.log(f"[SPOUT] 送信{'有効' if enabled else '無効'}化")

    def pause_at_pts(self, pts: float):
        """指定したPTSで一時停止するよう予約"""
        with self.pause_lock:
            self.pause_request_pts = pts
            self.is_paused = False  # まだ停止していない
        self.log(f"[SYNC] PTS {pts:.3f}秒での一時停止を予約")

    def resume(self):
        """一時停止を解除して再開"""
        with self.pause_lock:
            was_paused = self.is_paused
            self.is_paused = False
            self.pause_request_pts = None
        if was_paused:
            self.log("[SYNC] 再生再開")

    def log(self, msg: str):
        try:
            if self._log_cb:
                self._log_cb(msg)
            else:
                print(msg)
        except Exception:
            pass

    def wait_for_pts(self, target_pts: float, timeout: float = 15.0) -> bool:
        """
        指定したPTSに到達するまで待機する。
        
        Args:
            target_pts: 待機対象のPTS（秒）
            timeout: タイムアウト秒数（デフォルト15秒）
        
        Returns:
            True: 目標PTSに到達
            False: タイムアウトまたは停止
        """
        start_time = time.perf_counter()
        frame_interval = 1.0 / max(self.detected_fps, 1)
        tolerance = frame_interval / 2  # 1フレームの半分を許容差とする
        
        self.log(f"[SYNC] PTS待機開始: 目標={target_pts:.3f}秒, 許容差={tolerance:.4f}秒")
        
        while not self.stop_event.is_set():
            elapsed = time.perf_counter() - start_time
            if elapsed > timeout:
                self.log(f"[SYNC] PTS待機タイムアウト: {elapsed:.1f}秒経過")
                return False
            
            with self.pts_lock:
                current = self.current_frame_pts
            
            # 目標PTSに到達（許容差内）
            if current >= target_pts - tolerance:
                self.log(f"[SYNC] PTS到達: 現在={current:.3f}秒, 目標={target_pts:.3f}秒, 差分={abs(current - target_pts) * self.detected_fps:.2f}フレーム")
                return True
            
            # まだ到達していない場合は短い間隔で待機
            time.sleep(frame_interval / 4)
        
        self.log("[SYNC] PTS待機中断: ストリーマー停止")
        return False


    def _get_local_file_info(self) -> bool:
        """PyAVを使ってローカルファイルの情報を取得"""
        try:
            # PyAVでファイルを開く（メタデータ取得のみなので読み込みは最小限）
            with av.open(self.video_url) as container:
                video_stream = next((s for s in container.streams if s.type == 'video'), None)
                if not video_stream:
                    self.log("エラー: ファイル内に映像ストリームが見つかりません。")
                    return False

                # 解像度
                w = video_stream.width
                h = video_stream.height
                self.original_width = w
                self.original_height = h
                
                # 解像度制限・手動設定の適用
                if self.max_resolution:
                    maxw, maxh = self.max_resolution
                    if w > maxw or h > maxh:
                        scale = min(maxw / w, maxh / h)
                        w, h = int(w * scale), int(h * scale)
                
                if self.manual_resolution:
                    mw, mh = self.manual_resolution
                    if mw and mh:
                        w, h = int(mw), int(mh)
                
                self.width = w
                self.height = h

                # FPS
                # average_rate は Fraction 型で返ることが多い
                if video_stream.average_rate:
                    try:
                        fps = float(video_stream.average_rate)
                        self.detected_fps = int(round(fps))
                    except Exception:
                        self.detected_fps = 30
                else:
                    self.detected_fps = 30
                
                self.detected_fps = max(MIN_FPS, min(MAX_FPS, self.detected_fps))

                # 長さ (container.duration は通常マイクロ秒単位)
                if container.duration:
                    self.duration = float(container.duration) / 1000000.0
                else:
                    self.duration = 0.0

                self.is_vod = True
                self.is_live = False
                self.stream_url = self.video_url 

                self.log(f"ファイル情報(PyAV): {self.width}x{self.height} @ {self.detected_fps}fps, 長さ: {self.duration:.2f}s")
                return True

        except Exception as e:
            self.log(f"PyAVでのファイル情報取得に失敗: {e}")
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
        
        # Cookieをヘッダーに追加
        if hasattr(ydl, 'cookiejar'):
            cookies = []
            for cookie in ydl.cookiejar:
                cookies.append(f"{cookie.name}={cookie.value}")
            if cookies:
                cookie_header = "; ".join(cookies)
                # 既存のCookieがあれば維持しつつ追加（上書きせず）
                if 'Cookie' in self.http_headers:
                     self.log(f"既存のCookieヘッダーが見つかりました、今回取得したCookieで更新します。")
                     self.http_headers['Cookie'] = cookie_header # ここではシンプルに置き換えを選択（通常yt-dlpが正）
                else:
                    self.http_headers['Cookie'] = cookie_header
                # self.log(f"Cookieをヘッダーに注入しました: {len(cookies)}個")

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
            # self.log(f"ffmpegコマンド: {cmd_str}")  # ログが長すぎるため抑制
            
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
            if self.owns_spout:
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
                                if img.shape[1] != self.width or img.shape[0] != self.height:
                                    img = cv2.resize(img, (self.width, self.height), interpolation=cv2.INTER_LINEAR)
                                with self.frame_lock:
                                    self.latest_frame_bgr = img
                                
                                # フレーム送信状態を更新
                                self.frames_sent += 1
                                if not self.is_sending_frames:
                                    self.is_sending_frames = True
                                    self.first_frame_sent_time = time.perf_counter()
                                
                                # PTS更新（フレーム同期用）
                                with self.pts_lock:
                                    self.current_frame_pts = self.playback_time
                                
                                # 一時停止PTSチェック
                                check_pause = False
                                with self.pause_lock:
                                    if self.pause_request_pts is not None and self.current_frame_pts >= self.pause_request_pts:
                                        check_pause = True
                                        self.is_paused = True
                                
                                if check_pause:
                                    self.log(f"[SYNC] 目標PTS({self.current_frame_pts:.3f}s)到達により一時停止待機...")
                                    while not self.stop_event.is_set():
                                        with self.pause_lock:
                                            if not self.is_paused:
                                                break
                                        time.sleep(0.01)

                                # Spout送信（有効時のみ）
                                with self.spout_enabled_lock:
                                    if self.spout_enabled:
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
                                if not self.stop_event.is_set() and self._stop_cb: 
                                    self._stop_cb()
                                break
                        else:
                            if not self.stop_event.is_set() and self._stop_cb: 
                                self._stop_cb()
                            break
                    img = frame.to_ndarray(format='bgr24')
                    if img.shape[1] != self.width or img.shape[0] != self.height:
                        img = cv2.resize(img, (self.width, self.height), interpolation=cv2.INTER_LINEAR)
                    now = time.perf_counter()
                    # 再生位置はフレームのタイムスタンプを優先
                    self.playback_time = frame.time if hasattr(frame, 'time') and frame.time is not None else self.playback_time + frame_interval
                    with self.frame_lock:
                        self.latest_frame_bgr = img
                    
                    # フレーム送信状態を更新
                    self.frames_sent += 1
                    if not self.is_sending_frames:
                        self.is_sending_frames = True
                        self.first_frame_sent_time = time.perf_counter()
                    
                    # PTS更新（フレーム同期用）
                    with self.pts_lock:
                        self.current_frame_pts = self.playback_time
                    
                    
                    # 一時停止PTSチェック
                    check_pause = False
                    with self.pause_lock:
                        if self.pause_request_pts is not None and self.current_frame_pts >= self.pause_request_pts:
                            check_pause = True
                            self.is_paused = True
                    
                    if check_pause:
                        self.log(f"[SYNC] 目標PTS({self.current_frame_pts:.3f}s)到達により一時停止待機...")
                        while not self.stop_event.is_set():
                            with self.pause_lock:
                                if not self.is_paused:
                                    break
                            time.sleep(0.01)
                    
                    # Spout送信（有効時のみ）
                    with self.spout_enabled_lock:
                        if self.spout_enabled:
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
                return False
            # Spout init
            if self.owns_spout:
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
                        if self.stop_event.is_set():
                            break # ユーザー停止時はコールバックしない
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
                    
                    # フレーム送信状態を更新
                    self.frames_sent += 1
                    if not self.is_sending_frames:
                        self.is_sending_frames = True
                        self.first_frame_sent_time = time.perf_counter()
                    
                    # PTS更新（フレーム同期用）
                    with self.pts_lock:
                        self.current_frame_pts = self.playback_time
                    
                    # Spout送信（有効時のみ）
                    with self.spout_enabled_lock:
                        if self.spout_enabled:
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
            if self.owns_spout:
                try:
                    self.spout.releaseSender()
                except Exception as e:
                    self.log(f"Spoutの解放に失敗: {e}")
            self.spout = None  # 参照を確実に切る

    def find_best_match_pts(self, target_img_bgr, center_pts: float, search_range: float = 3.0) -> tuple[float, float, object]:
        """
        指定された画像に最も近いフレームを周辺から探索し、そのPTSを返す。
        
        Args:
            target_img_bgr: ターゲット画像 (BGR numpy array)
            center_pts: 探索の中心となるPTS (秒)
            search_range: 前後の探索範囲 (秒)
            
        Returns:
            (best_pts, min_score, best_frame_img)
            - best_pts: 最も類似度が高いフレームのPTS (秒)
            - min_score: 類似度スコア (小さいほど似ている, 0-255)
            - best_frame_img: マッチしたフレームの画像 (確認用)
        """
        if not self.is_vod or not self.is_local_file or target_img_bgr is None:
            return center_pts, float('inf'), None
            
        try:
            import av
            # 比較用にターゲット画像を縮小・グレースケール化
            # 64x36程度あれば十分特徴を捉えられる
            h, w = target_img_bgr.shape[:2]
            chk_w, chk_h = 64, 36
            target_small = cv2.resize(target_img_bgr, (chk_w, chk_h))
            target_gray = cv2.cvtColor(target_small, cv2.COLOR_BGR2GRAY)
            
            best_score = float('inf')
            best_pts = center_pts
            best_img = None
            
            # 一時的にコンテナを開く (メイン再生用とは別)
            with av.open(self.video_url) as container:
                video_stream = next((s for s in container.streams if s.type == 'video'), None)
                if not video_stream:
                    return center_pts, float('inf'), None
                
                # 探索開始位置 (少し手前から)
                start_time = max(0, center_pts - (search_range / 2))
                end_time = center_pts + (search_range / 2)
                
                # シーク
                pts = int(start_time / float(video_stream.time_base))
                container.seek(pts, any_frame=False, backward=True, stream=video_stream)
                
                for frame in container.decode(video_stream):
                    # 時間チェック
                    if frame.time is None: continue
                    if frame.time > end_time: break # 範囲外に出たら終了
                    if frame.time < start_time: continue # シーク位置が手前すぎる場合はスキップ
                    
                    # 画像比較
                    img = frame.to_ndarray(format='bgr24')
                    small = cv2.resize(img, (chk_w, chk_h))
                    gray = cv2.cvtColor(small, cv2.COLOR_BGR2GRAY)
                    
                    # 差分計算 (Mean Absolute Difference)
                    diff = cv2.absdiff(target_gray, gray)
                    score = np.mean(diff)
                    
                    if score < best_score:
                        best_score = score
                        best_pts = frame.time
                        best_img = img
            
            return best_pts, best_score, best_img
            
        except Exception as e:
            self.log(f"画像マッチング探索エラー: {e}")
            return center_pts, float('inf'), None