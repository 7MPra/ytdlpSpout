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
        self.seek_value = 0.0
        self.duration_cache = 0.0 # 動画の長さをキャッシュ
        
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

        # --- シークバー関連 --- #
        seek_frame = ctk.CTkFrame(preview_frame, fg_color="transparent")
        seek_frame.pack(fill="x", padx=10, pady=5, side="bottom")

        self.seek_slider = ctk.CTkSlider(seek_frame, from_=0, to=100, state="disabled", command=self.on_seek_drag)
        self.seek_slider.pack(fill="x", expand=True, side="left", padx=(0, 10))
        self.seek_slider.bind("<ButtonPress-1>", self.on_seek_press)
        self.seek_slider.bind("<ButtonRelease-1>", self.on_seek_release)

        self.time_label = ctk.CTkLabel(seek_frame, text="--:-- / --:--", font=ctk.CTkFont(family=self.monospace_font, size=12))
        self.time_label.pack(side="right")
        # -------------------- #
        
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

    def log(self, msg: str):
        try:
            self.log_text.insert("end", msg + "\n")
            self.log_text.see("end")
        except Exception:
            pass
    
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
                    init_ok_cb=lambda: self.root.after(0, self.on_stream_start_success)
                )
                self.streamer.start()
            except Exception as e:
                # エラー時のUI更新
                self.root.after(0, lambda: self._handle_start_error(str(e)))
        
        # 別スレッドで実行
        threading.Thread(target=start_streaming, daemon=True).start()

    def on_stream_start_success(self):
        self.info_label.configure(text="ストリーミング開始")
        if self.streamer and self.streamer.is_vod:
            self.duration_cache = self.streamer.duration
            self.log(f"動画の長さを取得しました: {self.duration_cache} 秒")
            self.seek_slider.configure(state="normal", to=self.duration_cache)
            self.time_label.configure(text=f"00:00 / {self.format_time(self.duration_cache)}")

    def on_stop(self):
        if not self.streamer:
            return
        self.streamer.stop()
        self.streamer = None
        self.btn_start.configure(state="normal")
        self.btn_stop.configure(state="disabled")
        self.info_label.configure(text="Stopped")
        self.seek_slider.configure(state="disabled")
        self.time_label.configure(text="--:-- / --:--")

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
        self.seek_slider.configure(state="disabled")
        self.time_label.configure(text="--:-- / --:--")

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
                    
            # 解像度情報と再生時間の更新
            if self.streamer:
                if self.streamer.is_vod:
                    # シーク中でなければ、再生時間に合わせてUIを更新
                    if not self._seeking:
                        self.seek_slider.set(self.streamer.playback_time)
                        current_t = self.format_time(self.streamer.playback_time)
                        total_t = self.format_time(self.duration_cache)
                        self.time_label.configure(text=f"{current_t} / {total_t}")

                    # 解像度情報は常に更新
                    self.info_label.configure(
                        text=f"Resolution: {self.streamer.width}x{self.streamer.height} @ {self.streamer.detected_fps}fps")
                else: # ライブの場合
                    self.info_label.configure(
                        text=f"(LIVE) Resolution: {self.streamer.width}x{self.streamer.height} @ {self.streamer.detected_fps}fps")
            else:
                self.info_label.configure(text="No stream active")
        finally:
            self.root.after(33, self.update_preview)


if __name__ == "__main__":
    root = ctk.CTk()
    app = App(root)
    root.mainloop()
