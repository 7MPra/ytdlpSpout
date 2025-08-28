#!/usr/bin/env python3
"""
ytdlpSpout CLI - YouTube to Spout Streamer Command Line Tool

YouTube動画をSpout経由でリアルタイム配信するCLIツール
"""

import argparse
import cv2
import yt_dlp
import syphon
import subprocess
import sys
import os
import numpy as np
import time
import threading
import ssl
import certifi
from typing import Optional, Tuple

from ytdlpSpout.core import (
    DEFAULT_VIDEO_URL,
    DEFAULT_SENDER_NAME,
    find_ffmpeg_path,
    check_av1_support,
    get_optimal_format_string,
    Streamer
)

# SSL証明書の設定（Windows環境での証明書問題を回避）
try:
    import ssl
    ssl._create_default_https_context = ssl._create_unverified_context
except Exception:
    pass


class CLIStreamer(Streamer):
    """CLI版のStreamerクラス"""
    
    def __init__(self, video_url: str, sender_name: str, max_resolution: Optional[Tuple[int, int]] = None, 
                 manual_resolution: Optional[Tuple[int, int]] = None, loop_vod: bool = False, verbose: bool = False):
        super().__init__(video_url, sender_name, max_resolution, manual_resolution, loop_vod, verbose)
        self.console_log = True

    def log(self, msg: str):
        """ログ出力"""
        if self.verbose:
            print(f"[{time.strftime('%H:%M:%S')}] {msg}")
        else:
            print(msg)

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
