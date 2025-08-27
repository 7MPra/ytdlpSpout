# ytdlpSpout

YouTube動画をSpout経由でリアルタイム配信するPythonツール

## 概要

ytdlpSpoutは、YouTube動画をダウンロードしてSpout（Windows用のリアルタイム映像共有システム）経由で他のアプリケーションに配信するツールです。CLIとGUIの両方のインターフェースを提供します。

## 機能

- YouTube動画のリアルタイムストリーミング
- Spout経由での映像配信
- CLI（コマンドライン）インターフェース
- GUI（グラフィカル）インターフェース
- 日本語対応

## 必要な環境

- Windows 10/11
- Python 3.11以上
- FFmpeg（システムPATHに追加）

## インストール

### 1. リポジトリのクローン

```bash
git clone https://github.com/7MPra/ytdlpSpout.git
cd ytdlpSpout
```

### 2. 仮想環境の作成

```bash
uv venv --python 3.11 .venv
.venv\Scripts\Activate.ps1
```

### 3. 依存関係のインストール

```bash
uv add yt-dlp opencv-python customtkinter pillow numpy
uv pip install git+https://github.com/spiraltechnica/spoutpy.git
```

### 4. FFmpegのインストール

[FFmpeg公式サイト](https://ffmpeg.org/download.html)からダウンロードし、システムPATHに追加してください。

## 使用方法

### CLIモード

```bash
python main.py [YouTube URL]
```

例：
```bash
python main.py https://www.youtube.com/watch?v=dQw4w9WgXcQ
```

### GUIモード

```bash
python gui.py
```

GUIを起動し、YouTube URLを入力して配信を開始できます。

## ビルド

実行可能ファイルを作成する場合：

```bash
python build_distribution.py
```

**注意**: ビルドされた実行可能ファイルは、FFmpegの依存関係が組み込まれているため、別途FFmpegをインストールする必要がありません。

## ライセンス

MIT License - 詳細は[LICENSE](LICENSE)ファイルを参照してください。

## 貢献

プルリクエストやイシューの報告を歓迎します。

## 注意事項

- YouTube利用規約を遵守してください
- 著作権で保護されたコンテンツの使用には注意してください
- Spoutは主にWindows環境で動作します