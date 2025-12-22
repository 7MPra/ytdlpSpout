"""フォント検出ユーティリティ"""

import tkinter.font as tkfont

from .constants import PREFERRED_JAPANESE_FONTS, PREFERRED_MONOSPACE_FONTS


def get_best_japanese_font() -> str:
    """システムで利用可能な最適な日本語フォントを取得"""
    available_fonts = tkfont.families()
    for font in PREFERRED_JAPANESE_FONTS:
        if font in available_fonts:
            return font
    return "system"


def get_best_monospace_font() -> str:
    """システムで利用可能な最適な等幅フォントを取得（日本語対応優先）"""
    available_fonts = tkfont.families()
    for font in PREFERRED_MONOSPACE_FONTS:
        if font in available_fonts:
            return font
    return "monospace"
