"""ytdlpSpout GUI アプリケーション"""
from __future__ import annotations

import os
import queue
import ssl
import subprocess
import sys
import threading
import time
from pathlib import Path
from typing import TYPE_CHECKING, Any

import cv2
import customtkinter as ctk
import tkinter as tk
import yt_dlp
# SpoutGLは使用しない（C++ DLLがSpout送信を担当）
# import SpoutGL
from PIL import Image

from ytdlpSpout.core import (
    DEFAULT_SENDER_NAME,
    DEFAULT_VIDEO_URL,
    Streamer,
    get_optimal_format_string,
)

# 分離したGUIモジュールからのインポート
from ytdlpSpout_gui.constants import (
    UIConfig,
    PREFERRED_JAPANESE_FONTS,
    PREFERRED_MONOSPACE_FONTS,
)
from ytdlpSpout_gui.fonts import get_best_japanese_font, get_best_monospace_font
from ytdlpSpout_gui.logger import YtdlpLogger
from ytdlpSpout_gui.utils import clean_playlist_url

# =============================================================================
# C++ DLLバックエンド切り替え設定
# =============================================================================
# True: C++ DLLを使用（高パフォーマンス、要ビルド済みDLL）
# False: Python Streamerを使用（従来の動作）
USE_NATIVE_BACKEND = True

# NativeStreamerWrapper のインポート
NATIVE_BACKEND_AVAILABLE = False
if USE_NATIVE_BACKEND:
    try:
        from python.native_streamer_wrapper import NativeStreamerWrapper
        NATIVE_BACKEND_AVAILABLE = True
        print("[INFO] C++ DLLバックエンドが利用可能です")
    except ImportError as e:
        print(f"[WARNING] NativeStreamerWrapper import failed: {e}")
        print("[WARNING] Falling back to Python Streamer")
        USE_NATIVE_BACKEND = False

# YtDlpAsyncResolver のインポート（非同期URL解決用）
YTDLP_RESOLVER_AVAILABLE = False
try:
    from python.ytdlp_resolver import YtDlpAsyncResolver, ResolvedInfo
    YTDLP_RESOLVER_AVAILABLE = True
except ImportError as e:
    print(f"[WARNING] YtDlpAsyncResolver import failed: {e}")
# =============================================================================

if TYPE_CHECKING:
    import numpy as np


# SSL証明書の設定（Windows環境での証明書問題を回避）
try:
    ssl._create_default_https_context = ssl._create_unverified_context
except AttributeError:
    pass  # 古いPythonバージョンでは無視


