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

import os
import re
import concurrent.futures
from dataclasses import dataclass, field
from pathlib import Path
from typing import Optional, Callable, List, Union
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
        protocol: yt-dlpが報告したプロトコル（例: 'm3u8_native', 'https'）。不明なら空文字
        is_live: ライブ配信かどうか（yt-dlpのinfoから取得）
    """
    stream_url: str
    duration: float = 0.0
    width: int = 0
    height: int = 0
    title: str = ""
    http_headers: dict = field(default_factory=dict)
    protocol: str = ""
    is_live: bool = False

    @property
    def is_hls(self) -> Optional[bool]:
        """
        protocolからHLSストリームかどうかを判定する

        Returns:
            True: protocolに'm3u8'を含む（HLS）
            False: protocolは設定されているが'm3u8'を含まない
            None: protocolが未設定で判定不能
        """
        if not self.protocol:
            return None
        return 'm3u8' in self.protocol


class YtDlpAsyncResolver:
    """
    yt-dlp URLの非同期解決クラス
    
    YouTubeなどのyt-dlp対応サイトのURLを解決し、
    直接ストリームURLを取得する。
    """
    
    # 既知のyt-dlp対応ドメイン。is_ytdlp_url()ではこのリストに一致するドメインは
    # パスが直接メディア拡張子で終わっていてもyt-dlp解決を優先する判定にのみ使用する
    # （未知ドメインもyt-dlp解決自体は試みるため、allowlistとしては機能しない）
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
    # AAC音声対応・汎用フォールバックを含む
    FALLBACK_FORMATS: List[str] = [
        'bestvideo[ext=mp4]+bestaudio[ext=m4a]',
        'bestvideo[ext=mp4]+bestaudio[ext=aac]',
        'bestvideo+bestaudio',
        'bv*+ba',
        'b',
        'best',
    ]
    
    FORMAT_STRING = '/'.join(FALLBACK_FORMATS)
    
    # 単一フォーマット優先：HLSのみ提供などで bestvideo+bestaudio が使えないソース用
    SINGLE_FORMAT_FIRST_ORDER: List[str] = [
        'best',
        'bestvideo[ext=mp4]+bestaudio[ext=m4a]',
        'bestvideo[ext=mp4]+bestaudio[ext=aac]',
        'bestvideo+bestaudio',
        'bv*+ba',
        'b',
    ]
    SINGLE_FORMAT_FIRST_FORMAT_STRING = '/'.join(SINGLE_FORMAT_FIRST_ORDER)
    
    # 単一フォーマット優先を適用するホスト（HLS単体配信など）
    _SINGLE_FORMAT_FIRST_HOSTS: frozenset = frozenset((
        'nicovideo.jp', 'nico.ms', 'live.nicovideo.jp',
    ))
    
    # 参照元ヘッダーが必要なホスト → Referer 値（CDN要件）
    _REFERER_BY_HOST: dict = {
        'nicovideo.jp': 'https://www.nicovideo.jp/',
        'nico.ms': 'https://www.nicovideo.jp/',
        'live.nicovideo.jp': 'https://www.nicovideo.jp/',
    }

    # デフォルトで探すCookieファイルの相対パス（全サイト共通で使用）
    DEFAULT_COOKIE_CANDIDATES: List[Union[str, Path]] = [
        Path("data") / "cookies.txt",
        Path("cookies.txt"),
    ]

    @classmethod
    def get_default_cookie_file(cls, base_dir: Optional[Union[str, Path]] = None) -> Optional[str]:
        """
        デフォルトのCookieファイルパスを取得する。
        存在する最初の候補を返す。全サイトで同じCookieファイルを利用する想定。

        Args:
            base_dir: 探索の基準ディレクトリ。Noneの場合はカレントディレクトリ。

        Returns:
            存在するCookieファイルの絶対パス。見つからなければ None。
        """
        base = Path(base_dir).resolve() if base_dir is not None else Path(os.getcwd()).resolve()
        for rel in cls.DEFAULT_COOKIE_CANDIDATES:
            p = base / rel
            if p.is_file():
                return str(p)
        return None

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
            cookie_file: Cookieファイルのパス。Noneの場合は get_default_cookie_file() で自動探索（全サイトで共通利用）。
            verbose: 詳細ログを出力するか
        """
        self._log_cb = log_cb
        # None = デフォルトを探索して全サイトで共通利用、'' = 使わない、パス = そのファイルを利用
        if cookie_file is None:
            self._cookie_file = self.get_default_cookie_file()
        elif cookie_file == "":
            self._cookie_file = None
        else:
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
    
    @staticmethod
    def _host_for_url(url: str) -> str:
        """URLからホスト名を正規化して返す（www 除去）"""
        if not url or not url.strip():
            return ""
        try:
            parsed = urlparse(url.strip())
            host = (parsed.netloc or "").lower()
            return host[4:] if host.startswith("www.") else host
        except Exception:
            return ""

    @classmethod
    def _use_single_format_first(cls, url: str) -> bool:
        """単一フォーマット優先のフォーマット文字列を使うか"""
        host = cls._host_for_url(url)
        if not host:
            return False
        if host in cls._SINGLE_FORMAT_FIRST_HOSTS:
            return True
        return any(host == h or host.endswith("." + h) for h in cls._SINGLE_FORMAT_FIRST_HOSTS)

    @classmethod
    def _referer_for_url(cls, url: str) -> Optional[str]:
        """URLに応じた参照元ヘッダーがあれば返す"""
        host = cls._host_for_url(url)
        return cls._REFERER_BY_HOST.get(host)

    @staticmethod
    def _cookie_domain_matches_host(cookie_domain: str, host: str) -> bool:
        """cookieのdomain属性がhostにマッチするか判定（先頭の'.'を考慮した末尾一致）"""
        if not cookie_domain or not host:
            return False
        domain = cookie_domain.lower()
        if domain.startswith('.'):
            domain = domain[1:]
        host = host.lower()
        return host == domain or host.endswith('.' + domain)

    @classmethod
    def _build_cookie_header(cls, cookiejar, host: str) -> str:
        """
        cookiejarから、stream_urlのホストにドメインマッチするCookieのみを連結する

        全cookieをドメイン無視で連結すると、他サイトのCookieが漏えいしたり、
        yt-dlpが計算したCookieヘッダーを不適切に上書きしてしまうため、
        ホストにマッチするものだけを対象にする。

        Args:
            cookiejar: yt_dlp.YoutubeDL.cookiejar（http.cookiejar.CookieJar互換）
            host: 対象ストリームURLのホスト名

        Returns:
            "key=value; key2=value2" 形式のCookieヘッダー値（該当なしなら空文字）
        """
        cookies = []
        for cookie in cookiejar:
            if cls._cookie_domain_matches_host(getattr(cookie, 'domain', ''), host):
                cookies.append(f"{cookie.name}={cookie.value}")
        return "; ".join(cookies)

    def _get_ydl_opts(self, url: Optional[str] = None) -> dict:
        """
        yt-dlpオプションを取得
        
        Args:
            url: 解決対象URL（単一フォーマット優先のソースの場合は別フォーマットを使用）
        
        Returns:
            yt-dlpオプション辞書
        """
        format_str = (
            self.SINGLE_FORMAT_FIRST_FORMAT_STRING
            if (url and self._use_single_format_first(url))
            else self.FORMAT_STRING
        )
        return {
            'format': format_str,
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
            if os.path.exists(self._cookie_file):
                file_size = os.path.getsize(self._cookie_file)
                self.log(f"[yt-dlp] Cookieファイルを使用: {self._cookie_file} ({file_size} bytes)")
            else:
                self.log(f"[yt-dlp] Cookieファイルが見つかりません: {self._cookie_file}")
    
    @classmethod
    def _is_known_ytdlp_domain(cls, host: str) -> bool:
        """既知のyt-dlp対応ドメイン（YTDLP_DOMAINSまたはそのサブドメイン）かどうか判定"""
        if not host:
            return False
        return any(host == domain or host.endswith('.' + domain) for domain in cls.YTDLP_DOMAINS)

    @staticmethod
    def is_ytdlp_url(url: str) -> bool:
        """
        指定されたURLがyt-dlpで解決が必要かどうかを判定

        allowlist方式ではなく、http(s)のページURLは基本的にyt-dlpで解決を試みる
        （yt-dlpは数千サイトに対応しており、既知ドメインのみに限定すると
        未対応の大多数のサイトが生URLのままC++/FFmpegに渡り再生失敗するため）。
        yt-dlpでの解決に失敗した場合は呼び出し側が直接URL再生にフォールバックする。

        Args:
            url: 判定対象のURL

        Returns:
            True: yt-dlpでの解決を試みるべき
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

        # ホスト名の正規化（www.を除去）
        host = parsed.netloc.lower()
        if host.startswith('www.'):
            host = host[4:]

        # 既知のyt-dlp対応ドメインは、パスが直接メディア拡張子で終わっていても
        # yt-dlpでの解決を優先する（例: YouTubeの内部CDN URL等との誤判定回避）
        if YtDlpAsyncResolver._is_known_ytdlp_domain(host):
            return True

        # 直接再生可能な拡張子を持つURLはyt-dlp不要
        path_lower = parsed.path.lower()
        for ext in YtDlpAsyncResolver.DIRECT_EXTENSIONS:
            if path_lower.endswith(ext):
                return False

        # 未知のドメインでも、直接メディアファイルでないページURLはyt-dlpで試す価値がある
        return True
    
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
        
        # 共通オプションを取得（フォーマット別の特別処理は _get_ydl_opts 内で適用）
        ydl_opts = self._get_ydl_opts(url)
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
                protocol = info.get('protocol', '') or ''

                # urlがない場合、requested_formatsから取得を試みる
                if not stream_url:
                    requested_formats = info.get('requested_formats')
                    if isinstance(requested_formats, list):
                        for fmt in requested_formats:
                            if fmt and fmt.get('vcodec') not in (None, 'none') and fmt.get('url'):
                                stream_url = fmt.get('url')
                                http_headers = fmt.get('http_headers', {}) or http_headers
                                protocol = fmt.get('protocol', protocol) or protocol
                                break

                if not stream_url:
                    self.log("[yt-dlp] 解決失敗: ストリームURLが見つかりません")
                    return None

                # Cookieをヘッダーに追加
                # yt-dlpが既にCookieヘッダーを計算している場合はそれを尊重し上書きしない。
                # ない場合のみ、stream_urlのホストにドメインマッチするCookieだけを連結する
                # （cookiejar全体を無条件連結すると他サイトのCookieが漏えいするため）
                if 'Cookie' not in http_headers and hasattr(ydl, 'cookiejar'):
                    cookie_header = self._build_cookie_header(ydl.cookiejar, self._host_for_url(stream_url))
                    if cookie_header:
                        http_headers['Cookie'] = cookie_header

                # 参照元ヘッダーが必要なソースのみ付与
                referer = self._referer_for_url(url)
                if referer and "Referer" not in http_headers:
                    http_headers["Referer"] = referer

                # ResolvedInfoを構築
                result = ResolvedInfo(
                    stream_url=stream_url,
                    duration=info.get('duration', 0.0) or 0.0,
                    width=info.get('width', 0) or 0,
                    height=info.get('height', 0) or 0,
                    title=info.get('title', '') or '',
                    http_headers=http_headers,
                    protocol=protocol,
                    is_live=bool(info.get('is_live', False))
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
