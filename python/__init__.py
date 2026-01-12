"""
python - ytdlpspout Python FFI バインディング

このパッケージはC++で実装されたytdlpspoutライブラリへの
Pythonバインディングを提供します。

使用例:
    from python import YtdlpSpoutNative
    
    player = YtdlpSpoutNative()
    player.start("video.mp4")
    
    while player.is_playing:
        player.process_frame()
    
    player.stop()
"""

from .ytdlpspout_native import (
    YtdlpSpoutNative,
    YtdlpSpoutConfig,
    YtdlpSpoutVideoInfo,
    YtdlpSpoutState,
    get_version,
    play_video,
)

__all__ = [
    "YtdlpSpoutNative",
    "YtdlpSpoutConfig",
    "YtdlpSpoutVideoInfo",
    "YtdlpSpoutState",
    "get_version",
    "play_video",
]

__version__ = "0.1.0"
