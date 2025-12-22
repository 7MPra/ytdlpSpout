"""ytdlpSpout GUI アプリケーション"""
from __future__ import annotations

import os
import queue
import ssl
import subprocess
import sys
import threading
import time
from typing import TYPE_CHECKING, Any

import cv2
import customtkinter as ctk
import tkinter as tk
import yt_dlp
import SpoutGL
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
        
        # 切り替え処理管理用
        self._switching_in_progress = False  # 切り替え処理中フラグ
        self._switching_cancelled = False  # 切り替えキャンセルフラグ
        self._switching_thread: threading.Thread | None = None  # 切り替えスレッド
        self._new_streamer: Streamer | None = None  # 切り替え中の新Streamer
        
        # プログレスデータ初期化
        self.download_progress: dict[str, Any] = self._create_empty_progress()
        
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
        """デバッグ専用ログ（常に出力）"""
        try:
            timestamp = time.strftime("%H:%M:%S") + f".{int(time.time() * 1000) % 1000:03d}"
            debug_msg = f"[{timestamp}] {msg}"
            self._log_direct(debug_msg)
        except Exception:
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
    
    def _cancel_switching(self) -> None:
        """切り替え処理をキャンセルする"""
        if not self._switching_in_progress:
            return
        
        self.log("切り替え処理をキャンセル中...")
        self._switching_cancelled = True
        
        # 新しいStreamerが作成されていれば停止
        if self._new_streamer is not None:
            try:
                self._new_streamer.stop()
                self.log("新しいStreamerを停止しました")
            except Exception as e:
                self.log(f"新Streamer停止エラー: {e}")
            finally:
                self._new_streamer = None
        
        # 切り替えスレッドの完了を待機（最大1秒）
        if self._switching_thread and self._switching_thread.is_alive():
            self._switching_thread.join(timeout=1.0)
            if self._switching_thread.is_alive():
                self.log("警告: 切り替えスレッドがタイムアウトしました")
        
        self._switching_in_progress = False
        self._switching_thread = None

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
                
                # フォーマット文字列を取得
                format_str, codec_info = get_optimal_format_string()
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
                
                # cookiesファイルが存在すれば使用
                cookie_file = os.path.join("data", "cookies.txt")
                if os.path.exists(cookie_file):
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
        """ダウンロード完了後にローカルファイルへシームレスに切り替え（フレーム同期版）"""
        try:
            # 切り替え中フラグをセット
            self._switching_in_progress = True
            self._switching_cancelled = False
            
            # シークバーを無効化（切り替え中の操作を防ぐ）
            self.root.after(0, lambda: self.seek_slider.configure(state="disabled"))
            
            self.log(f"シームレス切り替え開始: {file_path}")
            
            if not os.path.exists(file_path):
                self.log(f"エラー: ローカルファイルが見つかりません: {file_path}")
                return
            
            # ファイルの絶対パスを取得（ログ省略）
            abs_file_path = os.path.abspath(file_path)
            # self.log(f"絶対パス: {abs_file_path}")
            
            # 旧ストリーマーの参照を保持
            old_streamer = self.streamer
            if not old_streamer:
                self.log("エラー: 現在アクティブなストリーマーがありません")
                return
            
            # 現在のPTSを取得
            with old_streamer.pts_lock:
                current_pts = old_streamer.current_frame_pts
            
            # 詳細ログ省略
            # self.log(f"[SYNC] 旧ストリーマーPTS取得: {current_pts:.3f}秒")
            
            # ローカルファイルパスを設定
            self.local_video_path = abs_file_path
            
            # 同期目標PTSを計算
            buffer_frames = 15
            fps = old_streamer.detected_fps if old_streamer.detected_fps > 0 else 30
            buffer_time = buffer_frames / fps
            sync_target_pts = current_pts + buffer_time
            
            # self.log(f"[SYNC] 同期目標PTS: {sync_target_pts:.3f}秒 ({buffer_frames}フレームバッファ, FPS={fps})")
            
            # フレーム同期による切り替え処理
            def prepare_and_switch():
                try:
                    # キャンセルチェック
                    if self._switching_cancelled:
                        self.log("切り替え処理がキャンセルされました")
                        return
                    
                    # === フェーズ1: 新ストリーマーを先に準備 ===
                    self.log("新しいローカルストリーマーを準備中...")
                    
                    # 旧ストリーマーの解像度を引き継ぐ（ログ省略）
                    old_resolution = None
                    if old_streamer and hasattr(old_streamer, 'width') and hasattr(old_streamer, 'height'):
                        old_resolution = (old_streamer.width, old_streamer.height)
                        # self.log(f"[SYNC] 旧ストリーマーの解像度を引き継ぎ: {old_resolution[0]}x{old_resolution[1]}")
                    
                    max_res, manual_res = self._get_resolution_settings()
                    
                    # 旧ストリーマーの解像度を優先
                    if old_resolution:
                        manual_res = old_resolution
                    
                    from ytdlpSpout.core import Streamer
                    new_streamer = Streamer(
                        abs_file_path,
                        self.sender_var.get(),
                        max_resolution=max_res,
                        manual_resolution=manual_res,
                        loop_vod=self.vod_loop.get(),
                        log_cb=lambda m: self.root.after(0, self.log, m),
                        stop_cb=None,
                        init_ok_cb=None,
                        external_spout_sender=self.get_shared_spout_sender(self.sender_var.get())
                    )
                    
                    # 新Streamerを保持（キャンセル用）
                    self._new_streamer = new_streamer
                    
                    # キャンセルチェック
                    if self._switching_cancelled:
                        self.log("切り替え処理がキャンセルされました（新Streamer作成後）")
                        new_streamer.stop()
                        return
                    
                    self.log("新しいストリーマーを開始中...")
                    new_streamer.start()
                    
                    # Spout初期化待ち
                    init_wait_start = time.time()
                    while (time.time() - init_wait_start) < 3.0:
                        if (hasattr(new_streamer, 'spout') and new_streamer.spout is not None and
                            hasattr(new_streamer, 'detected_fps') and new_streamer.detected_fps > 0):
                            break
                        time.sleep(0.05)
                    
                    # 動画長チェック
                    old_dur = getattr(old_streamer, 'duration', 0.0)
                    new_dur = getattr(new_streamer, 'duration', 0.0)
                    if old_dur > 0 and new_dur > 0:
                        dur_diff = abs(old_dur - new_dur)
                        self.log(f"[SYNC] 動画長比較: 旧={old_dur:.2f}s, 新={new_dur:.2f}s, 差={dur_diff:.2f}s")
                        if dur_diff > 1.0:
                            self.log(f"[SYNC] 警告: 動画の長さが {dur_diff:.2f}秒 異なります。同期位置が不正確になる可能性があります。")
                    
                    # === フェーズ2: 新ストリーマーのSpout送信を無効化 ===
                    # シーク完了まで、新ストリーマーからのフレーム送信を抑制
                    new_streamer.set_spout_enabled(False)
                    
                    # === フェーズ3: 画像マッチングによる同期ズレ補正 ===
                    sync_offset = 0.0
                    try:
                        # 旧ストリーマーの現在の画像とPTSを取得
                        target_img = None
                        base_pts = 0.0
                        with old_streamer.frame_lock:
                            if old_streamer.latest_frame_bgr is not None:
                                target_img = old_streamer.latest_frame_bgr.copy()
                        with old_streamer.pts_lock:
                            base_pts = old_streamer.current_frame_pts
                            
                        if target_img is not None:
                             self.log(f"[SYNC] 画像マッチング開始: 基準PTS={base_pts:.3f}秒")
                             # 探索範囲4秒でベストマッチを探す
                             best_pts, score, _ = new_streamer.find_best_match_pts(target_img, base_pts, search_range=4.0)
                             
                             # スコア（画素値の平均絶対差）が小さいほど似ている
                             # 圧縮ノイズ等を考慮して閾値を緩和 (30.0 -> 60.0)
                             if score < 60.0: 
                                 sync_offset = best_pts - base_pts
                                 self.log(f"[SYNC] マッチング成功: 検出PTS={best_pts:.3f}秒 (スコア={score:.1f}), オフセット={sync_offset:+.3f}秒")
                             else:
                                 # マッチング失敗時も参考情報を出す
                                 temp_offset = best_pts - base_pts
                                 self.log(f"[SYNC] マッチング信頼度低 (スコア={score:.1f} > 60.0) - オフセット補正なし (参考オフセット={temp_offset:+.3f}秒)")
                    except Exception as e:
                        self.log(f"[SYNC] 画像マッチング失敗: {e}")

                    # === フェーズ4: 未来の目標PTSを設定（待ち伏せ戦略） ===
                    with old_streamer.pts_lock:
                        current_old_pts = old_streamer.current_frame_pts
                    
                    # 旧ストリーマー基準の目標切り替え時間（現在 + 5秒）
                    switch_trigger_pts = current_old_pts + 5.0
                    
                    # 新ストリーマーがシークすべき時間（オフセット適用）
                    seek_target_pts = switch_trigger_pts + sync_offset
                    
                    self.log(f"[SYNC] 目標: 旧到達={switch_trigger_pts:.3f}秒, 新シーク={seek_target_pts:.3f}秒 (オフセット={sync_offset:+.3f}秒)")
                    
                    # === フェーズ5: 新ストリーマーを目標PTSにシーク＆一時停止予約 ===
                    # 指定PTSで自動的に一時停止するように設定
                    new_streamer.pause_at_pts(seek_target_pts)
                    
                    # new_streamer.seek(seek_target_pts)
                    new_streamer.seek(seek_target_pts)
                    
                    # === フェーズ6: 新ストリーマーが目標PTSに到達（一時停止）するまで待機 ===
                    # self.log("[SYNC] 新ストリーマーの準備（シーク＆プリロード）を待機中...")
                    
                    # タイムアウト10秒で待機
                    if new_streamer.wait_for_pts(seek_target_pts, timeout=10.0):
                        # self.log("[SYNC] 新ストリーマー準備完了（一時停止中）")
                        pass
                    else:
                        self.log("[SYNC] 警告: 新ストリーマーの準備がタイムアウトしました")
                    
                    # 一時停止が実際に完了するまで待機（デコードループが停止していることを確認）
                    pause_wait_start = time.time()
                    while (time.time() - pause_wait_start) < 2.0:
                        with new_streamer.pause_lock:
                            if new_streamer.is_paused:
                                break
                        time.sleep(0.005)
                    
                    # 一時停止完了後のPTSを取得（これが実際の切り替え位置）
                    with new_streamer.pts_lock:
                        actual_new_pts = new_streamer.current_frame_pts
                    
                    # 新ストリーマーの実際の停止位置から、オフセットを逆算して旧ストリーマーのトリガー位置を再調整
                    # seek_target_pts (目標) -> actual_new_pts (実際) のズレもここで吸収
                    # 旧トリガー = 新実際PTS - オフセット
                    switch_trigger_pts = actual_new_pts - sync_offset
                    self.log(f"[SYNC] 最終調整: 旧トリガー={switch_trigger_pts:.3f}秒 (新実PTS={actual_new_pts:.3f} - オフセット)")
                    
                    # === フェーズ7: 旧ストリーマーがトリガーPTSに到達するのを監視 ===
                    wait_start = time.time()
                    switch_pts = switch_trigger_pts  # 実際の切り替えPTS
                    while (time.time() - wait_start) < 10.0 and not self._switching_cancelled:
                        with old_streamer.pts_lock:
                            current_old = old_streamer.current_frame_pts
                        
                        # 目標PTSに到達（または通過）したら即切り替え
                        if current_old >= switch_trigger_pts:
                            switch_pts = current_old  # 実際の切り替えPTSを記録
                            self.log(f"[SYNC] 到達確認: 旧PTS={current_old:.3f}秒 (目標={switch_trigger_pts:.3f}秒)")
                            break
                        
                        time.sleep(0.005)  # より高頻度でチェック（5ms間隔）
                    
                    # キャンセルチェック
                    if self._switching_cancelled:
                        self.log("切り替え処理が直前でキャンセルされました")
                        new_streamer.stop()
                        return

                    # === フェーズ7: 切り替え実行 ===
                    # 順序重要: 旧ストリーマーを先に停止してから新ストリーマーを再開
                    # これにより、新ストリーマーがフレームを進める前に旧を止められる
                    
                    # 1. 旧ストリーマーを即座に停止（フレーム送信を止める）
                    old_streamer.stop()
                    
                    # 2. 新ストリーマーのSpout送信を有効化
                    new_streamer.set_spout_enabled(True)
                    
                    # 3. 新ストリーマーの一時停止を解除
                    new_streamer.resume()
                    
                    self.log("[SYNC] 切り替え実行完了")
                    
                    # 精度確認用の記録
                    with new_streamer.pts_lock:
                        final_new_pts = new_streamer.current_frame_pts
                    
                    self.log(f"[SYNC] 最終状態: トリガー={switch_trigger_pts:.3f}秒, 旧最終={switch_pts:.3f}秒, 新開始={final_new_pts:.3f}秒")
                    
                    # === フェーズ9: 完了処理 ===
                    # 最終キャンセルチェック
                    if self._switching_cancelled:
                        self.log("切り替え処理がキャンセルされました（完了直前）")
                        new_streamer.stop()
                        return
                    
                    # GUIのストリーマー参照を更新
                    self.streamer = new_streamer
                    self._new_streamer = None  # 参照をクリア
                    
                    # 同期精度計算（参考）
                    # 補正後理想 = switch_pts + sync_offset
                    # 実際 = final_new_pts
                    expected_new_pts = switch_pts + sync_offset
                    pts_diff = abs(expected_new_pts - final_new_pts)
                    frame_diff = pts_diff * fps
                    
                    self.log(f"[SYNC] 同期切替完了 (推定精度: {frame_diff:.2f}フレーム, オフセット適用済)")
                    
                    # ステータス更新
                    original_url_display = self.original_url if self.original_url else "不明"
                    self.root.after(0, lambda: self.info_label.configure(
                        text=f"ローカルファイル再生中（{original_url_display} からダウンロード済み）"
                    ))
                    self.root.after(0, lambda: self.update_seekbar_color(UIConfig.SEEKBAR_KNOB_LOCAL))
                    
                    self.log(f"[SYNC] シームレス切り替え完了: 同期精度={frame_diff:.2f}フレーム")
                    
                    # 進捗バーを非表示
                    self.root.after(0, lambda: self.progress_bar.stop())  # アニメーション停止
                    self.root.after(0, lambda: self.progress_bar.configure(mode="determinate"))  # 通常モードに戻す
                    self.root.after(0, self.hide_download_progress)
                    self.root.after(500, self.hide_download_progress)
                    
                    # 切り替え完了フラグをリセット
                    self._switching_in_progress = False
                    
                    # シークバーを再有効化
                    self.root.after(0, lambda: self.seek_slider.configure(state="normal"))
                        
                except Exception as e:
                    self.log(f"シームレス切り替え準備エラー: {e}")
                    import traceback
                    self.log(traceback.format_exc())
                    # エラー時もアニメーションを停止
                    self.root.after(0, lambda: self.progress_bar.stop())
                    self.root.after(0, lambda: self.progress_bar.configure(mode="determinate"))
                    if 'new_streamer' in locals():
                        new_streamer.stop()
                    self._new_streamer = None
                    self._switching_in_progress = False
                    # エラー時もシークバーを再有効化
                    self.root.after(0, lambda: self.seek_slider.configure(state="normal"))
            
            # バックグラウンドで並行準備
            self._switching_thread = threading.Thread(target=prepare_and_switch, daemon=True)
            self._switching_thread.start()
            
        except Exception as e:
            self.log(f"シームレス切り替えエラー: {e}")

    
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
        """共有SpoutSenderを取得または作成する"""
        if self.spout_sender is None:
            try:
                self.spout_sender = SpoutGL.SpoutSender()
                self.spout_sender.createOpenGL()
                self.spout_sender.setSenderName(sender_name)
                self.log(f"共有SpoutSenderを作成しました: {sender_name}")
            except Exception as e:
                self.log(f"SpoutSender作成エラー: {e}")
                return None
        else:
            try:
                # 名前を更新（既に同じなら変化なし、だが念のため呼ぶ）
                self.spout_sender.setSenderName(sender_name)
            except Exception as e:
                self.log(f"Sender名更新エラー: {e}")
        
        return self.spout_sender
    
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
        # 切り替え中はシークを無効化
        if self._switching_in_progress:
            self.log("切り替え中のためシークは無効です")
            return
        
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
        # 切り替え中はシークを無効化
        if self._switching_in_progress:
            self._seeking = False
            self.log("切り替え中のためシークは無効です")
            return
        
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
            pass
        
        # 手動解像度設定
        try:
            manw = self.manw_var.get().strip()
            manh = self.manh_var.get().strip()
            if self.manual_enable.get() and manw and manh:
                manual_res = (int(manw), int(manh))
        except (ValueError, TypeError):
            pass
        
        return max_res, manual_res
    
    def _start_local_file_stream(self, file_path: str) -> None:
        """ローカルファイルからのストリーミングを開始"""
        try:
            sender = self.sender_var.get().strip() or DEFAULT_SENDER_NAME
            
            self.streamer = Streamer(
                file_path, sender,
                max_resolution=None,
                manual_resolution=None,
                loop_vod=self.vod_loop.get(),
                verbose=True,
                log_cb=self._log_direct,
                stop_cb=self.on_auto_stop,
                init_ok_cb=self.on_stream_start_success,
                external_spout_sender=self.get_shared_spout_sender(sender)
            )
            self.streamer.start()
            self.update_seekbar_color(UIConfig.SEEKBAR_KNOB_LOCAL)
            
        except Exception as e:
            self.log(f"ローカルファイルストリーミング開始エラー: {e}")
            self.on_auto_stop()
    
    def _start_url_stream(self, video_source_url: str) -> None:
        """URLからのストリーミングを開始（従来の処理）"""
        sender = self.sender_var.get().strip() or DEFAULT_SENDER_NAME
        max_res, manual_res = self._get_resolution_settings()
        
        def start_streaming_thread() -> None:
            try:
                self.streamer = Streamer(
                    video_source_url,
                    sender,
                    max_resolution=max_res,
                    manual_resolution=manual_res,
                    loop_vod=self.vod_loop.get(),
                    log_cb=lambda m: self.root.after(0, self.log, m),
                    stop_cb=lambda: self.root.after(0, self.on_auto_stop),
                    init_ok_cb=lambda: self.root.after(0, self.on_stream_start_success),
                    external_spout_sender=self.get_shared_spout_sender(sender)
                )
                self.streamer.start()
                self.root.after(0, lambda: self.update_seekbar_color(UIConfig.SEEKBAR_KNOB_DOWNLOADING))
            except Exception as e:
                self.root.after(0, self._handle_start_error, f"ストリーミング開始エラー: {e}")
        
        threading.Thread(target=start_streaming_thread, daemon=True).start()

    def on_stream(self) -> None:
        """「Stream」ボタン：VODならストリーミング再生＋バックグラウンドダウンロード"""
        url = self.url_var.get().strip()
        if not url:
            self.log("エラー: ストリーミングするURLが入力されていません。")
            return

        # メインスレッド監視を開始（ブロッキング検出のため）
        self._start_main_thread_monitor()

        # 元のURLを保存（ローカルファイルでない場合のみ）
        if not os.path.exists(url):
            self.original_url = url

        # まずストリーミング再生
        self._start_spout_stream(url)

        # VOD判定とダウンロード開始を非同期で行う（プチフリを防ぐ）
        def check_and_start_download() -> None:
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
                            # 常にサブプロセスダウンロードを使用（プチフリーズを回避）
                            self.root.after(0, self.log, "VOD検出: 別プロセスでバックグラウンドダウンロードを開始します")
                            self.start_subprocess_download(url)
                        return
                except Exception:
                    # streamerのアクセスでエラーが発生した場合は継続
                    continue
            
            # タイムアウトした場合
            self.root.after(0, self.log, "VOD判定がタイムアウトしました。ライブストリームまたは判定不可。")
        
        # バックグラウンドスレッドで実行
        threading.Thread(target=check_and_start_download, daemon=True).start()

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
            # ダウンロード中のサブプロセスをキャンセル
            try:
                self._cancel_download()
            except Exception as e:
                self.log(f"ダウンロードキャンセル警告: {e}")
            
            # 切り替え処理をキャンセル
            try:
                self._cancel_switching()
            except Exception as e:
                self.log(f"切り替えキャンセル警告: {e}")
            
            # 現在のStreamerを停止
            if self.streamer:
                try:
                    self.streamer.stop()
                except Exception as e:
                    self.log(f"ストリーマー停止警告: {e}")
                finally:
                    self.streamer = None
            
            # 進捗バーを非表示にする
            self.hide_download_progress()
            
        finally:
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
        
        self.download_in_progress = False
        self.btn_start.configure(state="normal")
        self.btn_stop.configure(state="disabled")
        self.info_label.configure(text="エラーが発生しました")
        self.update_seekbar_color(UIConfig.SEEKBAR_KNOB_STANDBY)

    def on_auto_stop(self) -> None:
        """動画終了時の自動停止処理"""
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

    def update_preview(self) -> None:
        """プレビュー画面を更新する"""
        if self.preview_update_disabled:
            self.root.after(UIConfig.PREVIEW_UPDATE_INTERVAL, self.update_preview)
            return
            
        update_start_time: float | None = None
        try:
            update_start_time = time.time()
            
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
    
    def _update_preview_frame_async(self, frame: Any, start_time: float) -> None:
        """プレビューフレーム更新を非同期で処理"""
        def process_frame() -> None:
            try:
                # 画像処理を別スレッドで実行
                rgb = cv2.cvtColor(frame, cv2.COLOR_BGR2RGB)
                self.root.after(0, lambda: self._finalize_preview_update(rgb, start_time))
            except Exception as e:
                self.root.after(0, self.debug_log, f"フレーム処理エラー: {e}")
        
        threading.Thread(target=process_frame, daemon=True).start()
    
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


if __name__ == "__main__":
    root = ctk.CTk()
    app = App(root)
    root.mainloop()
