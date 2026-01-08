# C++ Backend DLL & Python FFI Integration

このドキュメントはC++ DLLとPythonバインディングの開発ガイドを提供します。

## 概要

ytdlpspout C++バックエンドはDLL（`ytdlpspout.dll`）として提供され、Pythonからctypesを使用してFFI呼び出しできます。

## アーキテクチャ

```
┌─────────────────────────────────────────────────────────┐
│                    Python Application                    │
│  (GUI / CLI / Custom Scripts)                           │
└─────────────────────────┬───────────────────────────────┘
                          │ ctypes FFI
                          ▼
┌─────────────────────────────────────────────────────────┐
│              python/ytdlpspout_native.py                │
│  - YtdlpSpoutNative クラス                              │
│  - コールバック対応                                      │
│  - コンテキストマネージャー対応                           │
└─────────────────────────┬───────────────────────────────┘
                          │ C API
                          ▼
┌─────────────────────────────────────────────────────────┐
│                  ytdlpspout.dll                         │
│  - C言語互換API                                         │
│  - スレッドセーフ                                        │
│  - VideoPlayerラップ                                    │
└─────────────────────────────────────────────────────────┘
```

## C API リファレンス

### ハンドル管理

```c
// プレイヤー作成
YtdlpSpoutHandle ytdlpspout_create(void);

// プレイヤー破棄
void ytdlpspout_destroy(YtdlpSpoutHandle handle);
```

### 再生制御

```c
// 再生開始
int ytdlpspout_start(YtdlpSpoutHandle handle, const YtdlpSpoutConfig* config);

// 再生停止
void ytdlpspout_stop(YtdlpSpoutHandle handle);

// 一時停止/再開
void ytdlpspout_pause(YtdlpSpoutHandle handle);
void ytdlpspout_resume(YtdlpSpoutHandle handle);

// シーク
int ytdlpspout_seek(YtdlpSpoutHandle handle, double seconds);

// 1フレーム処理
int ytdlpspout_process_frame(YtdlpSpoutHandle handle);
```

### 状態取得

```c
// 再生状態
YtdlpSpoutState ytdlpspout_get_state(YtdlpSpoutHandle handle);
int ytdlpspout_is_playing(YtdlpSpoutHandle handle);

// 時間情報
double ytdlpspout_get_position(YtdlpSpoutHandle handle);
double ytdlpspout_get_duration(YtdlpSpoutHandle handle);

// 動画情報
int ytdlpspout_get_video_info(YtdlpSpoutHandle handle, YtdlpSpoutVideoInfo* info);
```

### ビート機能

```c
// ビートジャンプ
int ytdlpspout_jump_beats(YtdlpSpoutHandle handle, int beats, int forward);

// BPM取得
float ytdlpspout_get_bpm(YtdlpSpoutHandle handle);
```

### フレームデータ取得（GUI連携用）

```c
// フレームバッファサイズを取得
// @return width * height * 4 (BGRA形式)、動画未読込時は0
int ytdlpspout_get_frame_buffer_size(YtdlpSpoutHandle handle);

// 現在のフレームピクセルデータを取得
// @param buffer 出力バッファ（呼び出し側で確保）
// @param bufferSize バッファサイズ
// @param outWidth 出力: 幅
// @param outHeight 出力: 高さ
// @return 成功時0、失敗時は非0
int ytdlpspout_get_current_frame(
    YtdlpSpoutHandle handle,
    uint8_t* buffer,
    int bufferSize,
    int* outWidth,
    int* outHeight
);
```

**使用例（Python）**:
```python
import ctypes

# バッファサイズを取得
buffer_size = player.lib.ytdlpspout_get_frame_buffer_size(player.handle)
if buffer_size > 0:
    # バッファを確保
    buffer = (ctypes.c_uint8 * buffer_size)()
    width = ctypes.c_int()
    height = ctypes.c_int()
    
    # フレームデータを取得
    result = player.lib.ytdlpspout_get_current_frame(
        player.handle,
        buffer,
        buffer_size,
        ctypes.byref(width),
        ctypes.byref(height)
    )
    
    if result == 0:
        # BGRAピクセルデータをPIL Imageに変換など
        pass
```

### エラー処理

```c
// 最後のエラー（スレッドローカル）
const char* ytdlpspout_get_last_error(void);
```

## Python バインディング使用例

### 基本的な使用

```python
from python.ytdlpspout_native import YtdlpSpoutNative

# DLLパスを自動検索（または明示的に指定）
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

### コールバック

```python
player = YtdlpSpoutNative()

def on_progress(current, duration):
    print(f"Progress: {current:.1f} / {duration:.1f}s")

def on_error(message):
    print(f"Error: {message}")

def on_completion():
    print("Playback completed!")

player.set_progress_callback(on_progress)
player.set_error_callback(on_error)
player.set_completion_callback(on_completion)

player.start("video.mp4")
```

## ビルド手順

### DLLのビルド

```powershell
cd cpp
cmake --preset windows-vs2022 -DBUILD_SHARED_LIB=ON
cmake --build build/vs2022 --config Release --target ytdlpspout
```

出力: `cpp/build/vs2022/bin/Release/ytdlpspout.dll`

### テスト実行

```powershell
# C++ テスト
cmake --build build/vs2022 --config Release --target test_c_api
.\build\vs2022\bin\Release\test_c_api.exe

