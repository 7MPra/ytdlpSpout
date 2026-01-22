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

### 拡張再生制御（HTTPヘッダー対応）

```c
// HTTPヘッダー構造体
typedef struct YtdlpSpoutHttpHeader {
    const char* key;    // ヘッダーキー（例: "Cookie"）
    const char* value;  // ヘッダー値
} YtdlpSpoutHttpHeader;

// YtdlpSpoutConfigEx構造体に追加されたフィールド
const YtdlpSpoutHttpHeader* httpHeaders;  // HTTPヘッダー配列
int httpHeadersCount;                      // ヘッダー数

// 拡張設定で再生開始（HTTPヘッダー対応）
int ytdlpspout_start_ex(YtdlpSpoutHandle handle, const YtdlpSpoutConfigEx* config);

// デフォルト設定で初期化
void ytdlpspout_config_ex_init(YtdlpSpoutConfigEx* config);
```

**使用例（Python）**:
```python
# HTTPヘッダーを含む再生開始
player.start_ex(
    source="https://example.com/video.mp4",
    sender_name="MySpout",
    http_headers={"Cookie": "session=abc123", "User-Agent": "MyApp/1.0"}
)
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

### HLSキャッシュ統計

HLSストリーム再生時のキャッシュ状態を取得するAPI。

```c
/// @brief HLSキャッシュ統計
typedef struct YtdlpSpoutHlsCacheStats {
    int cachedSegments;         ///< キャッシュ済みセグメント数
    int totalSegments;          ///< 総セグメント数
    double downloadProgress;    ///< ダウンロード進捗 (0.0〜1.0)
    double bandwidth;           ///< 推定帯域幅 (bytes/sec)
    int isFullyCached;          ///< 完全キャッシュ済み (1=true, 0=false)
    int isHlsMode;              ///< HLSモードで再生中 (1=true, 0=false)
} YtdlpSpoutHlsCacheStats;

/// @brief HLSキャッシュ統計を取得
/// @param handle インスタンスハンドル
/// @param stats 統計情報の出力先
/// @return 成功時0、失敗時-1
YTDLPSPOUT_API int ytdlpspout_get_hls_cache_stats(
    YtdlpSpoutHandle handle,
    YtdlpSpoutHlsCacheStats* stats
);
```

**使用例（Python）**:
```python
player = YtdlpSpoutNative()
player.start_ex("https://example.com/playlist.m3u8")

# HLS統計を取得
stats = player.get_hls_cache_stats()
if stats:
    print(f"HLSモード: {stats['is_hls_mode']}")
    print(f"キャッシュ済み: {stats['cached_segments']}/{stats['total_segments']}")
    print(f"ダウンロード進捗: {stats['download_progress'] * 100:.1f}%")
    print(f"帯域幅: {stats['bandwidth'] / 1024:.1f} KB/s")
    print(f"完全キャッシュ済み: {stats['is_fully_cached']}")
```

**戻り値の説明**:
- `cached_segments`: キャッシュに読み込まれたセグメント数（HLSモード）またはチャンク数（通常モード）
- `total_segments`: プレイリスト内の総セグメント数またはファイル全体のチャンク数
- `download_progress`: ダウンロード完了率（0.0〜1.0）
- `bandwidth`: 推定ダウンロード帯域幅（bytes/sec）、HLSモードのみ有効
- `is_fully_cached`: すべてのデータがキャッシュ済みかどうか
- `is_hls_mode`: HLSスライス読み込みモードで再生中かどうか

**未再生時の動作**:
プレイヤーが再生開始前または停止後の場合、すべての値がデフォルト（0またはfalse）で返されます。

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

### HTTPヘッダー対応（認証・Cookie等）

ニコニコ動画など認証が必要なサービスでは、HTTPヘッダー（特にCookie）をDLLに渡す必要があります。

#### 基本的な使い方

```python
# 直接start_ex()を使用
player = YtdlpSpoutNative()
player.start_ex(
    source="https://example.com/video.mp4",
    sender_name="MySpout",
    http_headers={
        "Cookie": "user_session=abcdef123456",
        "User-Agent": "MyApp/1.0"
    }
)
```

#### NativeStreamerWrapperでの使用

```python
from python.native_streamer_wrapper import NativeStreamerWrapper