class App:
    """ytdlpSpout GUIアプリケーションメインクラス"""
    
    def __init__(self, root: ctk.CTk) -> None:
        self.root = root
        self.root.title(UIConfig.WINDOW_TITLE)
        self.streamer: Streamer | None = None
        self._seeking = False
        
        # メインスレッド監視用変数
        self.main_thread_monitor_active = False
        self.main_thread_last_heartbeat: float | None = None
        self.main_thread_monitor_thread: threading.Thread | None = None
        self._last_heartbeat_time: float = 0.0
        
        # 設定フラグ（サブプロセスダウンロードは常時有効）
        self.use_subprocess_download = True  # 常時有効
        self.seek_value = 0.0
        self.duration_cache = 0.0
        self.local_video_path: str | None = None
        self.download_in_progress = False
        self.original_url: str | None = None
        
        # ログキュー（ログ処理の非同期化）
        self.log_queue: queue.Queue[str] = queue.Queue()
        self.log_processing = False
        
        # デバッグ用
        self.preview_update_disabled = False
        
        # 共有SpoutSender
        self.spout_sender = None
        
        # プログレス管理用
        self._subprocess_progress_active = False
        self._download_process: subprocess.Popen | None = None  # ダウンロードプロセスを保持
        self._download_cancelled = False  # ダウンロードキャンセルフラグ
        self.current_seekbar_knob_color = UIConfig.SEEKBAR_KNOB_STANDBY  # 現在のシークバーノブ色
        
        # yt-dlp非同期リゾルバー（URL解決中のインスタンス保持）
        self._ytdlp_resolver: YtDlpAsyncResolver | None = None
        self._url_resolving = False  # URL解決中フラグ
        # URL解決の世代カウンタ（Stop後にキャンセルされた解決結果を無視するため）
        self._url_resolve_generation = 0

        # ストリーム開始の世代カウンタ（Start処理中にStopが押された場合、
        # 後からバックグラウンドスレッドで構築が完了したStreamerを
        # 自動的に破棄し、孤立ストリーム化を防ぐため）
        self._stream_start_generation = 0
        self._stream_lock = threading.Lock()

        # プログレスデータ初期化
        self.download_progress: dict[str, Any] = self._create_empty_progress()

        # プレビュー処理用の常駐ワーカースレッド起動（最新フレーム1件のみ保持するキュー経由）
        self._preview_frame_queue: queue.Queue = queue.Queue(maxsize=1)
        self._preview_worker_thread: threading.Thread | None = None
        self._start_preview_worker()

        # UI初期化
        self._setup_appearance()
        self._setup_fonts()
        self._setup_window()
        self._setup_widgets()
        self._setup_event_handlers()
    
    def _create_empty_progress(self) -> dict[str, Any]:
        """空の進捗データを作成"""
        return {
            'percent': 0.0,
            'downloaded_bytes': 0,
            'total_bytes': 0,
            'speed': '',
            'eta': '',
            'filename': ''
        }
    
    def update_seekbar_color(self, color: str) -> None:
        """シークバーのノブ色を更新して状態を表示"""
        try:
            self.current_seekbar_knob_color = color
            self.seek_slider.configure(button_color=color, button_hover_color=color)
        except Exception:
            pass
    
    def _setup_appearance(self) -> None:
        """CustomTkinterの外観設定"""
        ctk.set_appearance_mode("dark")
        ctk.set_default_color_theme("blue")
    
    def _setup_fonts(self) -> None:
        """フォント設定"""
        self.japanese_font = get_best_japanese_font()
        self.monospace_font = get_best_monospace_font()
        self.log_font = self.japanese_font
    
    def _setup_window(self) -> None:
        """ウィンドウ設定"""
        self.root.geometry(UIConfig.INITIAL_SIZE)
        self.root.minsize(UIConfig.MIN_WIDTH, UIConfig.MIN_HEIGHT)
    
    def _setup_widgets(self) -> None:
        """ウィジェットのセットアップ"""
        self._setup_control_frame()
        self._setup_progress_frame()
        self._setup_info_labels()
        self._setup_main_content()
    
    def _setup_event_handlers(self) -> None:
        """イベントハンドラの設定"""
        self.root.protocol("WM_DELETE_WINDOW", self.on_close)
        self.root.after(UIConfig.PREVIEW_UPDATE_INTERVAL, self.update_preview)
    
    def _setup_control_frame(self) -> None:
        """コントロールフレームのセットアップ"""
        frm = ctk.CTkFrame(self.root)
        frm.pack(fill="x", padx=8, pady=4)  # pady reduced
        
        # Grid configuration
        frm.grid_columnconfigure(1, weight=1)
        # Column weights for balanced layout
        # 0: Label, 1: Entry/Data, 2: Label/Btn, 3: Entry/Btn, 4: Checkbox, 5: Checkbox

        # Row 0: URL (Full width)
        ctk.CTkLabel(frm, text="URL", font=ctk.CTkFont(family=self.japanese_font, size=12)).grid(
            row=0, column=0, sticky="w", padx=6, pady=2)
        self.url_var = ctk.StringVar(value=DEFAULT_VIDEO_URL)
        ctk.CTkEntry(frm, textvariable=self.url_var, width=400, font=ctk.CTkFont(family=self.japanese_font, size=11)).grid(
            row=0, column=1, columnspan=5, sticky="ew", padx=6, pady=2)

        # Row 1: Sender | Start Button | Stop Button
        ctk.CTkLabel(frm, text="Sender", font=ctk.CTkFont(family=self.japanese_font, size=12)).grid(
            row=1, column=0, sticky="w", padx=6, pady=2)
        self.sender_var = ctk.StringVar(value=DEFAULT_SENDER_NAME)
        ctk.CTkEntry(frm, textvariable=self.sender_var, width=200, font=ctk.CTkFont(family=self.japanese_font, size=11)).grid(
            row=1, column=1, sticky="ew", padx=6, pady=2)

        # Buttons moved to Row 1
        self.btn_start = ctk.CTkButton(
            frm, text="Start", command=self.on_stream, width=100,
            font=ctk.CTkFont(family=self.japanese_font, size=UIConfig.FONT_SIZE_NORMAL, weight="bold")
        )
        self.btn_stop = ctk.CTkButton(
            frm, text="Stop", command=self.on_stop, state="disabled", width=100,
            font=ctk.CTkFont(family=self.japanese_font, size=UIConfig.FONT_SIZE_NORMAL, weight="bold")
        )
        self.btn_start.grid(row=1, column=2, columnspan=2, padx=6, pady=2, sticky="ew")
        self.btn_stop.grid(row=1, column=4, columnspan=2, padx=6, pady=2, sticky="ew")

        # Row 2: Max W | Max H | Use Max Cap | 1440p Limit
        ctk.CTkLabel(frm, text="Max W", font=ctk.CTkFont(family=self.japanese_font, size=12)).grid(
            row=2, column=0, sticky="w", padx=6, pady=2)
        self.maxw_var = ctk.StringVar(value="1920")
        ctk.CTkEntry(frm, textvariable=self.maxw_var, width=70, font=ctk.CTkFont(family=self.monospace_font, size=11)).grid(
            row=2, column=1, sticky="w", padx=6, pady=2)
        
        ctk.CTkLabel(frm, text="Max H", font=ctk.CTkFont(family=self.japanese_font, size=12)).grid(
            row=2, column=2, sticky="w", padx=6, pady=2)
        self.maxh_var = ctk.StringVar(value="1080")
        ctk.CTkEntry(frm, textvariable=self.maxh_var, width=70, font=ctk.CTkFont(family=self.monospace_font, size=11)).grid(
            row=2, column=3, sticky="w", padx=6, pady=2)
        
        self.max_enable = ctk.BooleanVar(value=False)
        ctk.CTkCheckBox(frm, text="Cap", variable=self.max_enable, width=60, font=ctk.CTkFont(family=self.japanese_font, size=12)).grid(
            row=2, column=4, sticky="w", padx=6, pady=2)

        self.perf_limit = ctk.BooleanVar(value=True)  # デフォルトで有効
        perf_cb = ctk.CTkCheckBox(frm, text="1440p Limit", variable=self.perf_limit, width=100,
                                 font=ctk.CTkFont(family=self.japanese_font, size=12))
        perf_cb.grid(row=2, column=5, sticky="w", padx=6, pady=2)

        # Row 3: Manual W | Manual H | Use Manual | Loop VOD
        ctk.CTkLabel(frm, text="Manual W", font=ctk.CTkFont(family=self.japanese_font, size=12)).grid(
            row=3, column=0, sticky="w", padx=6, pady=2)
        self.manw_var = ctk.StringVar(value="")
        ctk.CTkEntry(frm, textvariable=self.manw_var, width=70, font=ctk.CTkFont(family=self.monospace_font, size=11)).grid(
            row=3, column=1, sticky="w", padx=6, pady=2)
        
        ctk.CTkLabel(frm, text="Manual H", font=ctk.CTkFont(family=self.japanese_font, size=12)).grid(
            row=3, column=2, sticky="w", padx=6, pady=2)
        self.manh_var = ctk.StringVar(value="")
        ctk.CTkEntry(frm, textvariable=self.manh_var, width=70, font=ctk.CTkFont(family=self.monospace_font, size=11)).grid(
            row=3, column=3, sticky="w", padx=6, pady=2)
        
        self.manual_enable = ctk.BooleanVar(value=False)
        ctk.CTkCheckBox(frm, text="Manual", variable=self.manual_enable, width=60, font=ctk.CTkFont(family=self.japanese_font, size=12)).grid(
            row=3, column=4, sticky="w", padx=6, pady=2)

        self.vod_loop = ctk.BooleanVar(value=False)
        ctk.CTkCheckBox(frm, text="Loop", variable=self.vod_loop, width=60,
                       font=ctk.CTkFont(family=self.japanese_font, size=12)).grid(
            row=3, column=5, sticky="w", padx=6, pady=2)

        # サブプロセスダウンロードは常時有効
        self.use_subprocess_download = True
    
    def _setup_progress_frame(self) -> None:
        """ダウンロード進捗表示エリアのセットアップ（互換性のため残すが何もしない）"""
        pass
    
    def _setup_progress_frame_in_preview(self, parent: ctk.CTkFrame) -> None:
        """ダウンロード進捗表示エリアのセットアップ（プレビュー最下部）"""
        self.progress_frame = ctk.CTkFrame(
            parent,
            fg_color=UIConfig.PROGRESS_FRAME_BG,
            border_width=2,
            border_color=UIConfig.PROGRESS_FRAME_BORDER,
            height=0  # 初期状態では高さ0
        )
        # 最下部に常に配置（高さ0で非表示）
        self.progress_frame.pack(fill="x", side="bottom", padx=6, pady=0)
        self.progress_frame.pack_propagate(False)  # 高さ0を強制
        
        # 進捗エリアのタイトル
        ctk.CTkLabel(
            self.progress_frame,
            text="🔄 ダウンロード進捗:",
            font=ctk.CTkFont(family=self.japanese_font, size=UIConfig.FONT_SIZE_SMALL, weight="bold"),
            text_color=UIConfig.PROGRESS_BAR_COLOR
        ).pack(anchor="w", padx=6, pady=(3, 0))
        
        # プログレスバー
        self.progress_bar = ctk.CTkProgressBar(
            self.progress_frame,
            progress_color=UIConfig.PROGRESS_BAR_COLOR,
            height=20,
            mode="determinate"  # 初期は確定モード
        )
        self.progress_bar.pack(fill="x", padx=6, pady=3)
        self.progress_bar.set(0)
        
        # 進捗詳細情報
        self.progress_info = ctk.CTkLabel(
            self.progress_frame,
            text="",
            font=ctk.CTkFont(family=self.monospace_font, size=UIConfig.FONT_SIZE_SMALL),
            text_color="#ffffff"
        )
        self.progress_info.pack(padx=6, pady=(0, 3))
    
    def _setup_info_labels(self) -> None:
        """情報ラベルのセットアップ"""
        self.info_label = ctk.CTkLabel(
            self.root,
            text="",
            font=ctk.CTkFont(family=self.japanese_font, size=UIConfig.FONT_SIZE_LARGE)
        )
        self.info_label.pack(padx=8, pady=4)

        # コーデック対応状況を表示
        _, codec_info = get_optimal_format_string()
        self.codec_label = ctk.CTkLabel(
            self.root,
            text=f"コーデック対応: {codec_info}",
            font=ctk.CTkFont(family=self.japanese_font, size=UIConfig.FONT_SIZE_NORMAL),
            text_color="cyan"
        )
        self.codec_label.pack(padx=8, pady=2)
    
    def _setup_main_content(self) -> None:
        """メインコンテンツエリア（プレビューとログ）のセットアップ"""
        # メインコンテンツエリア（プレビューとログを分割可能なPanedWindow）
        main_paned = tk.PanedWindow(
            self.root,
            orient=tk.VERTICAL,
            sashwidth=5,
            sashrelief=tk.RAISED,
            bg=UIConfig.PANED_BG
        )
        main_paned.pack(fill="both", expand=True, padx=8, pady=8)
        
        self._setup_preview_area(main_paned)
        self._setup_log_area(main_paned)
        
        # 初期の分割比率を設定（下からログエリアの高さを確保）
        # 初期の分割比率を設定（下からログエリアの高さを確保）
        def on_paned_configure(event):
            # イベントがPanedWindow自身のものか確認
            if event.widget != main_paned:
                return
                
            height = event.height
            if height < 100:  # まだ小さすぎる場合は無視
                return
                
            # すでにサッシが設定されているかチェックするために、イベントハンドラを解除
            main_paned.unbind("<Configure>")
            
            # ログエリアの高さを確保（150px）
            # PanedWindowの高さ - 150px の位置にサッシを設定
            desired_log_height = 150
            sash_pos = max(UIConfig.PREVIEW_MIN_HEIGHT + UIConfig.SEEKBAR_MIN_HEIGHT, height - desired_log_height)
            
            try:
                main_paned.sash_place(0, 0, sash_pos)
            except Exception:
                pass

        # Configureイベントにバインドして、レイアウト確定後にサッシを設定
        main_paned.bind("<Configure>", on_paned_configure)
    
    def _setup_preview_area(self, parent: tk.PanedWindow) -> None:
        """プレビューエリアのセットアップ"""
        # プレビューコンテナ（シークバーとプレビューを含む）
        preview_container = ctk.CTkFrame(parent, fg_color="transparent")
        parent.add(preview_container, minsize=UIConfig.PREVIEW_MIN_HEIGHT + UIConfig.SEEKBAR_MIN_HEIGHT)
        
        # ★重要: pack()は下から上へ配置する（side="bottom"の要素を先に配置）
        
        # 進捗バーエリア（プレビュー最下部）- 最初に配置
        self._setup_progress_frame_in_preview(preview_container)
        
        # シークバーエリア（最小高さを保証）- 進捗バーの上に配置
        self.seek_container = ctk.CTkFrame(preview_container, fg_color="transparent", height=UIConfig.SEEKBAR_MIN_HEIGHT)
        self.seek_container.pack(fill="x", side="bottom", before=self.progress_frame)
        self.seek_container.pack_propagate(False)  # 最小高さを強制
        
        seek_frame = ctk.CTkFrame(self.seek_container, fg_color="transparent")
        seek_frame.pack(fill="both", expand=True, padx=10, pady=5)

        self.seek_slider = ctk.CTkSlider(
            seek_frame, from_=0, to=100, state="disabled", command=self.on_seek_drag,
            button_color=self.current_seekbar_knob_color,
            button_hover_color=self.current_seekbar_knob_color
        )
        self.seek_slider.pack(fill="x", expand=True, side="left", padx=(0, 10))
        self.seek_slider.bind("<ButtonPress-1>", self.on_seek_press)
        self.seek_slider.bind("<ButtonRelease-1>", self.on_seek_release)

        self.time_label = ctk.CTkLabel(
            seek_frame,
            text="--:-- / --:--",
            font=ctk.CTkFont(family=self.monospace_font, size=UIConfig.FONT_SIZE_NORMAL)
        )
        self.time_label.pack(side="right")
        
        # プレビュー画面 - 最後に配置（残りのスペースを占有）
        self.preview_frame = ctk.CTkFrame(preview_container, fg_color="black")
        self.preview_frame.pack(fill="both", expand=True, padx=0, pady=0)
        
        self.preview_label = ctk.CTkLabel(
            self.preview_frame,
            text="No Signal",
            text_color="white",
            font=ctk.CTkFont(family=self.japanese_font, size=UIConfig.FONT_SIZE_TITLE)
        )
        self.preview_label.pack(fill="both", expand=True)
        self.preview_imgtk = None
        self._no_signal_shown = True

    
    def _setup_log_area(self, parent: tk.PanedWindow) -> None:
        """ログエリアのセットアップ"""
        log_frame = ctk.CTkFrame(parent)
        parent.add(log_frame, minsize=UIConfig.LOG_MIN_HEIGHT)
        
        # ログエリアのタイトル
        ctk.CTkLabel(
            log_frame,
            text="ログ出力:",
            font=ctk.CTkFont(family=self.japanese_font, size=UIConfig.FONT_SIZE_NORMAL, weight="bold")
        ).pack(anchor="w", padx=5, pady=(5, 0))
        
        # ログテキストエリア
        self.log_text = ctk.CTkTextbox(
            log_frame,
            height=150,
            font=ctk.CTkFont(family=self.log_font, size=UIConfig.FONT_SIZE_NORMAL)
        )
        self.log_text.pack(fill="both", expand=True, padx=5, pady=5)

    def log(self, msg: str) -> None:
        """ログメッセージを完全に非同期で処理"""
        try:
            if threading.current_thread() == threading.main_thread():
                self._log_direct(msg)
            else:
                self.log_queue.put(msg)
                if not self.log_processing:
                    self.root.after(0, self.process_log_queue)
        except Exception:
            pass
    
    def debug_log(self, msg: str) -> None:
        """デバッグ専用ログ（無効化：出力しない）"""
        pass
    
    def _start_main_thread_monitor(self) -> None:
        """メインスレッドのブロッキングを監視"""
        if self.main_thread_monitor_active:
            return
            
        self.main_thread_monitor_active = True
        
        def monitor_main_thread() -> None:
            """メインスレッドのハートビートとフレーム送信を監視"""
            last_frame_check = 0
            last_frame_count = 0
            
            while self.main_thread_monitor_active:
                current_time = time.time()
                
                # ハートビート更新をメインスレッドに依頼
                heartbeat_start = current_time
                self.root.after(0, self._main_thread_heartbeat)
                
                # Spout送信状態を定期的にログ（削除：ログが多すぎるため）
                # if current_time - last_frame_check > 2.0:  # 2秒に1回
                #     try:
                #         if self.streamer and hasattr(self.streamer, 'latest_frame_bgr'):
                #             frame_info = "フレーム有" if self.streamer.latest_frame_bgr is not None else "フレーム無"
                #             playback_time = getattr(self.streamer, 'playback_time', 0)
                #             self.root.after(0, self.log, f"[SPOUT] {frame_info}, 再生時間: {playback_time:.1f}秒")
                #         last_frame_check = current_time
                #     except Exception:
                #         pass
                
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
        
    def _main_thread_heartbeat(self) -> None:
        """メインスレッドのハートビート（GUI スレッドで実行される）"""
        self._last_heartbeat_time = time.time()
        
    def _stop_main_thread_monitor(self) -> None:
        """メインスレッド監視を停止"""
        self.main_thread_monitor_active = False
        if self.main_thread_monitor_thread:
            self.main_thread_monitor_thread = None
        self.log("[DEBUG] メインスレッド監視を停止しました")

    def _cancel_download(self) -> None:
        """ダウンロードプロセスをキャンセルする"""
        self._download_cancelled = True
        self._subprocess_progress_active = False
        
        proc = self._download_process
        if proc is not None:
            try:
                proc.terminate()
                self.log("ダウンロードプロセスを終了しました")
                # プロセスの終了を待機（最大2秒）
                try:
                    proc.wait(timeout=2)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    self.log("ダウンロードプロセスを強制終了しました")
            except Exception as e:
                self.log(f"ダウンロードキャンセルエラー: {e}")
            finally:
                self._download_process = None
        
        # ダウンロード中フラグをリセット
        self.download_in_progress = False
        
        # 進捗バーを非表示
        self.hide_download_progress()
        
        # 一時ダウンロードファイルを削除（部分的にダウンロードされたファイル）
        try:
            import glob
            partial_files = glob.glob("data/*.part") + glob.glob("data/*.ytdl")
            for f in partial_files:
                try:
                    os.remove(f)
                    self.log(f"部分ダウンロードファイルを削除: {f}")
                except Exception:
                    pass
        except Exception:
            pass
    
    def _find_yt_dlp_executable(self) -> str:
        """yt-dlp実行ファイルのパスを検索（exe環境対応）"""
        from ytdlpSpout.core import get_executable_dir
        
        exe_dir = get_executable_dir()
        yt_dlp_name = "yt-dlp.exe" if sys.platform == "win32" else "yt-dlp"
        
        # 1. exe同階層のbinディレクトリを確認
        bin_dir = os.path.join(exe_dir, 'bin')
        yt_dlp_in_bin = os.path.join(bin_dir, yt_dlp_name)
        if os.path.exists(yt_dlp_in_bin):
            return yt_dlp_in_bin
        
        # 2. exe同階層を確認
        yt_dlp_in_exe_dir = os.path.join(exe_dir, yt_dlp_name)
        if os.path.exists(yt_dlp_in_exe_dir):
            return yt_dlp_in_exe_dir
        
        # 3. システムPATHから検索（Python環境のyt-dlpを含む）
        import shutil
        yt_dlp_path = shutil.which("yt-dlp")
        if yt_dlp_path:
            return yt_dlp_path
        
        # 4. 見つからなければ名前だけ返す（PATHに存在することを期待）
        return yt_dlp_name

    def start_subprocess_download(self, url: str) -> None:
        """別プロセスでダウンロードを実行（yt-dlp CLIを使用、exe環境対応）"""
        import json
        from ytdlpSpout.core import get_optimal_format_string
        
        # キャンセルフラグをリセット
        self._download_cancelled = False
        
        def run_subprocess() -> None:
            try:
                # キャンセルチェック
                if self._download_cancelled:
                    return
                
                # URLをクリーンアップ（プレイリストパラメータを除去）
                cleaned_url = clean_playlist_url(url)
                if cleaned_url != url:
                    self.root.after(0, self.log, "プレイリストURL検出：単体動画として処理します")
                
                self.root.after(0, self.log, f"別プロセスダウンロード開始: {cleaned_url}")
                
                # 進捗バーを表示して初期状態を設定
                def init_subprocess_progress():
                    # 進捗バーを表示（高さを復元）
                    self.progress_frame.configure(height=80)  # 適切な高さに設定
                    self.progress_frame.pack_configure(pady=4)  # パディングを復元
                    # 初期進捗情報を設定
                    initial_progress = {
                        'percent': 0.0,
                        'downloaded_bytes': 0,
                        'total_bytes': 0,
                        'speed': '準備中...',
                        'eta': '',
                        'filename': 'ダウンロード準備中...'
                    }
                    self.update_download_progress(initial_progress)
                    self.update_seekbar_color(UIConfig.SEEKBAR_KNOB_DOWNLOADING)
                
                self.root.after(0, init_subprocess_progress)
                self._subprocess_progress_active = True
                self.download_in_progress = True
                
                # dataディレクトリを作成
                os.makedirs("data", exist_ok=True)
                
                # yt-dlp実行ファイルを検索
                yt_dlp_path = self._find_yt_dlp_executable()
                self.root.after(0, self.log, f"yt-dlp パス: {yt_dlp_path}")
                
                # ストリーミング時の解像度を取得してダウンロード解像度を制限
                # （4Kデコードは重いので、ストリーミング時と同じ解像度でダウンロード）
                max_download_height = 1440  # デフォルト
                if self.streamer and hasattr(self.streamer, 'height'):
                    streaming_height = self.streamer.height
                    if streaming_height > 0:
                        max_download_height = streaming_height
                        self.root.after(0, self.log, f"ダウンロード解像度制限: {max_download_height}p (ストリーミング解像度に合わせる)")
                
                # フォーマット文字列を取得
                format_str, codec_info = get_optimal_format_string(max_height=max_download_height)
                self.root.after(0, self.log, f"フォーマット設定: {codec_info}")
                
                # yt-dlp コマンドを構築（CLIモード、進捗出力付き）
                cmd = [
                    yt_dlp_path,
                    "--format", format_str,
                    "--output", "data/%(id)s.%(ext)s",
                    "--no-playlist",
                    "--newline",  # 進捗を新しい行で出力
                    "--progress",  # 進捗を表示
                    "--no-colors",  # カラー出力を無効化
                    "--no-warnings",
                    cleaned_url
                ]
                
                # 全サイト共通のCookieファイル（リゾルバーと同じ候補）
                cookie_file = YtDlpAsyncResolver.get_default_cookie_file(Path(__file__).parent)
                if cookie_file:
                    cmd.insert(-1, "--cookies")
                    cmd.insert(-1, cookie_file)
                
                self.root.after(0, self.log, f"ダウンロードコマンド: {' '.join(cmd[:5])}...")
                
                # 別プロセスで実行
                creationflags = subprocess.CREATE_NO_WINDOW if hasattr(subprocess, 'CREATE_NO_WINDOW') else 0
                process = subprocess.Popen(
                    cmd,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.STDOUT,  # stderrもstdoutにマージ
                    text=True,
                    bufsize=1,
                    creationflags=creationflags
                )
                
                # プロセスを保持（キャンセル用）
                self._download_process = process
                
                downloaded_file = None
                last_progress_update = 0
                
                # 出力を読み取りながら進捗を解析
                try:
                    for line in process.stdout:
                        # キャンセルチェック
                        if self._download_cancelled:
                            process.terminate()
                            self.root.after(0, self.log, "ダウンロードがキャンセルされました")
                            return
                        
                        line = line.strip()
                        if not line:
                            continue
                        
                        # yt-dlpの進捗出力を解析
                        # 例: [download]  45.3% of  123.45MiB at  5.67MiB/s ETA 00:15
                        if "[download]" in line and "%" in line:
                            try:
                                import re
                                # パーセンテージを抽出
                                percent_match = re.search(r'(\d+\.?\d*)%', line)
                                # サイズを抽出（例: "of  123.45MiB" または "of ~123.45MiB"）
                                size_match = re.search(r'of\s+~?(\d+\.?\d*)(Ki?B|Mi?B|Gi?B)', line)
                                # 速度を抽出
                                speed_match = re.search(r'at\s+(\d+\.?\d*\s*\w+/s)', line)
                                # ETAを抽出
                                eta_match = re.search(r'ETA\s+(\S+)', line)
                                
                                percent = float(percent_match.group(1)) if percent_match else 0
                                
                                # サイズをバイトに変換
                                total_bytes = 0
                                if size_match:
                                    size_val = float(size_match.group(1))
                                    size_unit = size_match.group(2).upper()
                                    if 'G' in size_unit:
                                        total_bytes = int(size_val * 1024 * 1024 * 1024)
                                    elif 'M' in size_unit:
                                        total_bytes = int(size_val * 1024 * 1024)
                                    elif 'K' in size_unit:
                                        total_bytes = int(size_val * 1024)
                                    else:
                                        total_bytes = int(size_val)
                                
                                downloaded_bytes = int(total_bytes * percent / 100) if total_bytes > 0 else 0
                                speed = speed_match.group(1) if speed_match else ""
                                eta = eta_match.group(1) if eta_match else ""
                                
                                # 進捗更新（頻度制限：0.5秒に1回）
                                current_time = time.time()
                                if current_time - last_progress_update >= 0.5:
                                    last_progress_update = current_time
                                    gui_progress = {
                                        'percent': percent,
                                        'downloaded_bytes': downloaded_bytes,
                                        'total_bytes': total_bytes,
                                        'speed': speed,
                                        'eta': eta,
                                        'filename': ''
                                    }
                                    self.root.after(0, self.update_download_progress, gui_progress)
                            except Exception:
                                pass
                        
                        # ダウンロード先ファイルパスを抽出
                        # 例: [download] Destination: data/xxxxx.mp4
                        elif "[download] Destination:" in line:
                            downloaded_file = line.split("Destination:")[-1].strip()
                        elif "[Merger]" in line and "Merging formats into" in line:
                            # マージ後のファイル名を抽出
                            merge_match = re.search(r'"([^"]+)"', line)
                            if merge_match:
                                downloaded_file = merge_match.group(1)
                        elif "has already been downloaded" in line:
                            # 既にダウンロード済みの場合
                            match = re.search(r'\[download\]\s+(.+?)\s+has already been downloaded', line)
                            if match:
                                downloaded_file = match.group(1)
                
                except Exception as e:
                    self.root.after(0, self.log, f"進捗読み取りエラー: {e}")
                
                # プロセス完了を待機
                return_code = process.wait()
                self._download_process = None
                
                # キャンセルチェック
                if self._download_cancelled:
                    return
                
                if return_code == 0:
                    # ダウンロード成功
                    # ファイルパスが取得できなかった場合、dataフォルダから最新ファイルを探す
                    if not downloaded_file or not os.path.exists(downloaded_file):
                        import glob
                        data_files = [f for f in glob.glob("data/*") 
                                     if not f.endswith('.part') and not f.endswith('.ytdl') 
                                     and os.path.isfile(f)]
                        if data_files:
                            downloaded_file = max(data_files, key=os.path.getctime)
                    
                    if downloaded_file and os.path.exists(downloaded_file):
                        # 模擬進捗を停止
                        self._subprocess_progress_active = False
                        # 完了時の進捗表示（切り替え中メッセージ）
                        completion_progress = {
                            'percent': 100.0,
                            'downloaded_bytes': 0,
                            'total_bytes': 0,
                            'speed': '完了 - ローカル再生に切り替え中...',
                            'eta': '',
                            'filename': os.path.basename(downloaded_file)
                        }
                        self.root.after(0, self.update_download_progress, completion_progress)
                        # 進捗バーを不確定モード（アニメーション）に変更
                        self.root.after(0, lambda: self.progress_bar.configure(mode="indeterminate"))
                        self.root.after(0, lambda: self.progress_bar.start())
                        # 進捗バーは切り替え完了後に非表示にする（hide_download_progressは削除）
                        
                        self.root.after(0, self.log, f"別プロセスダウンロード完了: {downloaded_file}")
                        # シームレス切り替えを実行
                        self.root.after(0, self.switch_to_local_file, downloaded_file)
                    else:
                        self._subprocess_progress_active = False
                        self.root.after(0, self.hide_download_progress)
                        self.root.after(0, self.log, "ダウンロード完了しましたが、ファイルが見つかりませんでした")
                else:
                    # エラー時も進捗バーを非表示
                    self._subprocess_progress_active = False
                    self.root.after(0, self.hide_download_progress)
                    self.root.after(0, self.log, f"ダウンロードエラー（終了コード: {return_code}）")
                    
            except Exception as e:
                # エラー時も進捗バーを非表示
                self._subprocess_progress_active = False
                self._download_process = None
                self.root.after(0, self.hide_download_progress)
                self.root.after(0, self.log, f"別プロセス実行エラー: {e}")
            finally:
                self.download_in_progress = False
        
        # バックグラウンドで実行
        threading.Thread(target=run_subprocess, daemon=True).start()
    
    def switch_to_local_file(self, file_path: str):
        """ダウンロード完了後にローカルファイルへ切り替え（簡略版）
        
        C++ DLLバックエンドではスライスローディングにより
        シームレス切り替えが不要になったため、単純な切り替えを行う。
        """
        try:
            self.log(f"ローカルファイルへの切り替え開始: {file_path}")
            
            if not os.path.exists(file_path):
                self.log(f"エラー: ローカルファイルが見つかりません: {file_path}")
                return
            
            abs_file_path = os.path.abspath(file_path)
            self.local_video_path = abs_file_path
            
            # 旧ストリーマーの現在位置を取得
            current_position = 0.0
            old_streamer = self.streamer
            if old_streamer:
                try:
                    current_position = old_streamer.playback_time
                except Exception:
                    pass
                
                # 旧ストリーマーのstop_cbを無効化（新ストリーマーを誤って停止しないため）
                old_streamer._stop_cb = None
                
                # 旧ストリーマーを停止
                try:
                    old_streamer.stop()
                except Exception as e:
                    self.log(f"旧ストリーマー停止エラー: {e}")
            
            # 新しいストリーマーを作成・開始
            max_res, manual_res = self._get_resolution_settings()
            
            new_streamer = self._create_streamer(
                abs_file_path,
                self.sender_var.get(),
                max_resolution=max_res,
                manual_resolution=manual_res,
                loop_vod=self.vod_loop.get(),
                log_cb=lambda m: self.root.after(0, self.log, m),
                stop_cb=self.on_auto_stop,
                init_ok_cb=lambda: self.root.after(0, self.on_stream_start_success),
                external_spout_sender=None
            )
            
            self.streamer = new_streamer
            new_streamer.start()
            
            # 前の再生位置にシーク
            if current_position > 0:
                new_streamer.seek(current_position)
                self.log(f"再生位置を復元: {self.format_time(current_position)}")
            
            # ステータス更新
            original_url_display = self.original_url if self.original_url else "不明"
            self.info_label.configure(
                text=f"ローカルファイル再生中（{original_url_display} からダウンロード済み）"
            )
            self.update_seekbar_color(UIConfig.SEEKBAR_KNOB_LOCAL)
            
            # 進捗バーを非表示
            self.hide_download_progress()
            
            self.log(f"ローカルファイルへの切り替え完了: {file_path}")
            
        except Exception as e:
            self.log(f"ローカルファイル切り替えエラー: {e}")
            import traceback
            self.log(traceback.format_exc())

    
    def _log_direct(self, msg: str) -> None:
        """メインスレッドからの直接ログ処理"""
        try:
            self.log_text.insert("end", msg + "\n")
            self.log_text.see("end")
        except Exception:
            pass
    
    def process_log_queue(self) -> None:
        """ログキューを処理"""
        try:
            self.log_processing = True
            processed_count = 0
            max_process_per_batch = 5
            
            while not self.log_queue.empty() and processed_count < max_process_per_batch:
                try:
                    msg = self.log_queue.get_nowait()
                    self.log_text.insert("end", msg + "\n")
                    processed_count += 1
                except Exception:
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

    def get_shared_spout_sender(self, sender_name: str):
        """共有SpoutSenderを取得または作成する
        
        注意: C++ DLLがSpout送信を担当するため、このメソッドは何もしない。
        互換性のためにNoneを返す。
        """
        # C++ DLL (Spout有効ビルド) がSpout送信を担当
        # Python側のSpoutGLは使用しない（競合回避）
        return None
    
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

    def on_seek_drag(self, value: float) -> None:
        """シークバードラッグ時の処理"""
        if self._seeking:
            self.seek_value = value
            current_t = self.format_time(value)
            total_t = self.format_time(self.duration_cache)
            self.time_label.configure(text=f"{current_t} / {total_t}")

    def on_seek_press(self, event: tk.Event) -> None:
        """シークバー押下時の処理"""
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

    def on_seek_release(self, event: tk.Event) -> None:
        """シークバーリリース時の処理"""
        if self.streamer and self.streamer.is_vod and self._seeking:
            self._seeking = False
            # マウスリリース時の最終的な値を元にシーク
            final_seek_value = self.seek_slider.get()
            self.streamer.seek(final_seek_value)

    def update_download_progress(self, progress_data: dict[str, Any]) -> None:
        """ダウンロード進捗を更新する（ファイルサイズベース）"""
        try:
            # 進捗データを更新
            self.download_progress.update(progress_data)
            
            # プログレスバーを表示（まだ表示されていない場合のみ）
            if not self.progress_frame.winfo_viewable():
                # info_labelの上に表示するため、info_labelの前に挿入
                self.progress_frame.pack(fill="x", padx=8, pady=4, before=self.info_label)
                self.log("進捗バーを表示しました")
            
            # ファイルサイズからパーセンテージを計算
            downloaded = self.download_progress.get('downloaded_bytes', 0)
            total = self.download_progress.get('total_bytes', 0)
            
            if total > 0 and downloaded > 0:
                # ファイルサイズベースでパーセンテージを計算
                percent = (downloaded / total) * 100
                self.progress_bar.set(percent / 100.0)  # CTkProgressBarは0-1の範囲
            else:
                # パーセンテージが直接提供されている場合
                percent = self.download_progress.get('percent', 0.0)
                if percent > 0:
                    self.progress_bar.set(percent / 100.0)
            
            # 詳細情報を更新（ファイルサイズベースの表示）
            info_parts = []
            
            # ダウンロード済み/合計サイズ（メインの進捗表示）
            if downloaded > 0:
                downloaded_mb = downloaded / (1024 * 1024)
                if total > 0:
                    total_mb = total / (1024 * 1024)
                    percent_calc = (downloaded / total) * 100
                    info_parts.append(f"💾 {downloaded_mb:.1f}MB / {total_mb:.1f}MB ({percent_calc:.1f}%)")
                else:
                    info_parts.append(f"💾 {downloaded_mb:.1f}MB ダウンロード済み")
            elif percent > 0:
                # バイト数がない場合はパーセンテージのみ表示
                info_parts.append(f"📊 {percent:.1f}%")
            
            # ダウンロード速度
            speed = self.download_progress.get('speed', '')
            if speed and speed != '':
                info_parts.append(f"⚡ {speed}")
            
            # 残り時間
            eta = self.download_progress.get('eta', '')
            if eta and eta != 'Unknown' and eta != '':
                info_parts.append(f"⏱️ 残り {eta}")
            
            # 情報を表示
            info_text = " | ".join(info_parts) if info_parts else "ダウンロード準備中..."
            self.progress_info.configure(text=info_text)
            
        except Exception as e:
            self.log(f"進捗更新エラー: {e}")

    def hide_download_progress(self) -> None:
        """ダウンロード進捗表示を非表示にする"""
        try:
            self.progress_frame.pack_forget()
            self.progress_bar.set(0)
            self.progress_info.configure(text="")
            self.download_progress = self._create_empty_progress()
        except Exception:
            pass

    def on_download_complete(self) -> None:
        """ダウンロード完了処理（Streamボタンに統合済みのため、簡素化）"""
        # 進捗バーを非表示にする
        self.hide_download_progress()
        
        self.log(f"動画をローカルに保存しました: {self.local_video_path}")
        self.btn_start.configure(state="normal")
        self.info_label.configure(text="準備完了。")
        # 注意：自動再生はon_streamで処理されるため、ここでは実行しない


    def _start_spout_stream(self, video_source_url: str) -> None:
        """Spoutストリームを開始する内部ヘルパー関数"""
        if self.streamer:
            self.log("エラー: 既にストリームがアクティブです。")
            return
        
        # UIを即座に更新
        self.btn_start.configure(state="disabled")
        self.btn_stop.configure(state="normal")
        self.info_label.configure(text="ストリーミング準備中...")
        self.log(f"{video_source_url} からストリーミングを開始します...")
        
        # プレビュー表示をリセット
        if hasattr(self, '_no_signal_shown'):
            delattr(self, '_no_signal_shown')
        self.preview_label.configure(text="")
        
        # ローカルファイルかどうか判定
        is_local_file = os.path.exists(video_source_url) and os.path.isfile(video_source_url)
        
        if is_local_file:
            self.log(f"ローカルファイルを検出: {video_source_url}")
            self._start_local_file_stream(video_source_url)
        else:
            self._start_url_stream(video_source_url)
    
    def _get_resolution_settings(self) -> tuple[tuple[int, int] | None, tuple[int, int] | None]:
        """解像度設定を取得する共通メソッド"""
        max_res: tuple[int, int] | None = None
        manual_res: tuple[int, int] | None = None
        
        # パフォーマンス制限（1440p）
        if self.perf_limit.get() and not self.max_enable.get() and not self.manual_enable.get():
            max_res = UIConfig.DEFAULT_MAX_RESOLUTION
        
        # 最大解像度設定
        try:
            maxw = self.maxw_var.get().strip()
            maxh = self.maxh_var.get().strip()
            if self.max_enable.get() and maxw and maxh:
                max_res = (int(maxw), int(maxh))
        except (ValueError, TypeError):
            # Issue GUI-3: 無警告のままCap設定が無効化されるのを防ぐため、
            # 何が無効化されたかをログに明示する
            self.log(f"警告: 解像度（Max）の値が不正です。Cap設定を適用しません: 幅='{maxw}' 高さ='{maxh}'")

        # 手動解像度設定
        try:
            manw = self.manw_var.get().strip()
            manh = self.manh_var.get().strip()
            if self.manual_enable.get() and manw and manh:
                manual_res = (int(manw), int(manh))
        except (ValueError, TypeError):
            # Issue GUI-3: 無警告のままManual設定が無効化されるのを防ぐため、
            # 何が無効化されたかをログに明示する
            self.log(f"警告: 解像度（Manual）の値が不正です。Manual設定を適用しません: 幅='{manw}' 高さ='{manh}'")

        return max_res, manual_res
    
    def _create_streamer(
        self,
        video_source: str,
        sender: str,
        max_resolution=None,
        manual_resolution=None,
        loop_vod: bool = False,
        verbose: bool = True,
        log_cb=None,
        stop_cb=None,
        init_ok_cb=None,
        external_spout_sender=None
    ):
        """
        ストリーマーを作成するファクトリメソッド
        
        設計案準拠: すべてC++ DLLを使用（Spout送信もC++側）
        - ローカルファイル: C++ DLL
        - URL: C++ DLL（YtDlpResolver + CustomIOContext経由）
        
        SpoutGLは使用しない（競合回避）
        """
        import os
        is_local_file = os.path.exists(video_source) and os.path.isfile(video_source)
        
        # C++ DLLが利用可能な場合は常にNativeStreamerWrapperを使用
        if NATIVE_BACKEND_AVAILABLE:
            if is_local_file:
                self.log(f"[Backend] C++ DLL（ローカルファイル）")
            else:
                self.log(f"[Backend] C++ DLL（URL/ストリーミング）")
            return NativeStreamerWrapper(
                video_url=video_source,
                sender_name=sender,
                max_resolution=max_resolution,
                manual_resolution=manual_resolution,
                loop_vod=loop_vod,
                verbose=verbose,
                log_cb=log_cb,
                stop_cb=stop_cb,
                init_ok_cb=init_ok_cb,
                external_spout_sender=None  # C++ DLLがSpout送信を担当
            )
        else:
            # C++ DLLが利用できない場合のみPython Streamerにフォールバック
            self.log(f"[Backend] Python Streamer (fallback)")
            return Streamer(
                video_source,
                sender,
                max_resolution=max_resolution,
                manual_resolution=manual_resolution,
                loop_vod=loop_vod,
                verbose=verbose,
                log_cb=log_cb,
                stop_cb=stop_cb,
                init_ok_cb=init_ok_cb,
                external_spout_sender=external_spout_sender
            )

    def _commit_started_streamer(self, streamer, generation: int, on_committed=None) -> bool:
        """バックグラウンドで構築済みのStreamerをself.streamerへ確定させる。

        Start処理（URL解決やStreamer構築）はバックグラウンドスレッドで行われるため、
        構築完了前にStop（on_stop）が呼ばれて世代カウンタ(_stream_start_generation)が
        進んでいる可能性がある。その場合はここで検出し、構築済みのstreamerを
        即座にstop()して破棄し、self.streamerへの代入も.start()も行わない
        （孤立ストリーム防止：Issue GUI-2）。

        generationチェックとself.streamerへの代入は_stream_lockで保護し、
        on_stop()側の世代インクリメント＋streamer読み取りと原子的に扱う。

        戻り値: 採用してstart()まで実行できた場合True、破棄した場合False。
        """
        with self._stream_lock:
            if generation != self._stream_start_generation:
                stale = True
            else:
                self.streamer = streamer
                stale = False

        if stale:
            self.log("Stopが先に実行されたため、構築済みのストリーマーを破棄します。")
            try:
                streamer.stop()
            except Exception as e:
                self.log(f"破棄対象ストリーマーの停止警告: {e}")
            return False

        streamer.start()
        if on_committed:
            on_committed()
        return True

    def _start_local_file_stream(self, file_path: str) -> None:
        """ローカルファイルからのストリーミングを開始"""
        try:
            sender = self.sender_var.get().strip() or DEFAULT_SENDER_NAME
            
            self.streamer = self._create_streamer(
                file_path, sender,
                max_resolution=None,
                manual_resolution=None,
                loop_vod=self.vod_loop.get(),
                verbose=True,
                log_cb=self._log_direct,
                stop_cb=self.on_auto_stop,
                init_ok_cb=self.on_stream_start_success,
                external_spout_sender=None  # C++ DLLがSpout送信を担当
            )
            self.streamer.start()
            self.update_seekbar_color(UIConfig.SEEKBAR_KNOB_LOCAL)
            
        except Exception as e:
            # 例外発生時点ではself.streamer=...の代入が未完了（Noneのまま）のため、
            # on_auto_stop()ではボタン状態が復帰しない。他の開始失敗経路と同じ
            # _handle_start_error()でStart/Stopボタンを正しい状態に戻す。
            self._handle_start_error(f"ローカルファイルストリーミング開始エラー: {e}")
    
    def _start_url_stream(self, video_source_url: str) -> None:
        """URLからのストリーミングを開始（非同期URL解決対応）"""
        sender = self.sender_var.get().strip() or DEFAULT_SENDER_NAME
        max_res, manual_res = self._get_resolution_settings()
        
        # yt-dlp対応URLかどうか判定
        needs_ytdlp = False
        if YTDLP_RESOLVER_AVAILABLE:
            needs_ytdlp = YtDlpAsyncResolver.is_ytdlp_url(video_source_url)
        
        if needs_ytdlp and NATIVE_BACKEND_AVAILABLE:
            # yt-dlp URLの場合: 非同期でURL解決を開始
            self._start_url_stream_with_resolver(video_source_url, sender, max_res, manual_res)
        else:
            # 直接URL/ローカルファイルの場合: 即座にストリーマー作成
            self._start_url_stream_direct(video_source_url, sender, max_res, manual_res)
    
    def _start_url_stream_with_resolver(
        self, 
        video_source_url: str, 
        sender: str, 
        max_res, 
        manual_res
    ) -> None:
        """yt-dlp URL解決を行ってからストリーミングを開始"""
        self.log("URL解決中...")
        self._url_resolving = True
        # Stop後にキャンセルされた解決結果を無視するための世代カウンタ
        self._url_resolve_generation += 1
        generation = self._url_resolve_generation

        # 全サイト共通のCookieファイル（存在すれば自動で使用）
        cookie_file = YtDlpAsyncResolver.get_default_cookie_file(Path(__file__).parent)

        # 新しいリゾルバーを作成
        self._ytdlp_resolver = YtDlpAsyncResolver(
            log_cb=lambda m: self.root.after(0, self.log, m),
            cookie_file=cookie_file,
            verbose=False
        )

        # 非同期でURL解決を開始
        future = self._ytdlp_resolver.resolve_async(video_source_url)

        def on_resolve_complete() -> None:
            """URL解決完了時のコールバック"""
            # Stop等で世代が進んでいれば、この解決結果は無視する（勝手な再生開始防止）
            if generation != self._url_resolve_generation:
                return
            try:
                result = future.result(timeout=0)  # 既に完了しているはず
                self._url_resolving = False

                if result is None:
                    self.log("[yt-dlp] URL解決に失敗。直接URLとして再生を試みます")
                    # フォールバック: 従来の方法で再生
                    self._start_url_stream_direct(video_source_url, sender, max_res, manual_res)
                    return
                
                self.log(f"[yt-dlp] URL解決完了: {result.title}")
                
                # 解決済みURLでストリーマーを作成
                def start_resolved_stream() -> None:
                    # Start処理中にStopが押された場合に構築済みStreamerを
                    # 破棄できるよう、構築前の世代を捕捉しておく（Issue GUI-2）
                    stream_generation = self._stream_start_generation
                    try:
                        streamer = NativeStreamerWrapper(
                            video_url=video_source_url,  # 元のURL（表示用）
                            sender_name=sender,
                            max_resolution=max_res,
                            manual_resolution=manual_res,
                            loop_vod=self.vod_loop.get(),
                            verbose=True,
                            log_cb=lambda m: self.root.after(0, self.log, m),
                            stop_cb=lambda: self.root.after(0, self.on_auto_stop),
                            init_ok_cb=lambda: self.root.after(0, self.on_stream_start_success),
                            external_spout_sender=None,
                            pre_resolved_url=result.stream_url,  # 解決済みURL
                            pre_resolved_headers=result.http_headers,  # HTTPヘッダー
                            pre_resolved_is_hls=result.is_hls  # yt-dlpのprotocolに基づくHLS判定（None=自動）
                        )
                        self._commit_started_streamer(
                            streamer,
                            stream_generation,
                            on_committed=lambda: self.root.after(
                                0, lambda: self.update_seekbar_color(UIConfig.SEEKBAR_KNOB_DOWNLOADING)
                            )
                        )
                    except Exception as e:
                        self.root.after(0, self._handle_start_error, f"ストリーミング開始エラー: {e}")

                threading.Thread(target=start_resolved_stream, daemon=True).start()
                
            except Exception as e:
                self._url_resolving = False
                self.log(f"[yt-dlp] URL解決エラー: {e}")
                # フォールバック
                self._start_url_stream_direct(video_source_url, sender, max_res, manual_res)
        
        def poll_resolution() -> None:
            """URL解決の完了をポーリング"""
            # Stop等で世代が進んでいれば、このポーリングループは打ち切る
            if generation != self._url_resolve_generation:
                return
            if future.done():
                on_resolve_complete()
            else:
                # まだ完了していない場合は100ms後に再チェック
                self.root.after(100, poll_resolution)
        
        # ポーリング開始
        self.root.after(50, poll_resolution)
    
    def _start_url_stream_direct(
        self, 
        video_source_url: str, 
        sender: str, 
        max_res, 
        manual_res
    ) -> None:
        """直接URLでストリーミングを開始（従来の処理）"""
        def start_streaming_thread() -> None:
            # Start処理中にStopが押された場合に構築済みStreamerを
            # 破棄できるよう、構築前の世代を捕捉しておく（Issue GUI-2）
            stream_generation = self._stream_start_generation
            try:
                streamer = self._create_streamer(
                    video_source_url,
                    sender,
                    max_resolution=max_res,
                    manual_resolution=manual_res,
                    loop_vod=self.vod_loop.get(),
                    log_cb=lambda m: self.root.after(0, self.log, m),
                    stop_cb=lambda: self.root.after(0, self.on_auto_stop),
                    init_ok_cb=lambda: self.root.after(0, self.on_stream_start_success),
                    external_spout_sender=None  # C++ DLLがSpout送信を担当
                )
                self._commit_started_streamer(
                    streamer,
                    stream_generation,
                    on_committed=lambda: self.root.after(
                        0, lambda: self.update_seekbar_color(UIConfig.SEEKBAR_KNOB_DOWNLOADING)
                    )
                )
            except Exception as e:
                self.root.after(0, self._handle_start_error, f"ストリーミング開始エラー: {e}")

        threading.Thread(target=start_streaming_thread, daemon=True).start()

    def on_stream(self) -> None:
        """「Stream」ボタン：スライスローディングでストリーミング再生
        
        スライスローディングにより、ダウンロード完了を待たずに即座に再生開始。
        チャンクは自動的にキャッシュされ、全チャンクがダウンロードされたら
        IsFullyCached() == true となる。ローカルファイルへの切り替えは不要。
        """
        url = self.url_var.get().strip()
        if not url:
            self.log("エラー: ストリーミングするURLが入力されていません。")
            return

        # メインスレッド監視を開始（ブロッキング検出のため）
        self._start_main_thread_monitor()

        # 元のURLを保存（ローカルファイルでない場合のみ）
        if not os.path.exists(url):
            self.original_url = url

        # スライスローディングでストリーミング再生
        # C++ DLLがCustomIOContext経由でチャンク読み込み・キャッシュを行う
        self._start_spout_stream(url)

    def _start_background_download(self, url: str) -> None:
        """VOD用: ストリーミング再生中にバックグラウンドでダウンロード"""
        if self.download_in_progress:
            self.root.after(0, self.log, "すでにバックグラウンドダウンロード中です。")
            return
        
        # URLをクリーンアップ（プレイリストパラメータを除去）
        cleaned_url = clean_playlist_url(url)
        if cleaned_url != url:
            self.root.after(0, self.log, "プレイリストURL検出：単体動画として処理します")
        
        self.download_in_progress = True
        self.root.after(0, self.log, "バックグラウンドダウンロードを準備中...")
        
        # 進捗バーを表示
        def show_progress_bar() -> None:
            if not self.progress_frame.winfo_viewable():
                self.progress_frame.pack(fill="x", padx=8, pady=4, before=self.info_label)
                self.progress_info.configure(text="ダウンロード準備中...")
                self.progress_bar.set(0)
        
        self.root.after(0, show_progress_bar)
        
        def download_video() -> None:
            try:
                # Windowsでスレッド優先度を下げてSpout送信への影響を軽減
                try:
                    import ctypes
                    kernel32 = ctypes.windll.kernel32
                    handle = kernel32.GetCurrentThread()
                    kernel32.SetThreadPriority(handle, -1)  # THREAD_PRIORITY_BELOW_NORMAL
                except Exception:
                    pass  # 失敗しても継続
                
                self.root.after(0, self.log, "バックグラウンドダウンロード処理を開始します...")
                os.makedirs("data", exist_ok=True)
                
                # 短時間待機でCPUリソースを他に譲る
                for i in range(10):
                    time.sleep(0.1)
                
                # カスタムロガーを作成（完全に非同期）
                def async_log(msg: str) -> None:
                    self.root.after(0, self.log, msg)
                custom_logger = YtdlpLogger(async_log)
                
                # ストリーミング時と同じ最適化された品質設定を使用
                format_str, codec_info = get_optimal_format_string()
                
                ydl_opts_info = {
                    'format': format_str,  # 最適化されたフォーマット文字列を使用
                    'outtmpl': 'data/%(id)s.%(ext)s',
                    'nocheckcertificate': True,
                    'logger': custom_logger,
                    'quiet': False,  # ログを表示するためFalseに変更
                    'no_warnings': False,
                    'extract_flat': False,  # 詳細情報が必要
                    'verbose': False,  # 詳細ログを無効化（プレビューフリーズを防ぐ）
                    'noplaylist': True,  # プレイリスト無効化（単体動画のみダウンロード）
                }
                
                self.root.after(0, self.log, f"動画情報取得品質設定: {codec_info}")
                
                with yt_dlp.YoutubeDL(ydl_opts_info) as ydl:
                    info = ydl.extract_info(cleaned_url, download=False)
                    final_filename = ydl.prepare_filename(info)
                    # 動画情報をログに出力
                    title = info.get('title', 'Unknown')
                    duration = info.get('duration', 0)
                    self.root.after(0, self.log, f"動画情報取得完了: {title} (長さ: {duration}秒)")

                # ダウンロード開始前に少し待機（プチフリ軽減）
                time.sleep(0.1)
                
                # プログレスフックの定義（進捗バー対応版）
                def progress_hook(d: dict[str, Any]) -> None:
                    try:
                        # サブプロセス進捗が活動中の場合は通常の進捗フックを無効化
                        if hasattr(self, '_subprocess_progress_active') and self._subprocess_progress_active:
                            return
                        
                        if d['status'] == 'downloading':
                            # 詳細な進捗情報を取得
                            progress_data = {}
                            
                            # パーセンテージ
                            if '_percent_str' in d:
                                percent_str = d['_percent_str'].strip('%')
                                try:
                                    progress_data['percent'] = float(percent_str)
                                except ValueError:
                                    progress_data['percent'] = 0.0
                            
                            # バイト数
                            progress_data['downloaded_bytes'] = d.get('downloaded_bytes', 0)
                            progress_data['total_bytes'] = d.get('total_bytes', 0)
                            
                            # 速度とETA
                            progress_data['speed'] = d.get('_speed_str', '')
                            progress_data['eta'] = d.get('_eta_str', '')
                            
                            # ファイル名
                            progress_data['filename'] = d.get('filename', '')
                            if not progress_data['filename'] and hasattr(progress_hook, 'filename'):
                                progress_data['filename'] = progress_hook.filename
                            
                            # GUI更新（メインスレッドで実行）
                            self.root.after(0, self.update_download_progress, progress_data)
                            
                            # ログ出力頻度を制限（1秒間隔）
                            if hasattr(progress_hook, 'last_log_time'):
                                import time
                                current_time = time.time()
                                if current_time - progress_hook.last_log_time < 2.0:  # 2秒間隔に変更
                                    return
                                progress_hook.last_log_time = current_time
                            else:
                                import time
                                progress_hook.last_log_time = time.time()
                            
                            # 簡潔なログ出力
                            percent = progress_data.get('percent', 0)
                            speed = progress_data.get('speed', '')
                            if percent > 0:
                                self.root.after(0, self.log, f"ダウンロード中: {percent:.1f}% ({speed})")
                            
                        elif d['status'] == 'finished':
                            # ダウンロード完了時は進捗バーを非表示
                            self.root.after(0, self.hide_download_progress)
                            self.root.after(0, self.log, "ダウンロード完了")
                        elif d['status'] == 'error':
                            # エラー時も進捗バーを非表示
                            self.root.after(0, self.hide_download_progress)
                            self.root.after(0, self.log, f"ダウンロードエラー: {d.get('_error_str', 'Unknown error')}")
                    except Exception as e:
                        # プログレスフックのエラーでダウンロードを停止させない
                        self.root.after(0, self.log, f"進捗処理エラー: {e}")
                
                # ファイル名をプログレスフックに保存
                progress_hook.filename = final_filename
                
                # ダウンロード開始の明示的なログと進捗バー初期化
                self.root.after(0, self.log, f"ダウンロードを開始します: {final_filename}")
                
                # 進捗バーの初期状態を設定
                def init_progress():
                    # サブプロセス進捗が活動中でない場合のみ進捗バー初期化
                    if not (hasattr(self, '_subprocess_progress_active') and self._subprocess_progress_active):
                        if not self.progress_frame.winfo_viewable():
                            self.progress_frame.pack(fill="x", padx=8, pady=4, before=self.info_label)
                        # 初期進捗情報を設定
                        initial_progress = {
                            'percent': 0.0,
                            'downloaded_bytes': 0,
                            'total_bytes': 0,
                            'speed': '',
                            'eta': '',
                            'filename': final_filename
                        }
                        self.update_download_progress(initial_progress)
                
                self.root.after(0, init_progress)
                
                # ダウンロード実行
                # ストリーミング時と同じ最適化された品質設定を使用
                from ytdlpSpout.core import get_optimal_format_string
                format_str, codec_info = get_optimal_format_string()
                
                ydl_opts_dl = {
                    'format': format_str,  # 最適化されたフォーマット文字列を使用
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
                    'noplaylist': True,  # プレイリスト無効化（単体動画のみダウンロード）
                }
                
                self.root.after(0, self.log, f"ダウンロード品質設定: {codec_info}")
                
                with yt_dlp.YoutubeDL(ydl_opts_dl) as ydl:
                    import time
                    bg_download_start_time = time.time()
                    self.root.after(0, self.log, f"[DEBUG] BGダウンロード開始: {bg_download_start_time:.3f}")
                    self.root.after(0, self.log, "yt-dlpダウンロード処理を実行中...")
                    
                    ydl.download([cleaned_url])
                    
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
            self.btn_start.configure(state="normal")
            self.info_label.configure(text="準備完了。Startボタンで再生を開始できます。")

    def on_stream_start_success(self) -> None:
        """ストリーミング開始成功時の処理"""
        self.info_label.configure(text="ストリーミング開始")
        if self.streamer and self.streamer.is_vod:
            self.duration_cache = self.streamer.duration
            self.log(f"動画の長さを取得しました: {self.duration_cache} 秒")
            self.seek_slider.configure(state="normal", to=self.duration_cache)
            self.time_label.configure(text=f"00:00 / {self.format_time(self.duration_cache)}")

    def on_stop(self, delete_local_file: bool = True, skip_ui_reset: bool = False) -> None:
        """ストリーミング停止処理"""
        try:
            # URL解決中のリゾルバーをキャンセル（Stop後に解決完了→勝手に再生開始するのを防止）
            self._url_resolving = False
            self._url_resolve_generation += 1
            if self._ytdlp_resolver is not None:
                try:
                    self._ytdlp_resolver.cancel()
                except Exception as e:
                    self.log(f"URL解決キャンセル警告: {e}")

            # ダウンロード中のサブプロセスをキャンセル
            try:
                self._cancel_download()
            except Exception as e:
                self.log(f"ダウンロードキャンセル警告: {e}")

            # ストリーム開始処理の世代を進める。開始処理中（バックグラウンドで
            # Streamer構築中）にStopが呼ばれた場合、後から構築が完了しても
            # 世代不一致により自動的に破棄され、self.streamerには代入されない
            # （孤立ストリーム防止）。self.streamerの読み取り・クリアも同じ
            # ロックの中で行い、_commit_started_streamer側との競合を防ぐ。
            with self._stream_lock:
                self._stream_start_generation += 1
                current_streamer = self.streamer
                self.streamer = None

            # 現在のStreamerを停止
            if current_streamer:
                try:
                    current_streamer.stop()
                except Exception as e:
                    self.log(f"ストリーマー停止警告: {e}")

            # 進捗バーを非表示にする
            self.hide_download_progress()

        finally:
            # メインスレッド監視を停止（動作中の場合のみ）
            if self.main_thread_monitor_active:
                self._stop_main_thread_monitor()

            # 共有SpoutSenderを解放（再生セッション終了のため確実に実行）
            if self.spout_sender:
                try:
                    self.spout_sender.releaseSender()
                    self.log("SpoutSenderを解放しました")
                    del self.spout_sender
                except Exception as e:
                    self.log(f"Spout解放警告: {e}")
                finally:
                    self.spout_sender = None
            
            # UIリセット
            if not skip_ui_reset:
                self.btn_start.configure(state="normal")
                self.btn_stop.configure(state="disabled")
                self.info_label.configure(text="停止しました")
                self.seek_slider.configure(state="disabled")
                self.time_label.configure(text="--:-- / --:--")
                self.update_seekbar_color(UIConfig.SEEKBAR_KNOB_STANDBY)
            
            # ローカルファイルを削除（明示的な停止時のみ）
            if delete_local_file:
                self._delete_local_file_with_retry()
            
            # ゾンビSender防止のためGCを強制実行
            import gc
            gc.collect()
    
    def _delete_local_file_with_retry(self) -> None:
        """ローカルファイルを遅延リトライ付きで削除する"""
        if not self.local_video_path or not os.path.exists(self.local_video_path):
            return
        
        file_to_delete = self.local_video_path
        max_retries = 5
        retry_delay = 0.5  # 500ms
        
        def try_delete():
            for attempt in range(max_retries):
                try:
                    # ファイルが存在するか確認
                    if not os.path.exists(file_to_delete):
                        self.log(f"ファイルは既に削除されています: {file_to_delete}")
                        self.local_video_path = None
                        return True
                    
                    # 削除を試行
                    os.remove(file_to_delete)
                    self.log(f"一時ファイルを削除しました: {file_to_delete}")
                    self.local_video_path = None
                    return True
                    
                except PermissionError:
                    if attempt < max_retries - 1:
                        self.log(f"ファイル削除リトライ {attempt + 1}/{max_retries}: {file_to_delete}")
                        time.sleep(retry_delay)
                    else:
                        self.log(f"警告: ファイルの削除に失敗しました（アクセス拒否）: {file_to_delete}")
                        self.log("ファイルは次回起動時に手動で削除してください")
                        return False
                        
                except Exception as e:
                    self.log(f"一時ファイルの削除エラー: {e}")
                    return False
            
            return False
        
        # バックグラウンドで削除を試行
        threading.Thread(target=try_delete, daemon=True).start()

    def _handle_start_error(self, error_msg: str) -> None:
        """開始エラーの共通処理"""
        self.log(f"エラー: {error_msg}")
        if self.streamer:
            self.streamer.stop()
            self.streamer = None

        # 進捗バーを非表示にする
        self.hide_download_progress()

        # メインスレッド監視を停止（開始失敗時も動作中の場合は必ず停止する）
        if self.main_thread_monitor_active:
            self._stop_main_thread_monitor()

        self.download_in_progress = False
        self.btn_start.configure(state="normal")
        self.btn_stop.configure(state="disabled")
        self.info_label.configure(text="エラーが発生しました")
        self.update_seekbar_color(UIConfig.SEEKBAR_KNOB_STANDBY)

    def on_auto_stop(self) -> None:
        """動画終了時の自動停止処理
        
        注意: ローカルファイル切り替え時に旧ストリーマーのstop_cbが呼ばれた場合、
        この関数は実行されない（stop_cbがNoneに設定されるため）
        """
        # ストリーマーが存在しない場合は何もしない（切り替え中の誤動作防止）
        if self.streamer is None:
            self.log("動画の再生が完了しました。（ストリーマーなし）")
            if self.main_thread_monitor_active:
                self._stop_main_thread_monitor()
            return
        
        self.log("動画の再生が完了しました。")
        self.on_stop()

    def on_close(self) -> None:
        """ウィンドウを閉じる時の処理"""
        try:
            self.on_stop()
            # 共有Sender解放
            if self.spout_sender:
                try:
                    self.spout_sender.releaseSender()
                except Exception:
                    pass
        finally:
            self.root.destroy()

    def _sync_streamer_preview_enabled(self) -> None:
        """ウィンドウの最小化状態をstreamer.preview_enabledへ反映する

        最小化中はプレビュー映像がユーザーに見えないため、streamerの
        preview_enabledをFalseにして、GUI表示用のフレーム取得・変換
        （get_current_frame/BGRA->BGR変換/リサイズ）を止める。これにより
        C++側の省電力リードバック省略機構が働く余地が生まれる。
        復帰（最小化解除）時はTrueに戻し、従来通りプレビューを更新する。

        最小化以外の「実際に表示されていない」状態（ウィジェット非表示等）は
        判別が不確実なため、ここでは連動させない。
        """
        if not self.streamer or not hasattr(self.streamer, 'preview_enabled'):
            return
        try:
            is_minimized = self.root.state() == 'iconic'
        except Exception:
            # 状態取得に失敗した場合は安全側（プレビュー有効のまま）に倒す
            return
        try:
            self.streamer.preview_enabled = not is_minimized
        except Exception:
            pass

    def update_preview(self) -> None:
        """プレビュー画面を更新する"""
        if self.preview_update_disabled:
            self.root.after(UIConfig.PREVIEW_UPDATE_INTERVAL, self.update_preview)
            return

        update_start_time: float | None = None
        try:
            update_start_time = time.time()
            self._sync_streamer_preview_enabled()

            if self.streamer and self.streamer.latest_frame_bgr is not None:
                # フレーム取得を高速化
                with self.streamer.frame_lock:
                    frame = self.streamer.latest_frame_bgr.copy()
                
                # 重い処理を分割して実行
                self._update_preview_frame_async(frame, update_start_time)
            else:
                if not hasattr(self, '_no_signal_shown') or not self._no_signal_shown:
                    self.preview_label.configure(image="", text="No Signal", text_color="white")
                    self._no_signal_shown = True
                    
            # ストリーマー情報更新（軽量処理のみ）
            if self.streamer:
                # NativeStreamerWrapper（C++ DLLバックエンド）の進捗情報を取得・表示
                self._update_native_progress()
                
                if self.streamer.is_vod:
                    if not self._seeking:
                        self.seek_slider.set(self.streamer.playback_time)
                        current_t = self.format_time(self.streamer.playback_time)
                        total_t = self.format_time(self.duration_cache)
                        self.time_label.configure(text=f"{current_t} / {total_t}")

                    self.info_label.configure(
                        text=f"Resolution: {self.streamer.width}x{self.streamer.height} @ {self.streamer.detected_fps}fps")
                else:  # ライブの場合
                    self.info_label.configure(
                        text=f"(LIVE) Resolution: {self.streamer.width}x{self.streamer.height} @ {self.streamer.detected_fps}fps")
            elif not self.download_in_progress:
                self.info_label.configure(text="No stream active")
                
            # 軽量処理時間を測定（ログ出力は削除）
                    
        except Exception as e:
            if update_start_time:
                update_duration = time.time() - update_start_time
                self.debug_log(f"プレビュー更新エラー: {e} (時間: {update_duration:.3f}秒)")
        finally:
            self.root.after(UIConfig.PREVIEW_UPDATE_INTERVAL, self.update_preview)
    
    def _update_native_progress(self) -> None:
        """NativeStreamerWrapper（C++ DLLバックエンド）の進捗情報を更新・表示"""
        if not self.streamer:
            return
        
        # NativeStreamerWrapperのみ処理（download_progressプロパティを持つか確認）
        if not hasattr(self.streamer, 'download_progress') or not hasattr(self.streamer, 'is_fully_cached'):
            return
        
        try:
            progress = self.streamer.download_progress  # 0.0〜1.0
            is_cached = self.streamer.is_fully_cached
            bandwidth = getattr(self.streamer, 'bandwidth', 0.0)  # bytes/sec
            
            # 進捗値を0.0〜1.0にクリップ（推定サイズ誤差で102%等になることを防止）
            progress = max(0.0, min(1.0, progress))
            
            # キャッシュ完了時は進捗バーを非表示
            if is_cached:
                if hasattr(self, '_native_progress_visible') and self._native_progress_visible:
                    self.hide_download_progress()
                    self._native_progress_visible = False
                return
            
            # 進捗が0より大きい場合のみ表示
            if progress > 0.0:
                # 進捗バーを表示
                if not hasattr(self, '_native_progress_visible') or not self._native_progress_visible:
                    self.show_native_progress()
                    self._native_progress_visible = True
                
                # 進捗バーを更新
                self.progress_bar.set(progress)
                
                # 帯域幅をフォーマット
                if bandwidth > 0:
                    if bandwidth >= 1024 * 1024:
                        speed_str = f"{bandwidth / (1024 * 1024):.1f} MB/s"
                    elif bandwidth >= 1024:
                        speed_str = f"{bandwidth / 1024:.1f} KB/s"
                    else:
                        speed_str = f"{bandwidth:.0f} B/s"
                    self.progress_info.configure(
                        text=f"キャッシュ進捗: {progress * 100:.1f}% | 速度: {speed_str}"
                    )
                else:
                    self.progress_info.configure(text=f"キャッシュ進捗: {progress * 100:.1f}%")
        except Exception:
            pass  # 進捗取得エラーは無視
    
    def show_native_progress(self) -> None:
        """C++ DLLバックエンドの進捗バーを表示"""
        try:
            # 進捗フレームの高さを設定して表示
            self.progress_frame.configure(height=70)
            self.progress_frame.pack_propagate(False)
        except Exception:
            pass
    
    def _start_preview_worker(self) -> None:
        """プレビューフレーム処理用の常駐ワーカースレッドを起動

        50ms毎の更新のたびに使い捨てスレッドを生成しないよう、
        単一の常駐スレッドがキューを待ち受けて処理する。
        キューには最新フレーム1件のみを保持し、古いフレームは破棄する。
        ストリーム停止中はキューが空のままアイドル待機する。
        """
        def worker() -> None:
            while True:
                item = self._preview_frame_queue.get()
                frame, start_time = item
                try:
                    # 画像処理をワーカースレッドで実行
                    rgb = cv2.cvtColor(frame, cv2.COLOR_BGR2RGB)
                    self.root.after(0, lambda: self._finalize_preview_update(rgb, start_time))
                except Exception as e:
                    self.root.after(0, self.debug_log, f"フレーム処理エラー: {e}")

        self._preview_worker_thread = threading.Thread(target=worker, daemon=True)
        self._preview_worker_thread.start()

    def _update_preview_frame_async(self, frame: Any, start_time: float) -> None:
        """プレビューフレームを常駐ワーカースレッドに渡す（最新フレームのみ保持）"""
        # 古いフレームが未処理のまま残っていれば破棄し、最新フレームに差し替える
        try:
            self._preview_frame_queue.get_nowait()
        except queue.Empty:
            pass
        try:
            self._preview_frame_queue.put_nowait((frame, start_time))
        except queue.Full:
            pass
    
    def _finalize_preview_update(self, rgb: Any, start_time: float) -> None:
        """プレビュー更新の最終処理（メインスレッド）"""
        try:
            self.preview_label.update_idletasks()
            label_width = self.preview_label.winfo_width()
            label_height = self.preview_label.winfo_height()
            
            if label_width < 100:
                label_width = 640
            if label_height < 100:
                label_height = 360
            
            h, w, _ = rgb.shape
            scale = min(label_width / float(w), label_height / float(h))
            new_w, new_h = int(w * scale), int(h * scale)
            
            if new_w > 0 and new_h > 0:
                dst = cv2.resize(rgb, (new_w, new_h), interpolation=cv2.INTER_LINEAR)
                img = Image.fromarray(dst)
                self.preview_imgtk = ctk.CTkImage(light_image=img, dark_image=img, size=(new_w, new_h))
                self.preview_label.configure(image=self.preview_imgtk, text="")
                self._no_signal_shown = False
                
            # 処理時間測定（ログ出力は削除）

        except Exception as e:
            self.debug_log(f"プレビュー最終処理エラー: {e}")


