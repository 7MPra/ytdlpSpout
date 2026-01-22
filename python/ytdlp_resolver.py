"""
ytdlp_resolver.py - yt-dlp URLの非同期解決モジュール

GUIの「Start」ボタン押下後に即座に再生を開始するため、
Python側でyt-dlp URL解決を先行実行し、解決済みURLをC++ DLLに渡す。

使用例:
    from python.ytdlp_resolver import YtDlpAsyncResolver, ResolvedInfo
    
    # 非同期解決
    resolver = YtDlpAsyncResolver()
    future = resolver.resolve_async("https://www.youtube.com/watch?v=xxxxx")
    result = future.result(timeout=30.0)  # ResolvedInfo or None
    
    # 同期解決
    result = resolver.resolve_sync("https://www.youtube.com/watch?v=xxxxx")
    
    # クリーンアップ
    resolver.shutdown()
"""

import re
import concurrent.futures
from dataclasses import dataclass, field
from typing import Optional, Callable, List
from urllib.parse import urlparse

import yt_dlp


@dataclass
class ResolvedInfo:
    """
    yt-dlpで解決されたストリームURL情報
    
    Attributes:
        stream_url: 解決された直接ストリームURL
        duration: 動画の長さ（秒）、ライブの場合は0
        width: 動画の幅（ピクセル）
        height: 動画の高さ（ピクセル）
        title: 動画のタイトル
        http_headers: HTTPヘッダー（Cookieなど）
    """
    stream_url: str
    duration: float = 0.0
    width: int = 0
    height: int = 0
    title: str = ""
    http_headers: dict = field(default_factory=dict)