# Python テスト
python -m pytest python/test_ytdlpspout_native.py -v
```

## DLLの配布

DLLを配布する場合、以下のファイルが必要です：

1. `ytdlpspout.dll` - メインDLL
2. 依存DLL（FFmpeg、Spout2など）

### 依存DLLの確認

```powershell
dumpbin /dependents ytdlpspout.dll
```

### vcpkgからのDLLコピー

ビルド時に依存DLLは自動的に`bin/`ディレクトリにコピーされます。

## スレッドセーフティ

- `ytdlpspout_get_last_error()` はスレッドローカルストレージを使用
- 各ハンドルは独立したプレイヤーインスタンス
- コールバックはプレイヤーのワーカースレッドから呼び出される

## GUI統合 (NativeStreamerWrapper)

### 概要

`NativeStreamerWrapper`はC++ DLL (`YtdlpSpoutNative`) をPython Streamer互換APIでラップします。
これにより、gui.pyからの移行を最小限に抑えながらC++バックエンドを使用できます。

### アーキテクチャ

```
┌─────────────────────────────────────────────────────────┐
│                      gui.py                             │
│  - USE_NATIVE_BACKEND フラグで切り替え                   │
│  - _create_streamer() ファクトリメソッド                  │
└─────────────────────────┬───────────────────────────────┘
                          │
        ┌─────────────────┴─────────────────┐
        │                                   │
        ▼                                   ▼
┌───────────────────────┐     ┌───────────────────────┐
│  NativeStreamerWrapper│     │     Streamer          │
│  (C++ DLL)            │     │  (Python/FFmpeg)      │
│                       │     │                       │
│  - 高パフォーマンス     │     │  - フル機能            │
│  - 低メモリ使用         │     │  - シームレス切替対応   │
└───────────────────────┘     └───────────────────────┘
```

### 使用方法

```python
from python.native_streamer_wrapper import NativeStreamerWrapper

wrapper = NativeStreamerWrapper(
    video_url="video.mp4",
    sender_name="MySpout",
    loop_vod=True,
    log_cb=lambda msg: print(msg),
    stop_cb=lambda: print("Stopped"),
    init_ok_cb=lambda: print("Ready")
)

wrapper.start()
# ...
wrapper.stop()
```

### Python Streamerとの互換性

NativeStreamerWrapperは以下のPython Streamerと同じインターフェースを提供します：

- **メソッド**: `start()`, `stop()`, `seek(seconds)`
- **プロパティ**: `is_vod`, `is_live`, `duration`, `playback_time`, `width`, `height`, `detected_fps`
- **フレームデータ**: `latest_frame_bgr`, `frame_lock`
- **コールバック**: `log_cb`, `stop_cb`, `init_ok_cb`

### gui.pyでの切り替え

gui.pyの先頭にあるフラグで切り替え可能：

```python
# True: C++ DLLを使用（高パフォーマンス、要ビルド済みDLL）
# False: Python Streamerを使用（従来の動作）
USE_NATIVE_BACKEND = False  # デフォルトは従来動作
```

### 注意事項

1. **DLLの存在確認**: `USE_NATIVE_BACKEND = True`の場合、DLLが見つからないと自動的にPython Streamerにフォールバック
2. **シームレス切替機能**: 現在のNativeStreamerWrapperはシームレス切替（画像マッチング同期）をサポートしていません。Python Streamerの高度な同期機能が必要な場合は`USE_NATIVE_BACKEND = False`を使用してください
3. **フレームデータ取得**: `get_current_frame()`でBGRA形式のnumpy配列を取得可能。`latest_frame_bgr`はBGR形式（Alphaチャネル削除済み）

### Spout送信機能

NativeStreamerWrapperはPython側のSpoutGLライブラリを使用してSpout送信を行います。

**動作フロー**:
1. `_run()`メソッドの開始時にSpoutSenderを初期化（`owns_spout=True`且つ`spout=None`の場合）
2. C++ DLLからフレームデータ（BGRA）を取得
3. BGRAからBGRに変換し、SpoutGLの`sendImage()`で送信
4. `stop()`時にSpoutSenderを解放（`owns_spout=True`の場合のみ）

**Spout制御**:
```python
# Spout送信を無効化
wrapper.set_spout_enabled(False)

# Spout送信を有効化
wrapper.set_spout_enabled(True)
```

**外部SpoutSender使用**:
```python
import SpoutGL

# 外部でSpoutSenderを作成
external_sender = SpoutGL.SpoutSender()
external_sender.createOpenGL()
external_sender.setSenderName("MySender")

# 外部SpoutSenderを渡す（wrapperは解放しない）
wrapper = NativeStreamerWrapper(
    video_url="video.mp4",
    sender_name="MySender",
    external_spout_sender=external_sender
)
```

## エラーハンドリング

```python
try:
    player.start("nonexistent.mp4")
except RuntimeError as e:
    print(f"Error: {e}")
    print(f"Details: {player.get_last_error()}")
```
