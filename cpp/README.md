# C++ Backend 開発ガイド

## 概要

ytdlpSpout C++ バックエンドは、高性能な動画再生とSpoutテクスチャ共有を実現するネイティブ実装です。

## ディレクトリ構造

```
cpp/
├── CMakeLists.txt           # ルートCMake設定
├── CMakePresets.json        # VS2022プリセット
├── vcpkg.json               # 依存パッケージ宣言
├── vcpkg-configuration.json # vcpkgレジストリ設定
│
├── src/                     # ソースファイル
│   ├── main.cpp             # CLIエントリーポイント
│   ├── Application.cpp/h    # アプリケーションロジック
│   │
│   ├── bindings/            # FFIバインディング
│   │   └── c_api.cpp        # C言語API実装
│   │
│   ├── decoder/             # FFmpegデコーダー
│   │   ├── VideoDecoder.cpp/h
│   │   ├── HWAccelContext.cpp/h
│   │   └── FrameConverter.cpp/h
│   │
│   ├── graphics/            # DirectX/Spout
│   │   ├── D3D11Context.cpp/h
│   │   ├── SpoutSender.cpp/h
│   │   └── TexturePool.cpp/h
│   │
│   ├── player/              # プレイヤー
│   │   ├── VideoPlayer.cpp/h
│   │   └── FrameTimer.cpp/h
│   │
│   ├── io/                  # I/O（ストリーミング）
│   │   ├── HttpClient.cpp/h
│   │   ├── CustomIOContext.cpp/h
│   │   ├── ChunkDownloader.cpp/h
│   │   ├── SparseFileCache.cpp/h
│   │   └── PrefetchScheduler.cpp/h
│   │
│   ├── ytdlp/               # yt-dlp連携
│   │   └── YtDlpResolver.cpp/h
│   │
│   ├── audio/               # 音声処理
│   │   ├── AudioDecoder.cpp/h
│   │   ├── BeatMap.cpp/h
│   │   ├── BeatDetector.cpp/h
│   │   └── BeatMapGenerator.cpp/h
│   │
│   └── utils/               # ユーティリティ
│       ├── Logger.cpp/h
│       ├── ComPtr.h
│       └── ErrorHandling.h
│
├── include/                 # 公開ヘッダー
│   └── ytdlpspout/
│       └── ytdlpspout.h     # DLL API
│
├── shaders/                 # HLSLシェーダー
│   ├── CMakeLists.txt
│   ├── NV12ToRGBA_PS.hlsl
│   └── Passthrough_VS.hlsl
│
└── tests/                   # テスト
    ├── CMakeLists.txt
    ├── test_decoder.cpp
    ├── test_http_client.cpp
    ├── test_sparse_file_cache.cpp
    ├── test_chunk_downloader.cpp
    ├── test_custom_io_context.cpp
    ├── test_ytdlp_resolver.cpp
    ├── test_audio_decoder.cpp
    ├── test_beat_map.cpp
    ├── test_beat_detector.cpp
    ├── test_beat_map_generator.cpp
    ├── test_video_player_beat.cpp
    └── test_c_api.cpp

python/                      # Pythonバインディング
├── __init__.py
├── ytdlpspout_native.py     # ctypes FFIラッパー
└── test_ytdlpspout_native.py
```

## 技術スタック

| カテゴリ | 技術 | 用途 |
|---------|------|------|
| 言語 | C++20 | 最新の言語機能 |
| グラフィックス | DirectX 11 | テクスチャ管理・GPU処理 |
| テクスチャ共有 | Spout2 SDK | フレーム送信 |
| 動画処理 | FFmpeg (libav*) | デコード |
| HTTP | libcurl | ストリーミング再生 |
| JSON | nlohmann/json | yt-dlpメタデータ解析 |
| ログ | spdlog | ログ出力 |
| CLI | CLI11 | 引数解析 |

## ビルド手順

### 前提条件

1. **Visual Studio 2022** (C++デスクトップ開発ワークロード)
2. **CMake 3.25以上**
3. **Windows SDK 10.0.19041.0以上**
4. **vcpkg** パッケージマネージャー

### vcpkg セットアップ

```powershell
# vcpkgのクローン（未インストールの場合）
git clone https://github.com/microsoft/vcpkg.git C:\vcpkg
cd C:\vcpkg
.\bootstrap-vcpkg.bat

# 環境変数設定
$env:VCPKG_ROOT = "C:\vcpkg"
[Environment]::SetEnvironmentVariable("VCPKG_ROOT", "C:\vcpkg", "User")

# Visual Studioとの統合
.\vcpkg integrate install
```

### ビルド実行

```powershell
cd F:\ytdlpSpout\cpp

# Debugビルド
cmake --preset windows-x64-debug
cmake --build build/windows-x64-debug

# Releaseビルド
cmake --preset windows-x64-release
cmake --build build/windows-x64-release

# Visual Studioソリューション生成
cmake --preset windows-vs2022
# build/vs2022/ytdlpSpoutCpp.sln を開く
```

