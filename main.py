#!/usr/bin/env python3
"""
ytdlpSpout CLI - YouTube to Spout Streamer Command Line Tool

YouTube動画をSpout経由でリアルタイム配信するCLIツール
"""

import argparse
import cv2
import yt_dlp
import SpoutGL
import subprocess
import sys
import os
import numpy as np
import time
import threading
import ssl
import certifi
from typing import Optional, Tuple

# SSL証明書の設定（Windows環境での証明書問題を回避）
try:
    import ssl
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


def detect_fps(info: dict) -> Optional[int]:
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


def detect_max_resolution(info: dict) -> Optional[Tuple[int, int]]:
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

class CLIStreamer:
    """CLI版のStreamerクラス"""
    
    def __init__(self, video_url: str, sender_name: str, max_resolution: Optional[Tuple[int, int]] = None, 
                 manual_resolution: Optional[Tuple[int, int]] = None, loop_vod: bool = False, verbose: bool = False):
        self.video_url = video_url
        self.sender_name = sender_name
        self.proc: Optional[subprocess.Popen] = None
        self.stop_event = threading.Event()
        self.width = DEFAULT_WIDTH
        self.height = DEFAULT_HEIGHT
        self.detected_fps = DEFAULT_FPS
        self.is_live = False
        self.http_headers = {}
        self.stream_url = None
        self.spout: Optional[SpoutGL.SpoutSender] = None
        self.max_resolution = max_resolution
        self.manual_resolution = manual_resolution
        self.loop_vod = loop_vod
        self.verbose = verbose

    def log(self, msg: str):
        """ログ出力"""
        if self.verbose:
            print(f"[{time.strftime('%H:%M:%S')}] {msg}")
        else:
            print(msg)

    def _yt_refresh(self) -> bool:
        """yt-dlpでストリーム情報を取得・更新"""
        # 環境に応じた最適なフォーマット文字列を取得
        format_str, codec_info = get_optimal_format_string()
        if self.verbose:
            self.log(f"コーデック対応状況: {codec_info}")
        
        ydl_opts = {
            # 環境に応じて動的に決定されたフォーマット
            'format': format_str,
            'noplaylist': True,
            'quiet': not self.verbose,
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
        
        # PyInstallerでの実行時にyt-dlpの問題を回避
        if getattr(sys, 'frozen', False):
            ydl_opts['extract_flat'] = False
            ydl_opts['no_warnings'] = True

        try:
            with yt_dlp.YoutubeDL(ydl_opts) as ydl:
                info = ydl.extract_info(self.video_url, download=False)
        except Exception as e:
            self.log(f"yt-dlp 取得失敗: {e}")
            return False

        # URL取得
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
        
        # FPS検出
        fps = detect_fps(info)
        if fps:
            self.detected_fps = max(MIN_FPS, min(MAX_FPS, fps))

        # 選択されたフォーマット情報をログ出力
        if self.verbose:
            selected_format = info.get('format_id', 'unknown')
            vcodec = info.get('vcodec', 'unknown')
            resolution = f"{info.get('width', '?')}x{info.get('height', '?')}"
            self.log(f"選択されたフォーマット: {selected_format}, コーデック: {vcodec}, 解像度: {resolution}")

        # 解像度検出
        wh = detect_max_resolution(info)
        if wh:
            w, h = wh
            # 最大解像度制限を適用
            if self.max_resolution:
                maxw, maxh = self.max_resolution
                if w > maxw or h > maxh:
                    rw = maxw / w
                    rh = maxh / h
                    scale = min(rw, rh)
                    w = int(w * scale)
                    h = int(h * scale)
            
            # 手動解像度設定を適用
            if self.manual_resolution:
                mw, mh = self.manual_resolution
                if mw and mh:
                    w, h = int(mw), int(mh)
            
            self.width, self.height = w, h

        return self.stream_url is not None

    def _start_ffmpeg(self):
        """ffmpegプロセスを開始"""
        user_agent = self.http_headers.get("User-Agent") or self.http_headers.get("user-agent")
        ffmpeg_path = find_ffmpeg_path()
        
        cmd = [
            ffmpeg_path,
            "-loglevel", "warning" if not self.verbose else "info",
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
            cmd += ["-rw_timeout", "5000000"]  # 5秒に延長

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
        
        # Windows用のコンソール非表示設定
        startupinfo = None
        creationflags = 0
        if hasattr(subprocess, 'STARTUPINFO') and hasattr(subprocess, 'STARTF_USESHOWWINDOW') and hasattr(subprocess, 'CREATE_NO_WINDOW'):
            startupinfo = subprocess.STARTUPINFO()
            startupinfo.dwFlags |= subprocess.STARTF_USESHOWWINDOW
            creationflags = subprocess.CREATE_NO_WINDOW

        # デバッグ用: 実行するffmpegコマンドをログ出力
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

    def run(self):
        """メインの実行ループ"""
        # 初期化
        if not self._yt_refresh():
            self.log("初期のストリームURL取得に失敗しました。")
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

        # Spout初期化
        self.spout = SpoutGL.SpoutSender()
        self.spout.createOpenGL()
        self.spout.setSenderName(self.sender_name)
        
        self.log(f"{self.sender_name} で送信を開始しました。({self.width}x{self.height}) @ {self.detected_fps}fps")
        if self.is_live:
            self.log("ライブストリームを検出しました。")
        else:
            self.log(f"VOD を検出しました。{'ループ再生' if self.loop_vod else '1回再生'}します。")

        frame_size = self.width * self.height * 3
        consecutive_failures = 0
        last_frame_time = time.perf_counter()
        frame_interval = 1.0 / self.detected_fps

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
                data = self.proc.stdout.read(frame_size)
                
                # EOF or プロセス終了を検知
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
                        break
                    else:
                        # ライブストリーム再接続
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

                # フレームレート制御
                now = time.perf_counter()
                elapsed = now - last_frame_time
                if elapsed < frame_interval:
                    time.sleep(frame_interval - elapsed)
                last_frame_time = time.perf_counter()

                # フレーム処理とSpout送信
                frame = np.frombuffer(data, dtype=np.uint8)
                frame = frame.reshape((self.height, self.width, 3))

                gl_format = SpoutGL.enums.GL_BGR_EXT
                bpp = SpoutGL.helpers.getBytesPerPixel(gl_format)
                self.spout.sendImage(frame.tobytes(), self.width, self.height, gl_format, False, bpp)

        except KeyboardInterrupt:
            self.log("終了します。")
        finally:
            self.cleanup()

        return True

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

    def stop(self):
        """停止シグナルを送信"""
        self.stop_event.set()


def parse_args():
    """コマンドライン引数を解析"""
    parser = argparse.ArgumentParser(
        description="YouTube動画をSpout経由でリアルタイム配信するCLIツール",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
使用例:
  %(prog)s "https://www.youtube.com/watch?v=dQw4w9WgXcQ"
  %(prog)s -s "MySpoutSender" -w 1280 --height 720 "https://youtu.be/dQw4w9WgXcQ"
  %(prog)s --max-width 1920 --max-height 1080 --loop "https://youtu.be/dQw4w9WgXcQ"
  %(prog)s --no-limit --verbose "https://youtu.be/dQw4w9WgXcQ"  # 4K以上も許可
  %(prog)s --check-codecs  # コーデック対応確認
        """
    )
    
    parser.add_argument("url", nargs="?", default=DEFAULT_VIDEO_URL,
                       help=f"YouTube URL (デフォルト: {DEFAULT_VIDEO_URL})")
    
    parser.add_argument("-s", "--sender", default=DEFAULT_SENDER_NAME,
                       help=f"Spout送信者名 (デフォルト: {DEFAULT_SENDER_NAME})")
    
    # 解像度設定
    res_group = parser.add_argument_group("解像度設定")
    res_group.add_argument("--max-width", type=int, metavar="W",
                          help="最大幅制限 (自動検出解像度に上限を設定)")
    res_group.add_argument("--max-height", type=int, metavar="H", 
                          help="最大高さ制限 (自動検出解像度に上限を設定)")
    res_group.add_argument("-w", "--width", type=int, metavar="W",
                          help="手動幅設定 (自動検出を上書き)")
    res_group.add_argument("--height", type=int, metavar="H",
                          help="手動高さ設定 (自動検出を上書き)")
    
    # その他のオプション
    parser.add_argument("--loop", action="store_true",
                       help="VOD（録画）をループ再生する")
    parser.add_argument("-v", "--verbose", action="store_true",
                       help="詳細ログを表示")
    parser.add_argument("--check-codecs", action="store_true",
                       help="対応コーデックを確認して終了")
    parser.add_argument("--no-limit", action="store_true",
                       help="解像度制限を無効にする（4K以上も許可）")
    
    return parser.parse_args()


def main():
    """メイン関数"""
    args = parse_args()
    
    # コーデック確認モード
    if args.check_codecs:
        print("=== コーデック対応状況確認 ===")
        av1_supported, av1_decoders = check_av1_support()
        format_str, codec_info = get_optimal_format_string()
        
        print(f"ffmpegパス: {find_ffmpeg_path()}")
        print(f"AV1対応: {'✓' if av1_supported else '✗'}")
        if av1_supported:
            print(f"AV1デコーダー: {', '.join(av1_decoders)}")
        print(f"使用フォーマット: {format_str}")
        print(f"設定: {codec_info}")
        return 0
    
    # 解像度設定の処理
    max_resolution = None
    if args.max_width and args.max_height:
        max_resolution = (args.max_width, args.max_height)
    elif not args.no_limit:
        # デフォルトで1080p制限を設定（安定性重視）
        max_resolution = (1920, 1080)
        if args.verbose:
            print(f"[{time.strftime('%H:%M:%S')}] デフォルト解像度制限: 1080p (--no-limitで無効化可能)")
    
    manual_resolution = None
    if args.width and args.height:
        manual_resolution = (args.width, args.height)
    
    # Streamerを作成・実行
    streamer = CLIStreamer(
        video_url=args.url,
        sender_name=args.sender,
        max_resolution=max_resolution,
        manual_resolution=manual_resolution,
        loop_vod=args.loop,
        verbose=args.verbose
    )
    
    try:
        success = streamer.run()
        return 0 if success else 1
    except Exception as e:
        print(f"エラー: {e}")
        return 1


if __name__ == "__main__":
    sys.exit(main())
