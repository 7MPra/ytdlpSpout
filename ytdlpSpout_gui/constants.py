"""GUI用定数定義"""


class UIConfig:
    """アプリケーションUI設定定数"""
    # ウィンドウ設定
    WINDOW_TITLE = "ytdlpSpout GUI"
    INITIAL_SIZE = "650x650"
    MIN_WIDTH = 600
    MIN_HEIGHT = 500
    
    # プレビュー・ログエリアの最小高さ
    PREVIEW_MIN_HEIGHT = 120
    LOG_MIN_HEIGHT = 80
    
    # フォントサイズ
    FONT_SIZE_NORMAL = 12
    FONT_SIZE_SMALL = 11
    FONT_SIZE_LARGE = 14
    FONT_SIZE_TITLE = 16
    
    # 色設定
    PROGRESS_BAR_COLOR = "#ff6b00"
    PROGRESS_FRAME_BG = "#2b2b2b"
    PROGRESS_FRAME_BORDER = "#404040"
    PANED_BG = "#212121"
    
    # ステータス色（シークバーノブ）
    SEEKBAR_KNOB_STANDBY = "#565b5e"       # 待機中（グレー）
    SEEKBAR_KNOB_DOWNLOADING = "#ff6b00"   # ダウンロード中/ストリーミング中（オレンジ）
    SEEKBAR_KNOB_LOCAL = "#1f6aa5"         # ローカル再生中（水色）
    
    # 更新間隔 (ms)
    PREVIEW_UPDATE_INTERVAL = 50  # 20fps
    LOG_PROCESS_INTERVAL = 25
    SASH_SETUP_DELAY = 100
    
    # パフォーマンス設定
    DEFAULT_MAX_RESOLUTION = (2560, 1440)  # 1440p制限
    
    # デフォルト値
    DEFAULT_MAX_WIDTH = "1920"
    DEFAULT_MAX_HEIGHT = "1080"
    
    # シークバー最小高さ
    SEEKBAR_MIN_HEIGHT = 35


# 優先フォントリスト
PREFERRED_JAPANESE_FONTS = (
    "Yu Gothic UI",      # Windows 10/11
    "BIZ UDPGothic",     # Windows 11
    "Noto Sans CJK JP",  # Google Noto
    "Noto Sans JP",      # Google Noto別名
    "Hiragino Sans",     # macOS
    "Yu Gothic",         # フォールバック
    "Meiryo UI",
    "Meiryo",
    "MS Gothic",
)

PREFERRED_MONOSPACE_FONTS = (
    "BIZ UDGothic",      # Windows 11日本語等幅
    "MS Gothic",         # 日本語対応等幅
    "Noto Sans Mono CJK JP",
    "Source Han Code JP",
    "Cascadia Code",     # Windows Terminal
    "Consolas",          # Windows標準
    "Courier New",
    "monospace",
)