### 実行

```powershell
# ローカルファイル再生
.\build\vs2022\bin\Release\ytdlpSpoutCLI.exe video.mp4

# オプション付き
.\build\vs2022\bin\Release\ytdlpSpoutCLI.exe video.mp4 --name "MyVideo" --loop -v

# HTTP URL再生
.\build\vs2022\bin\Release\ytdlpSpoutCLI.exe "https://example.com/video.mp4"

# YouTube等のURL再生（yt-dlp連携）
.\build\vs2022\bin\Release\ytdlpSpoutCLI.exe "https://www.youtube.com/watch?v=xxx" -f 1080p
```

## コマンドラインオプション

```
Usage: ytdlpSpoutCLI <input> [options]

Positional arguments:
  input                   Input video file path or URL

Options:
  -n, --name <name>       Spout sender name (default: ytdlpSpout)
  -l, --loop              Loop playback
  -v, --verbose           Enable verbose logging
  -f, --format <fmt>      Format selection (e.g., "best", "1080p", "720p")
  --height <pixels>       Preferred video height (default: 1080)
  --ytdlp-path <path>     Path to yt-dlp executable
  --no-hwaccel            Disable hardware acceleration
  --no-progress           Disable progress display
  --analyze-bpm           Analyze BPM before playback
  --load-beatmap <path>   Load beatmap from file
  --save-beatmap <path>   Save analyzed beatmap to file
  -h, --help              Show help
  --version               Show version
```

## DLLビルドとPython統合

### DLLのビルド

```powershell
cd F:\ytdlpSpout\cpp

# DLLビルド
cmake --preset windows-vs2022 -DBUILD_SHARED_LIB=ON
cmake --build build/vs2022 --config Release --target ytdlpspout

# 出力: build/vs2022/bin/Release/ytdlpspout.dll
```

### Python FFIバインディング

```python
from python.ytdlpspout_native import YtdlpSpoutNative

# プレイヤー作成
player = YtdlpSpoutNative()

# 再生開始
player.start("video.mp4", sender_name="MySpout", loop=True)

# フレームループ
while player.is_playing:
    if not player.process_frame():
        break

player.stop()
```

### コンテキストマネージャー

```python
with YtdlpSpoutNative() as player:
    player.start("video.mp4")
    while player.is_playing:
        player.process_frame()
# 自動的にstop()が呼ばれる
```

詳細は [llm/cpp-dll-python-ffi.md](../llm/cpp-dll-python-ffi.md) を参照。

## 入力ソースの種類

| 種類 | 例 | 処理方法 |
|------|-----|---------|
| ローカルファイル | `video.mp4` | 直接再生 |
| HTTP URL | `https://example.com/video.mp4` | CustomIOContext経由 |
| YouTube等 | `https://youtube.com/watch?v=xxx` | yt-dlp→ストリームURL取得 |

### サポートするプラットフォーム（yt-dlp連携）

- YouTube / YouTube Music
- Twitch
- Vimeo
- ニコニコ動画
- Twitter/X
- Dailymotion
- Bilibili
- その他yt-dlp対応サイト

## アーキテクチャ

### データフロー（Zero-Copyパス）

```
┌─────────────────────┐
│   Video File        │
└─────────┬───────────┘
          │ av_read_frame
          ▼
┌─────────────────────┐
│   VideoDecoder      │  FFmpeg + D3D11VA
│   (HW Accelerated)  │
└─────────┬───────────┘
          │ AVFrame (GPU Texture - NV12)
          ▼
┌─────────────────────┐
│   FrameConverter    │  HLSL Shader
│   NV12 → RGBA       │
└─────────┬───────────┘
          │ ID3D11Texture2D (RGBA)
          ▼
┌─────────────────────┐
│   SpoutSender       │  Texture Sharing
│   (SpoutDX)         │
└─────────┬───────────┘
          │ Shared Texture Handle
          ▼
┌─────────────────────┐
│   Spout Receiver    │  OBS, Resolume, etc.
└─────────────────────┘
```

### ハードウェアアクセラレーション

- **D3D11VA**: DirectX 11 Video Acceleration
- デコード出力はGPUメモリ上のNV12テクスチャ
- シェーダーでNV12→RGBA変換（CPUコピーなし）

## テスト

```powershell
# テストビルド
cmake --build build/windows-x64-debug --target test_decoder

# テスト実行
cd build/windows-x64-debug
ctest --output-on-failure
```

## トラブルシューティング

### FFmpegが見つからない

vcpkgでFFmpegをインストール:
```powershell
vcpkg install ffmpeg:x64-windows
```

### Spout2が見つからない

vcpkgでSpout2をインストール:
```powershell
vcpkg install spout2:x64-windows
```

### シェーダーコンパイルエラー

Windows SDKがインストールされていることを確認:
- Visual Studio Installerで「Windows 10 SDK」を追加

### ハードウェアデコードが動作しない

1. グラフィックスドライバを最新版に更新
2. `--no-hwaccel` オプションでソフトウェアデコードを試行
