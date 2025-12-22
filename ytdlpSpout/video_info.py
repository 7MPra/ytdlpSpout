"""動画情報取得・解析ユーティリティ"""

from typing import Any, Dict, Optional, Tuple


def detect_fps(info: Dict[str, Any]) -> Optional[int]:
    """yt-dlpの情報からFPSを検出する"""
    rf = info.get("requested_formats")
    if isinstance(rf, list):
        for f in rf:
            if f and f.get("vcodec") not in (None, "none") and f.get("fps"):
                return int(round(f["fps"]))
    if info.get("fps"):
        return int(round(info["fps"]))
    fmts = info.get("formats")
    if isinstance(fmts, list):
        fps_vals = [f.get("fps") for f in fmts if f and f.get("fps")]
        if fps_vals:
            return int(round(max(fps_vals)))
    return None


def detect_max_resolution(info: Dict[str, Any]) -> Optional[Tuple[int, int]]:
    """yt-dlpの情報から最大解像度を検出する"""
    rf = info.get("requested_formats")
    if isinstance(rf, list):
        best = None
        for f in rf:
            if not f or f.get("vcodec") in (None, "none"):
                continue
            w, h = f.get("width"), f.get("height")
            if w and h:
                wh = (int(w), int(h))
                if best is None or (wh[0] * wh[1]) > (best[0] * best[1]):
                    best = wh
        if best:
            return best
    if info.get("width") and info.get("height"):
        return int(info["width"]), int(info["height"])
    fmts = info.get("formats")
    if isinstance(fmts, list):
        best = None
        for f in fmts:
            if not f or f.get("vcodec") in (None, "none"):
                continue
            w, h = f.get("width"), f.get("height")
            if w and h:
                wh = (int(w), int(h))
                if best is None or (wh[0] * wh[1]) > (best[0] * best[1]):
                    best = wh
        if best:
            return best
    return None