# yt-dlpで取得したHTTPヘッダーを渡す
wrapper = NativeStreamerWrapper(
    video_url="resolved_stream_url",
    sender_name="MySpout",
    pre_resolved_headers={"Cookie": "user_session=xxx", "Referer": "https://example.com/"}
)
wrapper.start()
```

#### データフロー

```
Python dict                 C struct array               C++ std::map
{"Cookie": "xxx"}  ──────>  YtdlpSpoutHttpHeader[]  ──>  map<string,string>
                            [0].key = "Cookie"           ["Cookie"] = "xxx"
                            [0].value = "xxx"
```

#### C++ DLL内でのヘッダー適用箇所

HTTPヘッダーはDLL内で以下の全HTTPリクエストに適用されます：

| コンポーネント | リクエスト種別 | 用途 |
|---------------|---------------|------|
| CustomIOContext | HEAD | ファイルサイズ（Content-Length）取得 |
| ChunkDownloader | GET (Range) | 動画データのチャンクダウンロード |
| VideoDecoder (HLS) | GET | HLSマニフェスト、キーファイル、セグメント |

これにより、ニコニコ動画等のCookie認証が必要なサービスでも、すべてのHTTPリクエストで認証が機能します。

#### HLSストリーム対応

**重要**: HLSストリーム（`.m3u8`）では、CustomIOContextを使用せず、FFmpegのネイティブHTTPハンドラを使用します。

```
HLS URL判定（.m3u8, format=m3u8, /hls/）
    ↓ YES
FFmpegネイティブHTTPで直接オープン
    ↓
AVDictionaryでHTTPヘッダーを設定
    ↓
FFmpeg HLSデマクサが内部リクエストにヘッダーを継承
    ↓
キーファイル、セグメント取得時にもCookie認証が機能
```

**理由**: CustomIOContext使用時、FFmpegのHLSデマクサが実行する内部HTTPリクエスト（キーファイル、セグメント）にAVDictionaryのヘッダーオプションが継承されないため。

**判定ロジック（C++）**:
```cpp
static bool IsHlsUrl(const std::string& url) {
    std::string lower = url;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    return (lower.find(".m3u8") != std::string::npos) ||
           (lower.find("format=m3u8") != std::string::npos) ||
           (lower.find("/hls/") != std::string::npos);
}
```

**protocol_whitelist**: 暗号化HLS対応のため`crypto`プロトコルを追加：
```cpp
av_dict_set(&opts, "protocol_whitelist", "file,http,https,tcp,tls,crypto,data", 0);
```

#### セキュリティ注意事項

1. **ログ出力**: Cookieなどの機密情報は値をログに出力しない。ヘッダー数のみログ出力可
2. **メモリ管理**: Python側で文字列への参照を保持し、DLL呼び出し中にGCされないようにする
3. **配布禁止**: Cookie値を含むログファイルや設定ファイルは配布・公開しないこと

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

### DLL検索パス

`YtdlpSpoutNative`クラスは以下の順序でDLLを検索します（優先度順）：

1. **CMakeビルド出力（優先、Debug優先）**:
   - `cpp/build/bin/Debug/ytdlpspout.dll`
   - `cpp/build/bin/Release/ytdlpspout.dll`

2. **ローカルpythonフォルダ**:
   - `python/ytdlpspout.dll`

3. **レガシーパス（VS2022）**:
   - `cpp/build/vs2022/bin/Release/ytdlpspout.dll`
   - `cpp/build/vs2022/bin/Debug/ytdlpspout.dll`

4. **Presetビルドパス**:
   - `cpp/build/windows-x64-release/bin/ytdlpspout.dll`
   - `cpp/build/windows-x64-debug/bin/ytdlpspout.dll`

5. **環境変数**: `YTDLPSPOUT_DLL`

DLLがロードされると、パスがログに出力されます（INFOレベル）。

```python
import logging
logging.basicConfig(level=logging.INFO)

from python.ytdlpspout_native import YtdlpSpoutNative
player = YtdlpSpoutNative()
# INFO - DLL loaded from: F:\ytdlpSpout\cpp\build\bin\Release\ytdlpspout.dll
```

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
