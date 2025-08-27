# ytdlpSpout

YouTube 動画を Spout 経由でリアルタイム配信する Python ツール

## 概要

ytdlpSpout は、YouTube 動画をダウンロードして Spout（Windows 用のリアルタイム映像共有システム）経由で他のアプリケーションに配信するツールです。CLI と GUI の両方のインターフェースを提供します。

## 機能

- YouTube 動画のリアルタイムストリーミング
- Spout 経由での映像配信
- CLI（コマンドライン）インターフェース
- GUI（グラフィカル）インターフェース
- 日本語対応

## 必要な環境

- Windows 10/11
- Python 3.11 以上
- FFmpeg（システム PATH に追加）

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
uv add yt-dlp opencv-python customtkinter pillow numpy SpoutGL
```

### 4. FFmpeg のインストール

[FFmpeg 公式サイト](https://ffmpeg.org/download.html)からダウンロードし、システム PATH に追加してください。

## 使用方法

### CLI モード

```bash
python main.py [YouTube URL]
```

例：

```bash
python main.py https://www.youtube.com/watch?v=dQw4w9WgXcQ
```

### GUI モード

```bash
python gui.py
```

GUI を起動し、YouTube URL を入力して配信を開始できます。

## ビルド

実行可能ファイルを作成する場合：

```bash
python build_distribution.py
```

**注意**: ビルドされた実行可能ファイルは、FFmpeg の依存関係が組み込まれているため、別途 FFmpeg をインストールする必要がありません。

## ライセンス

MIT License - 詳細は[LICENSE](LICENSE)ファイルを参照してください。

## 貢献

プルリクエストやイシューの報告を歓迎します。

## 注意事項

- YouTube 利用規約を遵守してください
- 著作権で保護されたコンテンツの使用には注意してください
- Spout は主に Windows 環境で動作します