class YtDlpAsyncResolver:
    """
    yt-dlp URLの非同期解決クラス
    
    YouTubeなどのyt-dlp対応サイトのURLを解決し、
    直接ストリームURLを取得する。
    """
    
    # yt-dlpで解決が必要なドメインのパターン
    YTDLP_DOMAINS: List[str] = [
        # 動画サイト
        'youtube.com', 'youtu.be', 'youtube-nocookie.com',
        'twitch.tv',
        'nicovideo.jp', 'nico.ms', 'live.nicovideo.jp',
        'vimeo.com',
        'dailymotion.com',
        'bilibili.com', 'bilibili.tv',
        
        # SNS動画
        'twitter.com', 'x.com',
        'instagram.com',
        'tiktok.com',
        'facebook.com', 'fb.watch',
        
        # その他
        'soundcloud.com',
        'bandcamp.com',
        'reddit.com',
        'pornhub.com', 'xvideos.com',  # 18+
    ]
    
    # 直接再生可能なファイル拡張子
    DIRECT_EXTENSIONS: List[str] = [
        '.mp4', '.webm', '.mkv', '.avi', '.mov', '.flv', '.wmv',
        '.m3u8', '.mpd', '.ts',
        '.mp3', '.ogg', '.wav', '.flac', '.m4a',
    ]
    
    # フォールバックフォーマットのリスト（優先順）
    # ニコニコ動画等のAAC音声対応、汎用フォールバックを含む
    FALLBACK_FORMATS: List[str] = [
        'bestvideo[ext=mp4]+bestaudio[ext=m4a]',  # YouTube向け優先
        'bestvideo[ext=mp4]+bestaudio[ext=aac]',  # ニコニコ動画等のaac音声対応
        'bestvideo+bestaudio',  # 一般的なフォーマット
        'bv*+ba',  # yt-dlp推奨の汎用フォーマット
        'b',  # best shorthand
        'best',  # 最終フォールバック
    ]
    
    # ニコニコ動画対応のフォーマット文字列（FALLBACK_FORMATS から自動生成）
    FORMAT_STRING = '/'.join(FALLBACK_FORMATS)
    
    def __init__(
        self,
        log_cb: Optional[Callable[[str], None]] = None,
        cookie_file: Optional[str] = None,
        verbose: bool = False
    ):
        """
        YtDlpAsyncResolverを初期化
        
        Args:
            log_cb: ログコールバック関数
            cookie_file: cookieファイルのパス（オプション）
            verbose: 詳細ログを出力するか
        """
        self._log_cb = log_cb
        self._cookie_file = cookie_file
        self._verbose = verbose
        self._cancelled = False
        
        # スレッドプールエグゼキュータ（1スレッド）
        self._executor = concurrent.futures.ThreadPoolExecutor(
            max_workers=1,
            thread_name_prefix="ytdlp_resolver"
        )
        
        # 現在のFuture（キャンセル用）
        self._current_future: Optional[concurrent.futures.Future] = None
    
    def log(self, msg: str) -> None:
        """ログメッセージを出力"""
        if self._log_cb:
            self._log_cb(msg)
    
    def _get_ydl_opts(self) -> dict:
        """
        yt-dlpオプションを取得
        
        Returns:
            yt-dlpオプション辞書
        """
        return {
            'format': self.FORMAT_STRING,
            'noplaylist': True,
            'quiet': not self._verbose,
            'nocheckcertificate': True,
            'socket_timeout': 30,
            'retries': 3,
            'user_agent': 'Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36',
            'cookiefile': self._cookie_file,
        }
    
    def _log_cookie_info(self) -> None:
        """
        Cookieファイル使用状況をログ出力（内容は含めない）
        
        セキュリティ上、Cookie値自体はログに出力しない
        """
        if self._cookie_file:
            import os
            if os.path.exists(self._cookie_file):
                file_size = os.path.getsize(self._cookie_file)
                self.log(f"[yt-dlp] Cookieファイルを使用: {self._cookie_file} ({file_size} bytes)")
            else:
                self.log(f"[yt-dlp] Cookieファイルが見つかりません: {self._cookie_file}")
    
    @staticmethod
    def is_ytdlp_url(url: str) -> bool:
        """
        指定されたURLがyt-dlpで解決が必要かどうかを判定
        
        Args:
            url: 判定対象のURL
            
        Returns:
            True: yt-dlpで解決が必要
            False: 直接再生可能（ローカルファイル、直接ストリームURLなど）
        """
        if not url or not url.strip():
            return False
        
        url = url.strip()
        
        # ローカルファイルパスの判定
        # Windowsパス (C:\, D:\ など) または Unix絶対パス (/) または相対パス (./)
        if re.match(r'^[A-Za-z]:', url) or url.startswith('/') or url.startswith('./') or url.startswith('..'):
            return False
        
        # URLの解析
        try:
            parsed = urlparse(url)
        except Exception:
            return False
        
        # スキームがない場合はローカルファイルと見なす
        if not parsed.scheme:
            return False
        
        # http/https以外のスキーム
        if parsed.scheme not in ('http', 'https'):
            return False
        
        # ホスト名がない場合
        if not parsed.netloc:
            return False
        
        # 直接再生可能な拡張子を持つURLはyt-dlp不要
        path_lower = parsed.path.lower()
        for ext in YtDlpAsyncResolver.DIRECT_EXTENSIONS:
            if path_lower.endswith(ext):
                return False
        
        # yt-dlp対応ドメインのチェック
        host = parsed.netloc.lower()
        # www.を除去
        if host.startswith('www.'):
            host = host[4:]
        
        for domain in YtDlpAsyncResolver.YTDLP_DOMAINS:
            if host == domain or host.endswith('.' + domain):
                return True
        
        # 未知のドメインの場合、yt-dlpで試してみる価値がある
        # ただし、単純なファイルURLでないことを確認
        # → 保守的に、既知のドメインのみTrueを返す
        return False
    
    def resolve_sync(self, url: str) -> Optional[ResolvedInfo]:
        """
        URLを同期的に解決
        
        Args:
            url: 解決対象のURL
            
        Returns:
            ResolvedInfo: 解決成功時
            None: 解決失敗時またはキャンセル時
        """
        if self._cancelled:
            return None
        
        self.log(f"[yt-dlp] URL解決開始: {url}")
        
        # Cookie情報をログ出力
        self._log_cookie_info()
        
        # yt-dlpオプションを設定
        class YtDlpLogger:
            def __init__(self, log_func, verbose):
                self.log = log_func
                self.verbose = verbose
            
            def debug(self, msg):
                if self.verbose and not msg.startswith('[debug]'):
                    self.log(f"[yt-dlp] {msg}")
            
            def info(self, msg):
                self.log(f"[yt-dlp] {msg}")
            
            def warning(self, msg):
                self.log(f"[yt-dlp 警告] {msg}")
            
            def error(self, msg):
                self.log(f"[yt-dlp エラー] {msg}")
        
        logger = YtDlpLogger(self.log, self._verbose)
        
        # 共通オプションを取得
        ydl_opts = self._get_ydl_opts()
        ydl_opts['logger'] = logger
        
        try:
            with yt_dlp.YoutubeDL(ydl_opts) as ydl:
                if self._cancelled:
                    return None
                
                info = ydl.extract_info(url, download=False)
                
                if self._cancelled:
                    return None
                
                if not info:
                    self.log("[yt-dlp] 情報取得失敗: 結果が空")
                    return None
                
                # ストリームURLを取得
                stream_url = info.get('url')
                http_headers = info.get('http_headers', {}) or {}
                
                # urlがない場合、requested_formatsから取得を試みる
                if not stream_url:
                    requested_formats = info.get('requested_formats')
                    if isinstance(requested_formats, list):
                        for fmt in requested_formats:
                            if fmt and fmt.get('vcodec') not in (None, 'none') and fmt.get('url'):
                                stream_url = fmt.get('url')
                                http_headers = fmt.get('http_headers', {}) or http_headers
                                break
                
                if not stream_url:
                    self.log("[yt-dlp] 解決失敗: ストリームURLが見つかりません")
                    return None
                
                # Cookieをヘッダーに追加
                if hasattr(ydl, 'cookiejar'):
                    cookies = []
                    for cookie in ydl.cookiejar:
                        cookies.append(f"{cookie.name}={cookie.value}")
                    if cookies:
                        http_headers['Cookie'] = "; ".join(cookies)
                
                # ResolvedInfoを構築
                result = ResolvedInfo(
                    stream_url=stream_url,
                    duration=info.get('duration', 0.0) or 0.0,
                    width=info.get('width', 0) or 0,
                    height=info.get('height', 0) or 0,
                    title=info.get('title', '') or '',
                    http_headers=http_headers
                )
                
                self.log(f"[yt-dlp] URL解決完了: {result.width}x{result.height}, {result.duration:.1f}秒")
                return result
                
        except Exception as e:
            self.log(f"[yt-dlp] 解決エラー: {e}")
            return None
    
    def resolve_async(self, url: str) -> concurrent.futures.Future:
        """
        URLを非同期的に解決
        
        Args:
            url: 解決対象のURL
            
        Returns:
            Future[ResolvedInfo | None]: 解決結果のFuture
        """
        self._cancelled = False
        self._current_future = self._executor.submit(self.resolve_sync, url)
        return self._current_future
    
    def cancel(self) -> None:
        """
        進行中の解決をキャンセル
        """
        self._cancelled = True
        if self._current_future and not self._current_future.done():
            self._current_future.cancel()
    
    def shutdown(self) -> None:
        """
        リソースをクリーンアップ
        """
        self._cancelled = True
        if self._executor:
            self._executor.shutdown(wait=False)
