# ytdlpSpout

動画を Spout（Windows 用のリアルタイム映像共有システム）経由でリアルタイム配信するツールです。

## 概要

ytdlpSpout は、YouTube・ニコニコ動画をはじめとする yt-dlp 対応サイトの動画（および HLS ライブ配信・ローカルファイル）を、Spout 経由で OBS Studio などの受信側アプリに配信します。

アーキテクチャは大きく2層です:

- **C++ ネイティブバックエンド**（`cpp/`）: HLS のスライスローディング（セグメント単位の非同期ダウンロード・キャッシュ）、FFmpeg によるデコード、D3D11 によるハードウェアアクセラレーション（NV12→RGBA 変換含む）、Spout 送信を担う実行エンジン。`ytdlpspout.dll`（Python から ctypes 経由で利用）と、DLL を使わずに単体動作するネイティブ CLI `ytdlpSpoutCLI.exe` の両方をビルドします。
- **Python GUI / ラッパー層**（`gui.py`, `python/*.py`）: customtkinter 製 GUI、yt-dlp によるフォーマット解決（`python/ytdlp_resolver.py`）、C++ DLL の ctypes バインディング（`python/ytdlpspout_native.py`）と Streamer 互換ラッパー（`python/native_streamer_wrapper.py`）を提供します。C++ DLL が利用可能な場合は常にこちらが優先して使われ、DLL が見つからない場合のみレガシーの Python 実装（`ytdlpSpout/core.py`）にフォールバックします。

> **詳細な機能比較**: GUI版とCLI版（Python版・ネイティブ版）の機能差分については [FEATURES.md](FEATURES.md) を参照してください。

## 必要な環境

- Windows 10/11 64bit（Spout・D3D11 を利用するため Windows 専用）
- Python 3.11 以上
- C++ ネイティブバックエンドをソースからビルドする場合:
  - Visual Studio 2022（C++ ワークロード）
  - CMake 3.25 以上
  - [vcpkg](https://github.com/microsoft/vcpkg)（`VCPKG_ROOT` 環境変数を設定）
- FFmpeg（`bin/` 同梱、または C++ バックエンドをビルドした場合は vcpkg 経由で取得され別途インストール不要）

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

### 3. Python 依存関係のインストール

```bash
uv pip install -r requirements.txt
```

依存パッケージの一覧・バージョンは [requirements.txt](requirements.txt) を参照してください（`SpoutGL` は Windows 専用です）。

### 4. C++ ネイティブバックエンドのビルド

GUI・CLI いずれも、動画再生には `python/ytdlpspout.dll`（または `cpp/build/bin/Release/ytdlpspout.dll`）が必要です。リポジトリにはビルド済み DLL は同梱されていないため、**初回セットアップ時に必ずビルドしてください**:

```powershell
cd cpp
cmake -B build -G "Visual Studio 17 2022" -A x64 `
  -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"
cmake --build build --config Release
cd ..
```

ビルドが成功すると以下が生成されます:

- `cpp/build/bin/Release/ytdlpspout.dll` … Python から ctypes 経由で読み込まれるコアエンジン
- `cpp/build/bin/Release/ytdlpSpoutCLI.exe` … DLL/Python 不要で単体動作するネイティブ CLI
- 依存 DLL（`avcodec-61.dll`, `avformat-61.dll`, `avutil-59.dll`, `swscale-8.dll`, `swresample-5.dll`, `libcurl.dll`, `spdlog.dll`, `fmt.dll`, `zlib1.dll`, `Spout.dll` など）

Python 側（`python/ytdlpspout_native.py`）は `cpp/build/bin/Release/ytdlpspout.dll` を最優先で自動検出します。それ以外の場所（`python/ytdlpspout.dll` など）にコピーする場合は、**ビルドし直すたびに上書きコピーすること**（古い DLL が残っていると、ビルドが無い環境では気づかずに古い挙動へ無言でフォールバックします）。

```powershell
Copy-Item cpp\build\bin\Release\ytdlpspout.dll python\ytdlpspout.dll -Force
```

C++ テストの実行（推奨）:

```bash
ctest --test-dir cpp/build -C Release
```

### 5. Cookie が必要なサイトへの対応（任意）

ログインが必要な動画・年齢制限動画などは、Netscape 形式の Cookie ファイルを `data/cookies.txt` に配置すると yt-dlp がこれを利用します（`python/ytdlp_resolver.py` が `data/cookies.txt` を自動検出）。

## 使用方法

### GUI モード

```bash
python gui.py
```

customtkinter 製 GUI が起動し、URL を入力して配信を開始できます。

### CLI（ヘッドレス）モード（Python版）

`gui.py` は `--headless` を付けるとウィンドウを開かずコンソールのみで動作します（旧 `main.py` の機能を内包）:

```bash
python gui.py --headless "https://www.youtube.com/watch?v=dQw4w9WgXcQ"
python gui.py --headless -s "MySender" --max-width 1920 --max-height 1080 --loop "https://youtu.be/xxx"
python gui.py --headless --check-codecs
```

主なオプション: `-s/--sender`（Spout送信者名）, `-w/--width` `--height`（手動解像度）, `--max-width` `--max-height`（解像度上限）, `--loop`, `-v/--verbose`, `--no-limit`。詳細は `python gui.py --help` を参照。

### ネイティブ CLI（C++版、DLL/Python不要）

ビルド済みの `ytdlpSpoutCLI.exe` を直接実行できます。Python を経由しないため起動が速く、常駐サービス用途にも向いています:

```bash
cpp\build\bin\Release\ytdlpSpoutCLI.exe "https://www.youtube.com/watch?v=dQw4w9WgXcQ"
cpp\build\bin\Release\ytdlpSpoutCLI.exe -n "MySender" --loop "https://youtu.be/xxx"
cpp\build\bin\Release\ytdlpSpoutCLI.exe --help
```

## 対応サイト・形式

- YouTube（動画・ライブ配信）
- ニコニコ動画・ニコニコ生放送（詳細は [llm/specs/niconico-compatibility.md](llm/specs/niconico-compatibility.md)）
- その他 yt-dlp が対応する各種サイト全般
- HLS（m3u8）ストリーム（ライブ・VOD、C++ バックエンドによるスライスローディング対応）
- ローカル動画ファイル

## ビルド（配布パッケージ作成）

実行可能ファイル（配布用 exe 一式）を作成する場合：

```bash
python build_distribution.py
```

内部で C++ DLL のビルド、PyInstaller による exe 生成、C++ 依存 DLL・FFmpeg・yt-dlp の配置、ZIP 圧縮までを行います。詳細は `build_distribution.py` を参照してください。

## ライセンス

MIT License - 詳細は[LICENSE](LICENSE)ファイルを参照してください。

## 注意事項

- Spout は主に Windows 環境で動作します
- **ラップトップで Spout が表示されない・不安定な場合**: 電源オプションを「高パフォーマンス」にしてみるか、NVIDIA/AMD のコントロールパネルで本アプリを「高パフォーマンス GPU」に指定してください。本アプリは可能な場合に高パフォーマンス GPU を自動選択します（Windows 10 April 2018 Update 以降）。
