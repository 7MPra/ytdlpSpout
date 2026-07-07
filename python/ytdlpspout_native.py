"""
ytdlpspout_native - C++ DLLへのPython FFIバインディング

このモジュールはC++で実装されたytdlpspoutライブラリへの
ctypesを使用したPythonバインディングを提供します。

使用例:
    from python.ytdlpspout_native import YtdlpSpoutNative
    
    player = YtdlpSpoutNative()
    player.start("video.mp4", sender_name="MySpout", loop=True)
    
    while player.is_playing:
        player.process_frame()
    
    player.stop()
"""

import ctypes
from ctypes import (
    c_void_p, c_char_p, c_int, c_double, c_float, c_int64, c_uint8, c_size_t,
    POINTER, Structure, CFUNCTYPE, byref
)
from pathlib import Path
from typing import Optional, Callable
import os
import logging


# =============================================================================
# C構造体定義
# =============================================================================

class YtdlpSpoutConfig(Structure):
    """プレイヤー設定構造体"""
    _fields_ = [
        ("inputFile", c_char_p),
        ("senderName", c_char_p),
        ("loop", c_int),
        ("useHardwareAccel", c_int),
        ("verbose", c_int),
    ]


class YtdlpSpoutVideoInfo(Structure):
    """動画情報構造体"""
    _fields_ = [
        ("width", c_int),
        ("height", c_int),
        ("fps", c_double),
        ("duration", c_double),
        ("totalFrames", c_int64),
    ]


# =============================================================================
# スライス読み込み用構造体（Phase 4）
# =============================================================================

class YtdlpSpoutSliceConfig(Structure):
    """スライス読み込み設定構造体"""
    _fields_ = [
        ("enabled", c_int),
        ("chunkSize", c_size_t),
        ("maxCacheMemory", c_size_t),
        ("maxConcurrentDownloads", c_int),
        ("prefetchChunksAhead", c_int),
        ("criticalChunksAhead", c_int),
        ("enableContinuousDownload", c_int),
        ("cachePath", c_char_p),
    ]


class YtdlpSpoutYtDlpConfig(Structure):
    """yt-dlp設定構造体"""
    _fields_ = [
        ("path", c_char_p),
        ("preferredHeight", c_int),
    ]


class YtdlpSpoutHttpHeader(Structure):
    """HTTPヘッダー構造体"""
    _fields_ = [
        ("key", c_char_p),
        ("value", c_char_p),
    ]


class YtdlpSpoutHlsCacheStats(Structure):
    """HLSキャッシュ統計構造体"""
    _fields_ = [
        ("cachedSegments", c_int),
        ("totalSegments", c_int),
        ("downloadProgress", c_double),
        ("bandwidth", c_double),
        ("isFullyCached", c_int),
        ("isHlsMode", c_int),
    ]


class YtdlpSpoutConfigEx(Structure):
    """拡張設定構造体"""
    _fields_ = [
        ("source", c_char_p),
        ("senderName", c_char_p),
        ("outputWidth", c_int),
        ("outputHeight", c_int),
        ("loop", c_int),
        ("useHardwareAccel", c_int),
        ("verbose", c_int),
        ("slice", YtdlpSpoutSliceConfig),
        ("ytdlp", YtdlpSpoutYtDlpConfig),
        ("httpHeaders", POINTER(YtdlpSpoutHttpHeader)),
        ("httpHeadersCount", c_int),
        ("isHlsHint", c_int),
    ]


# 再生状態列挙型
class YtdlpSpoutState:
    STOPPED = 0
    PLAYING = 1
    PAUSED = 2
    ERROR = 3


# =============================================================================
# コールバック型定義
# =============================================================================

# typedef void (*YtdlpSpoutProgressCallback)(double currentTime, double duration, void* userData);
ProgressCallbackType = CFUNCTYPE(None, c_double, c_double, c_void_p)

# typedef void (*YtdlpSpoutErrorCallback)(const char* message, void* userData);
ErrorCallbackType = CFUNCTYPE(None, c_char_p, c_void_p)

# typedef void (*YtdlpSpoutCompletionCallback)(void* userData);
CompletionCallbackType = CFUNCTYPE(None, c_void_p)


# =============================================================================
# メインクラス
# =============================================================================