# =============================================================================
# CLI（ヘッドレス）モード対応
# =============================================================================


def parse_cli_args():
    """コマンドライン引数を解析（GUI/CLIモード両対応）"""
    import argparse
    
    parser = argparse.ArgumentParser(
        description="YouTube動画をSpout経由でリアルタイム配信するツール（GUI/CLI両対応）",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
使用例:
  %(prog)s                                           # GUIモードで起動
  %(prog)s --headless "https://youtu.be/xxx"         # ヘッドレス（CLI）モード
  %(prog)s --headless -s "MySender" "https://youtu.be/xxx"
  %(prog)s --headless --max-width 1920 --max-height 1080 --loop "https://youtu.be/xxx"
  %(prog)s --headless --check-codecs                 # コーデック対応確認
        """
    )
    
    # モード選択
    parser.add_argument("--headless", action="store_true",
                       help="ヘッドレス（CLI）モードで起動（GUIなし）")
    
    # URL（位置引数、オプション）
    parser.add_argument("url", nargs="?", default=DEFAULT_VIDEO_URL,
                       help=f"YouTube URL (デフォルト: {DEFAULT_VIDEO_URL})")
    
    # Spout設定
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


class HeadlessApp:
    """ヘッドレス（CLI）モード用のアプリケーションクラス
    
    GUIを起動せず、コンソールでStreamerを実行する。
    main.pyの機能を完全に内包。
    """
    
    def __init__(self, args):
        self.args = args
        self.streamer = None
        self.stop_event = threading.Event()
        
    def log(self, msg: str) -> None:
        """ログ出力"""
        if self.args.verbose:
            print(f"[{time.strftime('%H:%M:%S')}] {msg}")
        else:
            print(msg)

    @staticmethod
    def _is_streamer_alive(streamer) -> bool:
        """Streamer/NativeStreamerWrapperの実行中判定（Issue GUI-5）

        存在しない`is_running`属性への依存をやめ、実装によって異なる
        実在の状態を参照する:
        - NativeStreamerWrapper: self._thread / self._stop_event
        - レガシーStreamer:      self.thread  / self.stop_event

        判定不能な場合は安全側として「実行中」とみなし、
        stop_event（Ctrl+CやstopCb経由）による終了待機に委ねる。
        """
        if streamer is None:
            return False

        thread = getattr(streamer, '_thread', None)
        if thread is None:
            thread = getattr(streamer, 'thread', None)
        if thread is not None:
            return thread.is_alive()

        stop_event = getattr(streamer, '_stop_event', None)
        if stop_event is None:
            stop_event = getattr(streamer, 'stop_event', None)
        if stop_event is not None:
            return not stop_event.is_set()

        return True

    def run(self) -> int:
        """メイン実行ループ"""
        args = self.args
        
        # コーデック確認モード
        if args.check_codecs:
            return self._check_codecs()
        
        # 解像度設定の処理
        max_resolution = self._get_max_resolution()
        manual_resolution = self._get_manual_resolution()
        
        # Streamerを作成
        self.streamer = self._create_streamer(
            video_url=args.url,
            sender_name=args.sender,
            max_resolution=max_resolution,
            manual_resolution=manual_resolution,
            loop_vod=args.loop
        )
        
        # Ctrl+Cハンドラ設定
        import signal
        def signal_handler(sig, frame):
            self.log("\n停止シグナルを受信しました...")
            self.stop_event.set()
            if self.streamer:
                self.streamer.stop()
        
        signal.signal(signal.SIGINT, signal_handler)
        signal.signal(signal.SIGTERM, signal_handler)
        
        # ストリーミング開始
        try:
            self.log(f"ストリーミング開始: {args.url}")
            self.log(f"Spout送信者名: {args.sender}")
            self.streamer.start()
            
            # ストリーマーの終了を待機
            while not self.stop_event.is_set():
                if not self._is_streamer_alive(self.streamer):
                    break
                time.sleep(0.1)
            
            return 0
        except Exception as e:
            self.log(f"エラー: {e}")
            return 1
        finally:
            if self.streamer:
                self.streamer.stop()
    
    def _check_codecs(self) -> int:
        """コーデック対応確認"""
        from ytdlpSpout.core import check_av1_support, find_ffmpeg_path
        
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
    
    def _get_max_resolution(self):
        """最大解像度設定を取得"""
        args = self.args
        if args.max_width and args.max_height:
            return (args.max_width, args.max_height)
        if args.max_width or args.max_height:
            # Issue GUI-4: 片方だけ指定された場合に無警告で無視しない
            self.log("警告: --max-widthと--max-heightは両方指定してください。解像度上限の指定を無視します。")
        if not args.no_limit:
            # デフォルトで1080p制限を設定（安定性重視）
            if args.verbose:
                self.log("デフォルト解像度制限: 1080p (--no-limitで無効化可能)")
            return (1920, 1080)
        return None

    def _get_manual_resolution(self):
        """手動解像度設定を取得"""
        args = self.args
        if args.width and args.height:
            return (args.width, args.height)
        if args.width or args.height:
            # Issue GUI-4: 片方だけ指定された場合に無警告で無視しない
            self.log("警告: --widthと--heightは両方指定してください。手動解像度の指定を無視します。")
        return None
    
    def _create_streamer(self, video_url, sender_name, max_resolution, manual_resolution, loop_vod):
        """Streamerを作成"""
        # C++ DLLバックエンドが利用可能な場合
        if NATIVE_BACKEND_AVAILABLE:
            self.log("[Backend] C++ DLL")
            return NativeStreamerWrapper(
                video_url=video_url,
                sender_name=sender_name,
                max_resolution=max_resolution,
                manual_resolution=manual_resolution,
                loop_vod=loop_vod,
                verbose=self.args.verbose,
                log_cb=self.log,
                stop_cb=lambda: self.stop_event.set(),
                init_ok_cb=lambda: self.log("ストリーミング初期化完了"),
                external_spout_sender=None
            )
        else:
            # Python Streamerにフォールバック
            self.log("[Backend] Python Streamer (fallback)")
            return Streamer(
                video_url,
                sender_name,
                max_resolution=max_resolution,
                manual_resolution=manual_resolution,
                loop_vod=loop_vod,
                verbose=self.args.verbose,
                log_cb=self.log,
                stop_cb=lambda: self.stop_event.set(),
                init_ok_cb=lambda: self.log("ストリーミング初期化完了"),
                external_spout_sender=None
            )


def run_headless(args) -> int:
    """ヘッドレスモードでアプリケーションを実行"""
    app = HeadlessApp(args)
    return app.run()


if __name__ == "__main__":
    args = parse_cli_args()
    
    if args.headless or args.check_codecs:
        # ヘッドレス（CLI）モード
        sys.exit(run_headless(args))
    else:
        # GUIモード
        root = ctk.CTk()
        app = App(root)
        root.mainloop()
