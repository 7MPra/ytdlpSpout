"""ffmpeg/ffprobe 関連ユーティリティ"""

import os
import subprocess
import sys
from typing import Tuple


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
    return tool  # 見つからなければ名前だけ返す


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
            # 映像のみmp4最優先
            'bestvideo[height<=2160][height>=720][ext=mp4][acodec=none]/'
            'bestvideo[ext=mp4][acodec=none]/'
            # 映像のみ（他コンテナ）
            'bestvideo[height<=2160][height>=720][acodec=none]/'
            'bestvideo[acodec=none]/'
            # 映像+音声mp4
            'bestvideo[height<=2160][height>=720][ext=mp4]+bestaudio[ext=m4a]/'
            'bestvideo[ext=mp4]+bestaudio[ext=m4a]/'
            # 既存の映像+音声
            'bestvideo[height<=2160][height>=720]+bestaudio/'
            'bestvideo[vcodec!*=av01][height<=1440][height>=720]+bestaudio/'  # AV1が重い場合のフォールバック
            'best[height<=2160][height>=720]/'
            'bestvideo[height>=720]+bestaudio/'
            f'{fallback_formats}'
        )
        codec_info = f"AV1 supported (decoders: {', '.join(av1_decoders)}, up to 2160p)"
    else:
        format_str = (
            # 映像のみmp4最優先
            'bestvideo[vcodec!*=av01][height<=2160][height>=720][ext=mp4][acodec=none]/'
            'bestvideo[vcodec!*=av01][ext=mp4][acodec=none]/'
            # 映像のみ（他コンテナ）
            'bestvideo[vcodec!*=av01][height<=2160][height>=720][acodec=none]/'
            'bestvideo[vcodec!*=av01][acodec=none]/'
            # 映像+音声mp4
            'bestvideo[vcodec!*=av01][height<=2160][height>=720][ext=mp4]+bestaudio[ext=m4a]/'
            'bestvideo[vcodec!*=av01][ext=mp4]+bestaudio[ext=m4a]/'
            # 既存の映像+音声
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
