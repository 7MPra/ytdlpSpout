#!/bin/bash

# macOS向けビルドスクリプト

# PyInstallerがインストールされているか確認
if ! command -v pyinstaller &> /dev/null
then
    echo "PyInstallerが見つかりません。インストールします..."
    pip3 install pyinstaller
fi

# syphon-pythonがインストールされているか確認
if ! python3 -c "import syphon" &> /dev/null
then
    echo "syphon-pythonが見つかりません。インストールします..."
    pip3 install syphon-python
fi

# yt-dlpがインストールされているか確認
if ! python3 -c "import yt_dlp" &> /dev/null
then
    echo "yt-dlpが見つかりません。インストールします..."
    pip3 install yt-dlp
fi

# ffmpegのダウンロードと配置
FFMPEG_DIR="./ffmpeg"
if [ ! -d "$FFMPEG_DIR" ]; then
    echo "ffmpegをダウンロードしています..."
    mkdir -p "$FFMPEG_DIR"
    curl -L "https://www.gyan.dev/ffmpeg/builds/ffmpeg-release-essentials.zip" -o "$FFMPEG_DIR/ffmpeg.zip"
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