class YtdlpSpoutNative:
    """
    C++ ytdlpspout DLLへのPythonバインディング
    
    属性:
        dll_path: DLLのパス
        is_playing: 再生中かどうか
        position: 現在の再生位置（秒）
        duration: 総再生時間（秒）
        bpm: ビートマップのBPM（ない場合は0）
    
    使用例:
        player = YtdlpSpoutNative()
        player.start("video.mp4")
        while player.is_playing:
            player.process_frame()
        player.stop()
    """
    
    def __init__(self, dll_path: Optional[str] = None):
        """
        YtdlpSpoutNativeを初期化
        
        Args:
            dll_path: DLLファイルへのパス。Noneの場合、デフォルトの場所を検索
        """
        self._dll_path = self._find_dll(dll_path)
        
        # DLLディレクトリをPATHに追加（依存DLLの解決のため）
        dll_dir = str(self._dll_path.parent)
        if dll_dir not in os.environ.get("PATH", ""):
            os.environ["PATH"] = dll_dir + os.pathsep + os.environ.get("PATH", "")
        
        # Windows: DLL検索パスを明示的に追加
        if hasattr(os, 'add_dll_directory'):
            try:
                os.add_dll_directory(dll_dir)
            except (OSError, AttributeError):
                pass
        
        # 依存DLLを明示的にプリロード（ロード順序の問題を回避）
        self._preloaded_dlls = []
        dependency_dlls = [
            "zlib1.dll",
            "avutil-59.dll",
            "swresample-5.dll",
            "avcodec-61.dll",
            "avformat-61.dll",
            "swscale-8.dll",
            "fmt.dll",
            "spdlog.dll",
            "libcurl.dll",
            "Spout.dll",  # Spout有効ビルド（C++側でSpout送信）
        ]
        for dep_dll in dependency_dlls:
            dep_path = self._dll_path.parent / dep_dll
            if dep_path.exists():
                try:
                    # LOAD_WITH_ALTERED_SEARCH_PATH フラグを使用
                    handle = ctypes.WinDLL(str(dep_path))
                    self._preloaded_dlls.append(handle)
                except OSError:
                    pass  # 既にロードされている場合は無視
        
        # メインDLLをロード
        self._lib = ctypes.WinDLL(str(self._dll_path))
        self._setup_functions()
        
        self._handle = self._lib.ytdlpspout_create()
        if not self._handle:
            raise RuntimeError(f"Failed to create player: {self.get_last_error()}")
        
        # コールバック参照を保持（GC対策）
        self._progress_callback_ref = None
        self._error_callback_ref = None
        self._completion_callback_ref = None
        
        # Pythonコールバック
        self._on_progress: Optional[Callable[[float, float], None]] = None
        self._on_error: Optional[Callable[[str], None]] = None
        self._on_completion: Optional[Callable[[], None]] = None

        # get_current_frame()用の使い回しバッファ（サイズ変化時のみ再確保）
        self._frame_buffer = None
        self._frame_buffer_size = 0
    
    def __del__(self):
        """デストラクタ: プレイヤーを破棄"""
        if hasattr(self, '_handle') and self._handle:
            self._lib.ytdlpspout_destroy(self._handle)
            self._handle = None
    
    def __enter__(self):
        """コンテキストマネージャーのエントリ"""
        return self
    
    def __exit__(self, exc_type, exc_val, exc_tb):
        """コンテキストマネージャーのイグジット"""
        self.stop()
        return False
    
    def _find_dll(self, dll_path: Optional[str]) -> Path:
        """DLLパスを検索"""
        logger = logging.getLogger(__name__)
        
        if dll_path:
            path = Path(dll_path)
            if path.exists():
                logger.info(f"DLL loaded from specified path: {path.resolve()}")
                return path
            raise FileNotFoundError(f"DLL not found: {dll_path}")
        
        # デフォルトの検索パス（ビルド出力ディレクトリを優先）
        search_paths = [
            # CMakeビルド出力（Debug優先、開発中はDebugが最新の可能性が高い）
            Path(__file__).parent.parent / "cpp" / "build" / "bin" / "Debug" / "ytdlpspout.dll",
            Path(__file__).parent.parent / "cpp" / "build" / "bin" / "Release" / "ytdlpspout.dll",
            # ローカルpythonフォルダ
            Path(__file__).parent / "ytdlpspout.dll",
            # レガシーパス（VS2022）
            Path(__file__).parent.parent / "cpp" / "build" / "vs2022" / "bin" / "Release" / "ytdlpspout.dll",
            Path(__file__).parent.parent / "cpp" / "build" / "vs2022" / "bin" / "Debug" / "ytdlpspout.dll",
            # Presetビルドパス
            Path(__file__).parent.parent / "cpp" / "build" / "windows-x64-release" / "bin" / "ytdlpspout.dll",
            Path(__file__).parent.parent / "cpp" / "build" / "windows-x64-debug" / "bin" / "ytdlpspout.dll",
        ]
        
        for path in search_paths:
            if path.exists():
                logger.info(f"DLL loaded from: {path.resolve()}")
                return path
        
        # 環境変数からも検索
        if "YTDLPSPOUT_DLL" in os.environ:
            path = Path(os.environ["YTDLPSPOUT_DLL"])
            if path.exists():
                logger.info(f"DLL loaded from environment variable: {path.resolve()}")
                return path
        
        logger.warning(f"DLL not found in search paths: {[str(p) for p in search_paths]}")
        raise FileNotFoundError(
            f"ytdlpspout.dll not found. Searched: {[str(p) for p in search_paths]}. "
            "Set YTDLPSPOUT_DLL environment variable or pass dll_path to constructor."
        )
    
    def _setup_functions(self):
        """DLL関数のシグネチャを設定"""
        lib = self._lib
        
        # const char* ytdlpspout_version(void)
        lib.ytdlpspout_version.restype = c_char_p
        lib.ytdlpspout_version.argtypes = []
        
        # YtdlpSpoutHandle ytdlpspout_create(void)
        lib.ytdlpspout_create.restype = c_void_p
        lib.ytdlpspout_create.argtypes = []
        
        # void ytdlpspout_destroy(YtdlpSpoutHandle handle)
        lib.ytdlpspout_destroy.restype = None
        lib.ytdlpspout_destroy.argtypes = [c_void_p]
        
        # int ytdlpspout_start(YtdlpSpoutHandle handle, const YtdlpSpoutConfig* config)
        lib.ytdlpspout_start.restype = c_int
        lib.ytdlpspout_start.argtypes = [c_void_p, POINTER(YtdlpSpoutConfig)]
        
        # void ytdlpspout_stop(YtdlpSpoutHandle handle)
        lib.ytdlpspout_stop.restype = None
        lib.ytdlpspout_stop.argtypes = [c_void_p]
        
        # void ytdlpspout_pause(YtdlpSpoutHandle handle)
        lib.ytdlpspout_pause.restype = None
        lib.ytdlpspout_pause.argtypes = [c_void_p]
        
        # void ytdlpspout_resume(YtdlpSpoutHandle handle)
        lib.ytdlpspout_resume.restype = None
        lib.ytdlpspout_resume.argtypes = [c_void_p]
        
        # int ytdlpspout_seek(YtdlpSpoutHandle handle, double seconds)
        lib.ytdlpspout_seek.restype = c_int
        lib.ytdlpspout_seek.argtypes = [c_void_p, c_double]
        
        # int ytdlpspout_process_frame(YtdlpSpoutHandle handle)
        lib.ytdlpspout_process_frame.restype = c_int
        lib.ytdlpspout_process_frame.argtypes = [c_void_p]
        
        # YtdlpSpoutState ytdlpspout_get_state(YtdlpSpoutHandle handle)
        lib.ytdlpspout_get_state.restype = c_int
        lib.ytdlpspout_get_state.argtypes = [c_void_p]
        
        # int ytdlpspout_is_playing(YtdlpSpoutHandle handle)
        lib.ytdlpspout_is_playing.restype = c_int
        lib.ytdlpspout_is_playing.argtypes = [c_void_p]
        
        # double ytdlpspout_get_position(YtdlpSpoutHandle handle)
        lib.ytdlpspout_get_position.restype = c_double
        lib.ytdlpspout_get_position.argtypes = [c_void_p]
        
        # double ytdlpspout_get_current_time(YtdlpSpoutHandle handle)
        lib.ytdlpspout_get_current_time.restype = c_double
        lib.ytdlpspout_get_current_time.argtypes = [c_void_p]
        
        # double ytdlpspout_get_duration(YtdlpSpoutHandle handle)
        lib.ytdlpspout_get_duration.restype = c_double
        lib.ytdlpspout_get_duration.argtypes = [c_void_p]
        
        # int ytdlpspout_get_video_info(YtdlpSpoutHandle handle, YtdlpSpoutVideoInfo* info)
        lib.ytdlpspout_get_video_info.restype = c_int
        lib.ytdlpspout_get_video_info.argtypes = [c_void_p, POINTER(YtdlpSpoutVideoInfo)]
        
        # int ytdlpspout_jump_beats(YtdlpSpoutHandle handle, int beats, int forward)
        lib.ytdlpspout_jump_beats.restype = c_int
        lib.ytdlpspout_jump_beats.argtypes = [c_void_p, c_int, c_int]
        
        # float ytdlpspout_get_bpm(YtdlpSpoutHandle handle)
        lib.ytdlpspout_get_bpm.restype = c_float
        lib.ytdlpspout_get_bpm.argtypes = [c_void_p]
        
        # const char* ytdlpspout_get_last_error(void)
        lib.ytdlpspout_get_last_error.restype = c_char_p
        lib.ytdlpspout_get_last_error.argtypes = []
        
        # void ytdlpspout_set_progress_callback(...)
        lib.ytdlpspout_set_progress_callback.restype = None
        lib.ytdlpspout_set_progress_callback.argtypes = [c_void_p, ProgressCallbackType, c_void_p]
        
        # void ytdlpspout_set_error_callback(...)
        lib.ytdlpspout_set_error_callback.restype = None
        lib.ytdlpspout_set_error_callback.argtypes = [c_void_p, ErrorCallbackType, c_void_p]
        
        # void ytdlpspout_set_completion_callback(...)
        lib.ytdlpspout_set_completion_callback.restype = None
        lib.ytdlpspout_set_completion_callback.argtypes = [c_void_p, CompletionCallbackType, c_void_p]
        
        # int ytdlpspout_get_frame_buffer_size(YtdlpSpoutHandle handle)
        lib.ytdlpspout_get_frame_buffer_size.restype = c_int
        lib.ytdlpspout_get_frame_buffer_size.argtypes = [c_void_p]
        
        # int ytdlpspout_get_current_frame(...)
        lib.ytdlpspout_get_current_frame.restype = c_int
        lib.ytdlpspout_get_current_frame.argtypes = [c_void_p, POINTER(c_uint8), c_int, POINTER(c_int), POINTER(c_int)]
        
        # =====================================================================
        # スライス読み込みAPI（Phase 4）
        # =====================================================================
        
        # void ytdlpspout_config_ex_init(YtdlpSpoutConfigEx* config)
        lib.ytdlpspout_config_ex_init.restype = None
        lib.ytdlpspout_config_ex_init.argtypes = [POINTER(YtdlpSpoutConfigEx)]
        
        # int ytdlpspout_start_ex(YtdlpSpoutHandle handle, const YtdlpSpoutConfigEx* config)
        lib.ytdlpspout_start_ex.restype = c_int
        lib.ytdlpspout_start_ex.argtypes = [c_void_p, POINTER(YtdlpSpoutConfigEx)]
        
        # double ytdlpspout_get_download_progress(YtdlpSpoutHandle handle)
        lib.ytdlpspout_get_download_progress.restype = c_double
        lib.ytdlpspout_get_download_progress.argtypes = [c_void_p]
        
        # double ytdlpspout_get_bandwidth(YtdlpSpoutHandle handle)
        lib.ytdlpspout_get_bandwidth.restype = c_double
        lib.ytdlpspout_get_bandwidth.argtypes = [c_void_p]
        
        # int ytdlpspout_is_fully_cached(YtdlpSpoutHandle handle)
        lib.ytdlpspout_is_fully_cached.restype = c_int
        lib.ytdlpspout_is_fully_cached.argtypes = [c_void_p]
        
        # void ytdlpspout_get_cache_stats(YtdlpSpoutHandle handle, size_t* cachedChunks, size_t* totalChunks)
        lib.ytdlpspout_get_cache_stats.restype = None
        lib.ytdlpspout_get_cache_stats.argtypes = [c_void_p, POINTER(c_size_t), POINTER(c_size_t)]
        
        # int ytdlpspout_get_hls_cache_stats(YtdlpSpoutHandle handle, YtdlpSpoutHlsCacheStats* stats)
        lib.ytdlpspout_get_hls_cache_stats.restype = c_int
        lib.ytdlpspout_get_hls_cache_stats.argtypes = [c_void_p, POINTER(YtdlpSpoutHlsCacheStats)]
    
    # =========================================================================
    # プロパティ
    # =========================================================================
    
    @property
    def version(self) -> str:
        """ライブラリのバージョンを取得"""
        return self._lib.ytdlpspout_version().decode('utf-8')
    
    @property
    def is_playing(self) -> bool:
        """再生中かどうか"""
        return self._lib.ytdlpspout_is_playing(self._handle) != 0
    
    @property
    def state(self) -> int:
        """再生状態を取得"""
        return self._lib.ytdlpspout_get_state(self._handle)
    
    @property
    def position(self) -> float:
        """現在の再生位置（秒）"""
        return self._lib.ytdlpspout_get_position(self._handle)
    
    @property
    def duration(self) -> float:
        """総再生時間（秒）"""
        return self._lib.ytdlpspout_get_duration(self._handle)
    
    @property
    def bpm(self) -> float:
        """ビートマップのBPM（なければ0）"""
        return self._lib.ytdlpspout_get_bpm(self._handle)
    
    # =========================================================================
    # スライス読み込みプロパティ（Phase 4）
    # =========================================================================
    
    @property
    def download_progress(self) -> float:
        """ダウンロード進捗（0.0〜1.0）"""
        return self._lib.ytdlpspout_get_download_progress(self._handle)
    
    @property
    def bandwidth(self) -> float:
        """推定帯域幅（bytes/sec）"""
        return self._lib.ytdlpspout_get_bandwidth(self._handle)
    
    @property
    def is_fully_cached(self) -> bool:
        """全チャンクがキャッシュ済みか"""
        return self._lib.ytdlpspout_is_fully_cached(self._handle) != 0
    
    def get_cache_stats(self) -> tuple:
        """
        キャッシュ統計を取得
        
        Returns:
            (キャッシュ済みチャンク数, 総チャンク数) のタプル
        """
        cached = c_size_t()
        total = c_size_t()
        self._lib.ytdlpspout_get_cache_stats(self._handle, byref(cached), byref(total))
        return (cached.value, total.value)
    
    def get_hls_cache_stats(self) -> Optional[dict]:
        """
        HLSキャッシュ統計を取得
        
        Returns:
            HLS統計情報の辞書、または失敗時None
            - cached_segments: キャッシュ済みセグメント数
            - total_segments: 総セグメント数
            - download_progress: ダウンロード進捗 (0.0〜1.0)
            - bandwidth: 推定帯域幅 (bytes/sec)
            - is_fully_cached: 完全キャッシュ済み
            - is_hls_mode: HLSモードで再生中
        """
        stats = YtdlpSpoutHlsCacheStats()
        result = self._lib.ytdlpspout_get_hls_cache_stats(self._handle, byref(stats))
        if result != 0:
            return None
        return {
            "cached_segments": stats.cachedSegments,
            "total_segments": stats.totalSegments,
            "download_progress": stats.downloadProgress,
            "bandwidth": stats.bandwidth,
            "is_fully_cached": bool(stats.isFullyCached),
            "is_hls_mode": bool(stats.isHlsMode),
        }
    
    # =========================================================================
    # 再生制御
    # =========================================================================
    
    def start(
        self,
        input_file: str,
        sender_name: str = "ytdlpSpout",
        loop: bool = False,
        use_hardware_accel: bool = True,
        verbose: bool = False
    ) -> bool:
        """
        再生を開始
        
        Args:
            input_file: 入力ファイルパスまたはURL
            sender_name: Spout Sender名
            loop: ループ再生するかどうか
            use_hardware_accel: ハードウェアアクセラレーションを使用するか
            verbose: 詳細ログを出力するか
        
        Returns:
            成功した場合True
        """
        config = YtdlpSpoutConfig()
        config.inputFile = input_file.encode('utf-8')
        config.senderName = sender_name.encode('utf-8')
        config.loop = 1 if loop else 0
        config.useHardwareAccel = 1 if use_hardware_accel else 0
        config.verbose = 1 if verbose else 0
        
        result = self._lib.ytdlpspout_start(self._handle, byref(config))
        if result != 0:
            error = self.get_last_error()
            raise RuntimeError(f"Failed to start playback: {error}")
        return True
    
    def start_ex(
        self,
        source: str,
        sender_name: str = "ytdlpSpout",
        output_width: int = 0,
        output_height: int = 0,
        loop: bool = False,
        use_hardware_accel: bool = True,
        verbose: bool = False,
        slice_enabled: bool = True,
        chunk_size: Optional[int] = None,
        max_cache_memory: Optional[int] = None,
        max_concurrent_downloads: Optional[int] = None,
        prefetch_chunks_ahead: Optional[int] = None,
        cache_path: Optional[str] = None,
        ytdlp_path: Optional[str] = None,
        preferred_height: int = 1080,
        http_headers: Optional[dict] = None,
        is_hls: Optional[bool] = None
    ) -> bool:
        """
        拡張設定で再生開始（スライス読み込み対応）

        Args:
            source: ファイルパスまたはURL
            sender_name: Spout Sender名
            output_width: 出力幅（0=ソース解像度）
            output_height: 出力高さ（0=ソース解像度）
            loop: ループ再生するか
            use_hardware_accel: ハードウェアアクセラレーションを使用するか
            verbose: 詳細ログを出力するか
            slice_enabled: スライス読み込みを有効にするか
            chunk_size: チャンクサイズ（バイト）。None（デフォルト）の場合、
                ytdlpspout_config_ex_init()が設定するC++側のチューニング済み既定値
                （2MB、高解像度向け）を維持する。明示的に指定した場合のみ上書きする。
            max_cache_memory: 最大キャッシュメモリ（バイト）。Noneの場合はC++側の
                既定値（256MB）を維持する。
            max_concurrent_downloads: 最大並列ダウンロード数。Noneの場合はC++側の
                既定値（6）を維持する。
            prefetch_chunks_ahead: 先読みチャンク数。Noneの場合はC++側の既定値
                （24）を維持する。
            cache_path: ファイルキャッシュパス（Noneでメモリのみ）
            ytdlp_path: yt-dlpパス（Noneで自動検出）
            preferred_height: 希望解像度
            http_headers: HTTPヘッダー辞書（Cookieなど）
            is_hls: HLS判定ヒント（None=C++側で自動判定、True/False=呼び出し側の判定結果を強制）

        Returns:
            成功した場合True
        """
        # 文字列を保持するための参照（GC対策）
        self._config_ex_refs = []
        
        config = YtdlpSpoutConfigEx()
        self._lib.ytdlpspout_config_ex_init(byref(config))
        
        source_bytes = source.encode('utf-8')
        self._config_ex_refs.append(source_bytes)
        config.source = source_bytes
        
        sender_bytes = sender_name.encode('utf-8')
        self._config_ex_refs.append(sender_bytes)
        config.senderName = sender_bytes
        
        config.outputWidth = output_width
        config.outputHeight = output_height
        config.loop = 1 if loop else 0
        config.useHardwareAccel = 1 if use_hardware_accel else 0
        config.verbose = 1 if verbose else 0
        
        config.slice.enabled = 1 if slice_enabled else 0
        # PLY-2: Noneの場合はytdlpspout_config_ex_init()が設定したC++側の
        # チューニング済み既定値（chunkSize=2MB等）を保持する。呼び出し側が
        # 明示的に値を渡した場合のみ、その値でconfigを上書きする。
        if chunk_size is not None:
            config.slice.chunkSize = chunk_size
        if max_cache_memory is not None:
            config.slice.maxCacheMemory = max_cache_memory
        if max_concurrent_downloads is not None:
            config.slice.maxConcurrentDownloads = max_concurrent_downloads
        if prefetch_chunks_ahead is not None:
            config.slice.prefetchChunksAhead = prefetch_chunks_ahead
        if cache_path:
            cache_path_bytes = cache_path.encode('utf-8')
            self._config_ex_refs.append(cache_path_bytes)
            config.slice.cachePath = cache_path_bytes
        
        if ytdlp_path:
            ytdlp_path_bytes = ytdlp_path.encode('utf-8')
            self._config_ex_refs.append(ytdlp_path_bytes)
            config.ytdlp.path = ytdlp_path_bytes
        config.ytdlp.preferredHeight = preferred_height
        
        # HTTPヘッダーを設定
        if http_headers and len(http_headers) > 0:
            # ヘッダー配列の上限チェック（100個まで）
            MAX_HEADERS = 100
            if len(http_headers) > MAX_HEADERS:
                logging.warning(
                    f"HTTPヘッダー数が上限を超えています: {len(http_headers)} > {MAX_HEADERS}。"
                    f"最初の{MAX_HEADERS}個のみ使用します。"
                )
                # 辞書から最初のMAX_HEADERS個を取得
                http_headers = dict(list(http_headers.items())[:MAX_HEADERS])
            
            # ヘッダー配列を作成
            HeaderArray = YtdlpSpoutHttpHeader * len(http_headers)
            header_array = HeaderArray()
            
            for i, (key, value) in enumerate(http_headers.items()):
                key_bytes = key.encode('utf-8')
                value_bytes = value.encode('utf-8')
                # 文字列参照を保持（GC対策）
                self._config_ex_refs.extend([key_bytes, value_bytes])
                
                header_array[i].key = key_bytes
                header_array[i].value = value_bytes
            
            # 配列参照を保持
            self._config_ex_refs.append(header_array)
            config.httpHeaders = header_array
            config.httpHeadersCount = len(http_headers)
        else:
            config.httpHeaders = None
            config.httpHeadersCount = 0

        # HLS判定ヒント: None=自動判定(-1)、True/False=呼び出し側の判定結果を強制
        if is_hls is None:
            config.isHlsHint = -1
        else:
            config.isHlsHint = 1 if is_hls else 0

        result = self._lib.ytdlpspout_start_ex(self._handle, byref(config))
        if result != 0:
            error = self.get_last_error()
            raise RuntimeError(f"Failed to start playback: {error}")
        return True
    
    def stop(self):
        """再生を停止"""
        self._lib.ytdlpspout_stop(self._handle)
    
    def pause(self):
        """一時停止"""
        self._lib.ytdlpspout_pause(self._handle)
    
    def resume(self):
        """再生再開"""
        self._lib.ytdlpspout_resume(self._handle)
    
    def toggle_pause(self):
        """一時停止状態を切り替え"""
        if self.state == YtdlpSpoutState.PLAYING:
            self.pause()
        elif self.state == YtdlpSpoutState.PAUSED:
            self.resume()
    
    def seek(self, seconds: float) -> bool:
        """
        指定時刻にシーク
        
        Args:
            seconds: シーク先の時刻（秒）
        
        Returns:
            成功した場合True
        """
        return self._lib.ytdlpspout_seek(self._handle, c_double(seconds)) == 0
    
    def process_frame(self) -> bool:
        """
        1フレームを処理
        
        Returns:
            処理成功=True、終了/エラー=False
        """
        return self._lib.ytdlpspout_process_frame(self._handle) != 0
    
    # =========================================================================
    # ビート機能
    # =========================================================================
    
    def jump_beats(self, beats: int, forward: bool = True) -> bool:
        """
        ビート単位でジャンプ
        
        Args:
            beats: ジャンプするビート数
            forward: True=前進、False=後退
        
        Returns:
            成功した場合True
        """
        return self._lib.ytdlpspout_jump_beats(
            self._handle, c_int(beats), c_int(1 if forward else 0)
        ) == 0
    
    # =========================================================================
    # 動画情報
    # =========================================================================
    
    def get_video_info(self) -> Optional[dict]:
        """
        動画情報を取得
        
        Returns:
            動画情報の辞書（幅、高さ、FPS、再生時間、総フレーム数）
        """
        info = YtdlpSpoutVideoInfo()
        if self._lib.ytdlpspout_get_video_info(self._handle, byref(info)) != 0:
            return None
        
        return {
            "width": info.width,
            "height": info.height,
            "fps": info.fps,
            "duration": info.duration,
            "total_frames": info.totalFrames,
        }
    
    # =========================================================================
    # コールバック
    # =========================================================================
    
    def set_progress_callback(self, callback: Optional[Callable[[float, float], None]]):
        """
        進捗コールバックを設定
        
        Args:
            callback: (current_time, duration) を受け取るコールバック関数
        """
        self._on_progress = callback
        
        if callback:
            @ProgressCallbackType
            def c_callback(current_time, duration, user_data):
                if self._on_progress:
                    self._on_progress(current_time, duration)
            
            self._progress_callback_ref = c_callback
            self._lib.ytdlpspout_set_progress_callback(
                self._handle, c_callback, None
            )
        else:
            self._progress_callback_ref = None
            self._lib.ytdlpspout_set_progress_callback(
                self._handle, ProgressCallbackType(), None
            )
    
    def set_error_callback(self, callback: Optional[Callable[[str], None]]):
        """
        エラーコールバックを設定
        
        Args:
            callback: (message) を受け取るコールバック関数
        """
        self._on_error = callback
        
        if callback:
            @ErrorCallbackType
            def c_callback(message, user_data):
                if self._on_error:
                    self._on_error(message.decode('utf-8') if message else "")
            
            self._error_callback_ref = c_callback
            self._lib.ytdlpspout_set_error_callback(
                self._handle, c_callback, None
            )
        else:
            self._error_callback_ref = None
            self._lib.ytdlpspout_set_error_callback(
                self._handle, ErrorCallbackType(), None
            )
    
    def set_completion_callback(self, callback: Optional[Callable[[], None]]):
        """
        完了コールバックを設定
        
        Args:
            callback: 再生完了時に呼ばれるコールバック関数
        """
        self._on_completion = callback
        
        if callback:
            @CompletionCallbackType
            def c_callback(user_data):
                if self._on_completion:
                    self._on_completion()
            
            self._completion_callback_ref = c_callback
            self._lib.ytdlpspout_set_completion_callback(
                self._handle, c_callback, None
            )
        else:
            self._completion_callback_ref = None
            self._lib.ytdlpspout_set_completion_callback(
                self._handle, CompletionCallbackType(), None
            )
    
    # =========================================================================
    # ユーティリティ
    # =========================================================================
    
    def get_last_error(self) -> str:
        """最後のエラーメッセージを取得"""
        error = self._lib.ytdlpspout_get_last_error()
        return error.decode('utf-8') if error else ""
    
    # =========================================================================
    # フレームバッファ
    # =========================================================================
    
    def get_frame_buffer_size(self) -> int:
        """
        必要なフレームバッファサイズを取得
        
        Returns:
            フレームバッファサイズ（バイト）、無効な場合は0
        """
        return self._lib.ytdlpspout_get_frame_buffer_size(self._handle)
    
    def get_current_frame(self):
        """
        現在のフレームをBGRA numpy配列として取得

        Returns:
            numpy.ndarray (height, width, 4) BGRA形式、失敗時はNone
        """
        buffer_size = self.get_frame_buffer_size()
        if buffer_size <= 0:
            return None

        # バッファはインスタンスで使い回し、サイズが変化した場合のみ再確保する
        if self._frame_buffer is None or self._frame_buffer_size != buffer_size:
            self._frame_buffer = (ctypes.c_uint8 * buffer_size)()
            self._frame_buffer_size = buffer_size
        buffer = self._frame_buffer
        width = ctypes.c_int()
        height = ctypes.c_int()

        result = self._lib.ytdlpspout_get_current_frame(
            self._handle,
            buffer,
            buffer_size,
            ctypes.byref(width),
            ctypes.byref(height)
        )

        if result != 0:
            return None

        import numpy as np
        # バッファは次回呼び出しで上書きされるため、返却前にコピーして
        # 呼び出し側が保持する参照の独立性（従来の意味論）を維持する
        frame = np.frombuffer(buffer, dtype=np.uint8)
        frame = frame.reshape((height.value, width.value, 4)).copy()
        return frame


# =============================================================================
# 便利関数
# =============================================================================

def get_version(dll_path: Optional[str] = None) -> str:
    """
    ライブラリバージョンを取得（インスタンス作成不要）
    
    Args:
        dll_path: DLLパス（オプション）
    
    Returns:
        バージョン文字列
    """
    player = YtdlpSpoutNative(dll_path)
    return player.version


def play_video(
    input_file: str,
    sender_name: str = "ytdlpSpout",
    loop: bool = False,
    dll_path: Optional[str] = None
):
    """
    動画を再生する簡単なヘルパー関数
    
    Args:
        input_file: 入力ファイルパスまたはURL
        sender_name: Spout Sender名
        loop: ループ再生
        dll_path: DLLパス（オプション）
    """
    with YtdlpSpoutNative(dll_path) as player:
        player.start(input_file, sender_name=sender_name, loop=loop)
        while player.is_playing:
            if not player.process_frame():
                break
