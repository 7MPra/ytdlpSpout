"""
NativeStreamerWrapper - C++ DLLをPython Streamer互換APIでラップ

gui.pyからの移行を最小限に抑えるための互換レイヤー

使用例:
    from python.native_streamer_wrapper import NativeStreamerWrapper

    wrapper = NativeStreamerWrapper(
        video_url="video.mp4",
        sender_name="MySpout",
        log_cb=lambda msg: print(msg)
    )
    wrapper.start()
    # ...
    wrapper.stop()
"""

import threading
import time
from typing import Optional, Callable, Any

try:
    import numpy as np
    HAS_NUMPY = True
except ImportError:
    HAS_NUMPY = False

from .ytdlpspout_native import YtdlpSpoutNative, YtdlpSpoutState


class NativeStreamerWrapper:
    """
    C++ DLL (YtdlpSpoutNative) をPython Streamer互換APIでラップ
    
    gui.pyが期待するプロパティ/メソッド:
    - start() / stop()
    - seek(time_seconds)
    - is_vod / is_live
    - duration / playback_time
    - width / height / detected_fps
    - latest_frame_bgr
    - frame_lock
    - コールバック: log_cb, stop_cb, init_ok_cb
    """
    
    def __init__(
        self,
        video_url: str,
        sender_name: str,
        max_resolution: Optional[tuple[int, int]] = None,
        manual_resolution: Optional[tuple[int, int]] = None,
        loop_vod: bool = False,
        verbose: bool = True,
        log_cb: Optional[Callable[[str], None]] = None,
        stop_cb: Optional[Callable[[], None]] = None,
        init_ok_cb: Optional[Callable[[], None]] = None,
        external_spout_sender: Any = None,  # C++ DLLでは無視
        pre_resolved_url: Optional[str] = None,  # 事前解決済みの直接ストリームURL
    ):
        """
        NativeStreamerWrapperを初期化
        
        Args:
            video_url: 動画ファイルパスまたはURL（元のURL、表示用に保持）
            sender_name: Spout Sender名
            max_resolution: 最大解像度 (width, height)、オプション
            manual_resolution: 手動解像度 (width, height)、オプション
            loop_vod: VODをループ再生するか
            verbose: 詳細ログを出力するか
            log_cb: ログコールバック関数
            stop_cb: 停止時コールバック関数
            init_ok_cb: 初期化成功時コールバック関数
            external_spout_sender: 外部SpoutSender（C++ DLLでは無視）
            pre_resolved_url: 事前にyt-dlpで解決済みの直接ストリームURL（オプション）
        """
        self.video_url = video_url
        self._pre_resolved_url = pre_resolved_url  # 事前解決済みURL
        self.sender_name = sender_name
        self.loop_vod = loop_vod
        self._verbose = verbose
        self._log_cb = log_cb
        self._stop_cb = stop_cb
        self._init_ok_cb = init_ok_cb
        
        # 解像度制限（シームレス切り替え時の解像度引継ぎ用）
        self._max_resolution = max_resolution
        self._manual_resolution = manual_resolution
        self._output_width = 0  # 出力解像度（リサイズ後）
        self._output_height = 0
        
        # 外部SpoutSender（C++では使わないが互換性のため保持）
        self.spout = external_spout_sender
        self.owns_spout = (external_spout_sender is None)
        
        # ローカルファイル判定
        import os
        self.is_local_file = os.path.exists(video_url) and os.path.isfile(video_url)
        
        # C++ DLLインスタンス
        self._native: Optional[YtdlpSpoutNative] = None
        
        # 状態プロパティ
        self._is_vod = True  # デフォルトVOD
        self._is_live = False
        self._duration = 0.0
        self._width = 0
        self._height = 0
        self._detected_fps = 30.0
        
        # フレームデータ
        self.latest_frame_bgr: Optional[Any] = None  # np.ndarray
        self.frame_lock = threading.Lock()
        
        # PTS同期用（シームレス切り替え対応）
        self.current_frame_pts: float = 0.0  # 現在フレームのPTS（秒）
        self.pts_lock = threading.Lock()  # PTSアクセス用ロック
        
        # Spout制御（シームレス切り替え対応）
        self.spout_enabled: bool = True
        self.spout_enabled_lock = threading.Lock()
        
        # 一時停止制御（シームレス切り替え対応）
        self.pause_request_pts: Optional[float] = None
        self.is_paused = False
        self.pause_lock = threading.Lock()
        
        # ペンディングシーク（DLL初期化前に呼ばれたシークを保持）
        self._pending_seek: Optional[float] = None
        self._pending_seek_lock = threading.Lock()
        
        # DLL初期化完了フラグ
        self._dll_ready = threading.Event()
        
        # 再生スレッド
        self._thread: Optional[threading.Thread] = None
        self._stop_event = threading.Event()
        self._started = False
    
    @property
    def is_vod(self) -> bool:
        """VOD動画かどうか"""
        return self._is_vod
    
    @property
    def is_live(self) -> bool:
        """ライブストリームかどうか"""
        return self._is_live
    
    @property
    def duration(self) -> float:
        """総再生時間（秒）"""
        if self._native:
            return self._native.duration
        return self._duration
    
    @property
    def playback_time(self) -> float:
        """現在の再生位置（秒）"""
        if self._native:
            return self._native.position
        return 0.0
    
    @property
    def width(self) -> int:
        """出力フレームの幅（リサイズ後）"""
        return self._output_width if self._output_width > 0 else self._width
    
    @property
    def height(self) -> int:
        """出力フレームの高さ（リサイズ後）"""
        return self._output_height if self._output_height > 0 else self._height
    
    @property
    def detected_fps(self) -> float:
        """検出されたFPS"""
        return self._detected_fps
    
    @property
    def download_progress(self) -> float:
        """ダウンロード進捗（0.0〜1.0）"""
        if self._native:
            return self._native.download_progress
        return 0.0
    
    @property
    def is_fully_cached(self) -> bool:
        """全チャンクがキャッシュ済みか"""
        if self._native:
            return self._native.is_fully_cached
        return False
    
    @property
    def bandwidth(self) -> float:
        """推定帯域幅（bytes/sec）"""
        if self._native:
            return self._native.bandwidth
        return 0.0
    
    def log(self, msg: str):
        """ログメッセージを出力"""
        if self._log_cb:
            self._log_cb(msg)
    
    # === シームレス切り替え対応メソッド ===
    
    def set_spout_enabled(self, enabled: bool):
        """Spout送信の有効/無効を切り替える"""
        with self.spout_enabled_lock:
            self.spout_enabled = enabled
        self.log(f"[SPOUT] 送信{'有効' if enabled else '無効'}化")

    def pause_at_pts(self, pts: float):
        """指定したPTSで一時停止するよう予約"""
        with self.pause_lock:
            self.pause_request_pts = pts
            self.is_paused = False
        self.log(f"[SYNC] PTS {pts:.3f}秒での一時停止を予約")

    def resume(self):
        """一時停止を解除して再開"""
        with self.pause_lock:
            was_paused = self.is_paused
            self.is_paused = False
            self.pause_request_pts = None
            self.log(f"[SYNC] resume(): was_paused={was_paused}, is_paused設定=False")
        if was_paused:
            # 再開時に現在位置にシークしてバッファをフラッシュ（バースト防止）
            try:
                native = self._native
                if native is not None:
                    current_pos = native.position
                    native.seek(current_pos)
                    self.log(f"[SYNC] バッファフラッシュ: シーク位置={current_pos:.3f}秒")
            except Exception as e:
                self.log(f"[SYNC] バッファフラッシュ失敗: {e}")
            self.log("[SYNC] 再生再開")

    def wait_for_pts(self, target_pts: float, timeout: float = 15.0) -> bool:
        """
        指定したPTSに到達するまで待機する。
        
        Args:
            target_pts: 待機対象のPTS（秒）
            timeout: タイムアウト秒数（デフォルト15秒）
        
        Returns:
            True: 目標PTSに到達
            False: タイムアウトまたは停止
        """
        import time as time_module
        start_time = time_module.perf_counter()
        
        # DLL初期化を待機（タイムアウト時間の一部を使用）
        if not self._dll_ready.wait(timeout=min(timeout, 10.0)):
            self.log("[SYNC] PTS待機失敗: DLL初期化タイムアウト")
            return False
        
        # 残り時間を計算
        elapsed = time_module.perf_counter() - start_time
        remaining_timeout = timeout - elapsed
        if remaining_timeout <= 0:
            self.log("[SYNC] PTS待機タイムアウト: DLL初期化後に時間切れ")
            return False
        
        frame_interval = 1.0 / max(self._detected_fps, 1)
        tolerance = frame_interval / 2
        
        self.log(f"[SYNC] PTS待機開始: 目標={target_pts:.3f}秒, 許容差={tolerance:.4f}秒")
        
        # ループ用の開始時刻をリセット（DLL初期化待機後から計測）
        loop_start_time = time_module.perf_counter()
        
        while not self._stop_event.is_set():
            elapsed = time_module.perf_counter() - loop_start_time
            if elapsed > remaining_timeout:
                self.log(f"[SYNC] PTS待機タイムアウト: {elapsed:.1f}秒経過")
                return False
            
            with self.pts_lock:
                current = self.current_frame_pts
            
            if current >= target_pts - tolerance:
                self.log(f"[SYNC] PTS到達: 現在={current:.3f}秒, 目標={target_pts:.3f}秒")
                return True
            
            time_module.sleep(frame_interval / 4)
        
        self.log("[SYNC] PTS待機中断: ストリーマー停止")
        return False

    def find_best_match_pts(self, target_img_bgr, center_pts: float, search_range: float = 3.0) -> tuple:
        """
        指定された画像に最も近いフレームを周辺から探索し、そのPTSを返す。
        
        Args:
            target_img_bgr: ターゲット画像 (BGR numpy array)
            center_pts: 探索の中心となるPTS (秒)
            search_range: 前後の探索範囲 (秒)
            
        Returns:
            (best_pts, min_score, best_frame_img)
        """
        if not self._is_vod or not self.is_local_file or target_img_bgr is None:
            return center_pts, float('inf'), None
            
        try:
            import av
            import cv2
            import numpy as np
            
            # 比較用にターゲット画像を縮小・グレースケール化
            h, w = target_img_bgr.shape[:2]
            chk_w, chk_h = 64, 36
            target_small = cv2.resize(target_img_bgr, (chk_w, chk_h))
            target_gray = cv2.cvtColor(target_small, cv2.COLOR_BGR2GRAY)
            
            best_score = float('inf')
            best_pts = center_pts
            best_img = None
            
            # 一時的にコンテナを開く
            with av.open(self.video_url) as container:
                video_stream = next((s for s in container.streams if s.type == 'video'), None)
                if not video_stream:
                    return center_pts, float('inf'), None
                
                start_time = max(0, center_pts - (search_range / 2))
                end_time = center_pts + (search_range / 2)
                
                pts = int(start_time / float(video_stream.time_base))
                container.seek(pts, any_frame=False, backward=True, stream=video_stream)
                
                for frame in container.decode(video_stream):
                    if frame.time is None: 
                        continue
                    if frame.time > end_time: 
                        break
                    if frame.time < start_time: 
                        continue
                    
                    img = frame.to_ndarray(format='bgr24')
                    small = cv2.resize(img, (chk_w, chk_h))
                    gray = cv2.cvtColor(small, cv2.COLOR_BGR2GRAY)
                    
                    diff = cv2.absdiff(target_gray, gray)
                    score = np.mean(diff)
                    
                    if score < best_score:
                        best_score = score
                        best_pts = frame.time
                        best_img = img
            
            return best_pts, best_score, best_img
            
        except Exception as e:
            self.log(f"画像マッチング探索エラー: {e}")
            return center_pts, float('inf'), None

    def start(self):
        """再生を開始"""
        if self._started:
            return
        
        self._stop_event.clear()
        self._thread = threading.Thread(target=self._run, daemon=True)
        self._thread.start()
        self._started = True
    
    def stop(self):
        """再生を停止"""
        self._stop_event.set()
        
        # 一時停止を解除（スレッドが終了できるように）
        with self.pause_lock:
            self.is_paused = False
            self.pause_request_pts = None
        
        # スレッドの終了を待機（_nativeはスレッド内で使われるため、先にスレッドを止める）
        if self._thread:
            self._thread.join(timeout=3)
            if self._thread.is_alive():
                # タイムアウトした場合でも続行（デーモンスレッドなのでプロセス終了時に消える）
                pass
            self._thread = None
        
        # スレッドが完全に終了してから_nativeを停止・解放
        if self._native:
            try:
                self._native.stop()
            except Exception:
                pass
            self._native = None  # 参照を解放
        
        # SpoutSender解放（自前で作成した場合のみ）
        if self.owns_spout and self.spout is not None:
            try:
                self.spout.releaseSender()
            except Exception:
                pass
            self.spout = None
        
        self._started = False
    
    def seek(self, time_seconds: float):
        """
        指定時刻にシーク
        
        Args:
            time_seconds: シーク先の時刻（秒）
        """
        if self._native and self._is_vod:
            # DLLが初期化済み: 直接シーク
            result = self._native.seek(time_seconds)
            # 注意: PTSは楽観的に更新しない（process_frame()で実際の値が取得される）
            self.log(f"[Native] シーク実行: {time_seconds:.3f}秒 (result={result})")
        else:
            # DLLが未初期化: ペンディングシークに保持
            with self._pending_seek_lock:
                self._pending_seek = time_seconds
            self.log(f"[Native] シーク予約: {time_seconds:.3f}秒 (DLL初期化待ち)")
    
    def wait_for_dll_ready(self, timeout: float = 10.0) -> bool:
        """
        DLLの初期化完了を待機
        
        Args:
            timeout: タイムアウト秒数
            
        Returns:
            True: 初期化完了, False: タイムアウト
        """
        return self._dll_ready.wait(timeout=timeout)
    
    def _run(self):
        """再生スレッドのメイン処理"""
        try:
            # スレッド内でDLLディレクトリを設定（Windowsのスレッドセーフティ対策）
            import os
            from pathlib import Path
            dll_search_paths = [
                Path(__file__).parent / "ytdlpspout.dll",
                Path(__file__).parent.parent / "cpp" / "build" / "vs2022" / "bin" / "Release",
                Path(__file__).parent.parent / "cpp" / "build" / "vs2022" / "bin" / "Debug",
            ]
            for dll_dir in dll_search_paths:
                if dll_dir.is_dir():
                    dll_dir_str = str(dll_dir.resolve())
                    if dll_dir_str not in os.environ.get("PATH", ""):
                        os.environ["PATH"] = dll_dir_str + os.pathsep + os.environ.get("PATH", "")
                    if hasattr(os, 'add_dll_directory'):
                        try:
                            os.add_dll_directory(dll_dir_str)
                        except (OSError, AttributeError):
                            pass
                    break
            
            self._native = YtdlpSpoutNative()
            self.log(f"[Native] C++ DLLバックエンドを初期化中...")
            
            # 事前解決済みURLがある場合はそれを使用
            # （Python側でyt-dlp解決済みの直接ストリームURL）
            input_url = self._pre_resolved_url if self._pre_resolved_url else self.video_url
            if self._pre_resolved_url:
                self.log(f"[Native] 事前解決済みURLを使用: 直接ストリームURL")
            
            self._native.start(
                input_file=input_url,
                sender_name=self.sender_name,
                loop=self.loop_vod,
                verbose=self._verbose
            )
            
            # 動画情報取得
            info = self._native.get_video_info()
            if info:
                self._width = info["width"]
                self._height = info["height"]
                self._detected_fps = info["fps"]
                self._duration = info["duration"]
                self._is_vod = self._duration > 0
                self._is_live = self._duration <= 0
            
            # 出力解像度を決定（リサイズが必要な場合）
            self._output_width = self._width
            self._output_height = self._height
            
            if self._manual_resolution:
                # 手動解像度が指定された場合（シームレス切り替え時の解像度引継ぎ）
                self._output_width, self._output_height = self._manual_resolution
                self.log(f"[Native] 解像度引継ぎ: {self._width}x{self._height} -> {self._output_width}x{self._output_height}")
            elif self._max_resolution:
                # 最大解像度制限
                max_w, max_h = self._max_resolution
                if self._width > max_w or self._height > max_h:
                    scale = min(max_w / self._width, max_h / self._height)
                    self._output_width = int(self._width * scale)
                    self._output_height = int(self._height * scale)
                    self.log(f"[Native] 解像度制限: {self._width}x{self._height} -> {self._output_width}x{self._output_height}")
            
            self.log(f"[Native] 再生開始: {self._width}x{self._height} @ {self._detected_fps}fps (出力: {self._output_width}x{self._output_height})")
            
            # C++ DLLがSpout送信を担当するため、Python側のSpoutSender初期化は不要
            # （C++ DLLは独自のSpoutSenderを使用）
            
            # ペンディングシークを適用（DLL初期化前に呼ばれたシーク）
            with self._pending_seek_lock:
                pending_seek = self._pending_seek
                self._pending_seek = None
            
            if pending_seek is not None and self._is_vod:
                self.log(f"[Native] ペンディングシーク適用: {pending_seek:.3f}秒")
                seek_result = self._native.seek(pending_seek)
                # Seek後、C++ DLL側でcurrentTimeが更新されるので、positionで取得可能
                # process_frame()は呼ばない（Spout送信を防ぐため）
                actual_pos = self._native.position
                self.log(f"[Native] シーク結果: success={seek_result}, 実際位置={actual_pos:.3f}秒")
                with self.pts_lock:
                    self.current_frame_pts = actual_pos  # 実際の位置を設定
            
            # DLL初期化完了を通知
            self._dll_ready.set()
            
            # Spout送信状態フラグ（初回送信ログ用）
            spout_first_frame_sent = False
            spout_error_logged = False
            
            # FPS計測用
            fps_frame_count = 0
            fps_start_time = time.perf_counter()
            
            # FPS制限用タイマー
            frame_interval = 1.0 / self._detected_fps if self._detected_fps > 0 else 1.0 / 30.0
            last_frame_time = time.perf_counter()
            
            if self._init_ok_cb:
                self._init_ok_cb()
            
            # フレーム処理ループ
            while not self._stop_event.is_set():
                # _nativeのローカルコピーを取得（スレッドセーフ）
                native = self._native
                if native is None:
                    break
                
                try:
                    is_playing = native.is_playing
                except Exception:
                    break
                
                if not is_playing:
                    break
                
                # 一時停止チェック（シームレス切り替え対応）
                with self.pause_lock:
                    paused = self.is_paused
                
                if paused:
                    # 一時停止中はタイマーをリセット（再開時のバースト防止）
                    last_frame_time = time.perf_counter()
                    # 一時停止中もPTSは更新（wait_for_ptsが動作するため）
                    try:
                        current_pos = native.position
                        with self.pts_lock:
                            self.current_frame_pts = current_pos
                    except Exception:
                        pass
                    time.sleep(0.01)
                    continue

                try:
                    if not native.process_frame():
                        break
                except Exception as e:
                    self.log(f"[Native] process_frame例外: {e}")
                    break
                
                # PTS更新（シームレス切り替え対応）
                current_pos = 0.0
                try:
                    current_pos = native.position
                    with self.pts_lock:
                        self.current_frame_pts = current_pos
                except Exception:
                    pass
                
                # 一時停止予約チェック
                with self.pause_lock:
                    req_pts = self.pause_request_pts
                    if req_pts is not None:
                        if current_pos >= req_pts:
                            self.is_paused = True
                            self.pause_request_pts = None  # 予約をクリア
                            self.log(f"[SYNC] 予約PTSで一時停止: {current_pos:.3f}秒 (予約クリア)")
                            continue
                
                # フレームデータ取得（GUI表示用のみ、Spout送信はC++ DLLが担当）
                try:
                    t_start = time.perf_counter()
                    frame_bgra = native.get_current_frame()
                    t_get = time.perf_counter()
                    
                    if frame_bgra is not None and HAS_NUMPY:
                        # GUI表示用にフレームを保持
                        import cv2
                        frame_bgr = cv2.cvtColor(frame_bgra, cv2.COLOR_BGRA2BGR)
                        
                        # 解像度制限がある場合はリサイズ（GUI表示用）
                        if self._output_width != self._width or self._output_height != self._height:
                            frame_bgr = cv2.resize(frame_bgr, (self._output_width, self._output_height))
                        
                        with self.frame_lock:
                            self.latest_frame_bgr = frame_bgr
                        
                        # C++ DLLがSpout送信を担当（Spout有効ビルド）
                        # Python側のSpoutGL送信は不要
                        if not spout_first_frame_sent:
                            spout_first_frame_sent = True
                            self.log(f"[Spout] C++ DLLからフレーム送信中: {self._width}x{self._height}")
                        
                        # FPS計測（10秒ごとにログ出力）
                        fps_frame_count += 1
                        elapsed = time.perf_counter() - fps_start_time
                        if elapsed >= 10.0:
                            actual_fps = fps_frame_count / elapsed
                            get_ms = (t_get - t_start) * 1000
                            self.log(f"[Native] 実測FPS: {actual_fps:.1f} (目標: {self._detected_fps:.1f}) | get:{get_ms:.1f}ms")
                            fps_frame_count = 0
                            fps_start_time = time.perf_counter()
                except Exception:
                    pass
                
                # FPS制限：C++ DLL側でフレームレート制御しているので
                # Python側は急激なバースト（2倍以上）のみ防止
                current_time = time.perf_counter()
                time_since_last = current_time - last_frame_time
                min_interval = frame_interval * 0.5  # 目標FPSの2倍までは許容
                if time_since_last < min_interval:
                    time.sleep(min_interval - time_since_last)
                last_frame_time = time.perf_counter()
            
            self.log("[Native] 再生終了")
            
        except Exception as e:
            import traceback
            self.log(f"[Native] エラー: {e}")
            self.log(f"[Native] トレースバック:\n{traceback.format_exc()}")
        finally:
            if self._stop_cb:
                self._stop_cb()
