
#!/bin/bash

# venvが無ければuvで作成
if [ ! -d ".venv" ]; then
    echo ".venv ディレクトリが見つかりません。uvで仮想環境を作成します..."
    uv venv .venv
fi

source ./.venv/bin/activate

# macOS向けビルドスクリプト

# PyInstallerがインストールされているか確認
echo "ライブラリをインストールしています..."
uv pip install yt-dlp opencv-python customtkinter pillow numpy


# ffmpegのダウンロードと配置
FFMPEG_DIR="./ffmpeg"
if [ ! -d "$FFMPEG_DIR" ]; then
    echo "ffmpegをダウンロードしています..."
    mkdir -p "$FFMPEG_DIR"
    curl -L "https://evermeet.cx/ffmpeg/ffmpeg-8.0.zip" -o "$FFMPEG_DIR/ffmpeg.zip"
    unzip "$FFMPEG_DIR/ffmpeg.zip" -d "$FFMPEG_DIR"
    mv "$FFMPEG_DIR"/*/bin/* "$FFMPEG_DIR"/
    rm -rf "$FFMPEG_DIR"/ffmpeg.zip "$FFMPEG_DIR"/*/ # ダウンロードしたzipと展開したディレクトリを削除
fi

# ビルドディレクトリのクリーンアップ
rm -rf build dist

# PyInstallerでビルド
# --noconfirm: 既存のdist/buildディレクトリを上書き
# --onefile: 単一ファイルとしてビルド
# --windowed: コンソールウィンドウを表示しない (GUIアプリケーション向け)
# --add-binary: ffmpegをバンドル
# --hidden-import: syphon-pythonの隠れた依存関係を解決
# --collect-submodules: syphon-pythonのサブモジュールを収集

# macOSでは--windowedは使わない (CLIツールなので)
# --add-binary "$FFMPEG_DIR/ffmpeg":"."
# --add-binary "$FFMPEG_DIR/ffprobe":"."

# macOSではffmpegを直接バンドルするのではなく、PATHが通っていることを期待するか、
# アプリケーションバンドル内に配置する。
# 今回は簡単のため、PyInstallerの--add-binaryで配置する

# PyInstallerコマンド
pyinstaller \
    --noconfirm \
    --onefile \
    --name ytdlpSpout-mac \
    --add-binary "$FFMPEG_DIR/ffmpeg":"ffmpeg" \
    --add-binary "$FFMPEG_DIR/ffprobe":"ffprobe" \
    --hidden-import "syphon" \
    --hidden-import "syphon.syphon" \
    --collect-submodules "syphon" \
    main.py

echo "ビルドが完了しました。dist/ytdlpSpout-mac を確認してください。"


