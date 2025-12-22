"""ytdlpSpout パッケージ"""

# 定数モジュールから
from .constants import (
    DEFAULT_FPS,
    DEFAULT_HEIGHT,
    DEFAULT_SENDER_NAME,
    DEFAULT_VIDEO_URL,
    DEFAULT_WIDTH,
    MAX_CONSECUTIVE_FAILURES,
    MAX_FPS,
    MIN_FPS,
)

# ffmpegモジュールから
from .ffmpeg import (
    build_ffmpeg_header_args,
    check_av1_support,
    find_ffmpeg_path,
    get_executable_dir,
    get_optimal_format_string,
)

# 動画情報モジュールから
from .video_info import detect_fps, detect_max_resolution

# コアモジュールから
from .core import Streamer

__all__ = [
    # 定数
    "DEFAULT_VIDEO_URL",
    "DEFAULT_SENDER_NAME",
    "DEFAULT_WIDTH",
    "DEFAULT_HEIGHT",
    "DEFAULT_FPS",
    "MIN_FPS",
    "MAX_FPS",
    "MAX_CONSECUTIVE_FAILURES",
    # ffmpegユーティリティ
    "get_executable_dir",
    "find_ffmpeg_path",
    "check_av1_support",
    "get_optimal_format_string",
    "build_ffmpeg_header_args",
    # 動画情報ユーティリティ
    "detect_fps",
    "detect_max_resolution",
    # Streamer
    "Streamer",
]
