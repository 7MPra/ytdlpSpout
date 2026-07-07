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

#### 基本設定構造体 `YtdlpSpoutConfig`

`cpp/include/ytdlpspout/ytdlpspout.h:53-59`（Python側 `python/ytdlpspout_native.py:34-42` の`ctypes.Structure`とフィールド順・型が一致）。

| # | フィールド | C型 | 説明 |
|---|-----------|-----|------|
| 1 | `inputFile` | `const char*` | 入力ファイルパスまたはURL |
| 2 | `senderName` | `const char*` | Spout Sender名 |
| 3 | `loop` | `int` | ループ再生（0=false, 1=true） |
| 4 | `useHardwareAccel` | `int` | ハードウェアアクセラレーション（0=false, 1=true） |
| 5 | `verbose` | `int` | 詳細ログ（0=false, 1=true） |

`ytdlpspout_start()`はこの構造体へのポインタを1つ受け取る（2つの文字列引数を直接取るAPIではない）。Python `start()`は内部でこの構造体を組み立てて渡す（`python/ytdlpspout_native.py:514-546`）。

### 拡張再生制御（HTTPヘッダー・スライス読み込み対応）

```c
// HTTPヘッダー構造体
typedef struct YtdlpSpoutHttpHeader {
    const char* key;    // ヘッダーキー（例: "Cookie"）
    const char* value;  // ヘッダー値
} YtdlpSpoutHttpHeader;

// 拡張設定で再生開始
int ytdlpspout_start_ex(YtdlpSpoutHandle handle, const YtdlpSpoutConfigEx* config);

// デフォルト設定で初期化（source以外は使用可能な既定値で埋める）
void ytdlpspout_config_ex_init(YtdlpSpoutConfigEx* config);
```

#### 拡張設定構造体 `YtdlpSpoutConfigEx`

`cpp/include/ytdlpspout/ytdlpspout.h:90-103`（Python側 `python/ytdlpspout_native.py:102-117` と全12フィールドの順序・型が一致）。

| # | フィールド | C型 | 説明 |
|---|---|---|---|
| 1 | `source` | `const char*` | ファイルパスまたはURL |
| 2 | `senderName` | `const char*` | Spout Sender名 |
| 3 | `outputWidth` | `int` | 出力幅（0=ソース解像度） |
| 4 | `outputHeight` | `int` | 出力高さ（0=ソース解像度） |
| 5 | `loop` | `int` | ループ再生（1=有効） |
| 6 | `useHardwareAccel` | `int` | ハードウェアアクセラレーション（1=有効） |
| 7 | `verbose` | `int` | 詳細ログ（1=有効） |
| 8 | `slice` | `YtdlpSpoutSliceConfig` | スライス読み込み設定（下表） |
| 9 | `ytdlp` | `YtdlpSpoutYtDlpConfig` | yt-dlp設定（下表） |
| 10 | `httpHeaders` | `const YtdlpSpoutHttpHeader*` | HTTPヘッダー配列（NULL=なし） |
| 11 | `httpHeadersCount` | `int` | HTTPヘッダー数 |
| 12 | `isHlsHint` | `int` | HLS判定ヒント。`-1`=自動判定、`0`=非HLS、`1`=HLS（yt-dlp側の判定結果を伝搬する用途） |

#### スライス読み込み設定 `YtdlpSpoutSliceConfig`

`cpp/include/ytdlpspout/ytdlpspout.h:66-75` / `python/ytdlpspout_native.py:60-71`。

| # | フィールド | C型 | 説明 |
|---|---|---|---|
| 1 | `enabled` | `int` | スライス読み込み有効（1=有効、0=無効） |
| 2 | `chunkSize` | `size_t` | チャンクサイズ（バイト） |
| 3 | `maxCacheMemory` | `size_t` | 最大キャッシュメモリ（バイト） |
| 4 | `maxConcurrentDownloads` | `int` | 最大並列ダウンロード数 |
| 5 | `prefetchChunksAhead` | `int` | 先読みチャンク数 |
| 6 | `criticalChunksAhead` | `int` | 最優先チャンク数 |
| 7 | `enableContinuousDownload` | `int` | 継続ダウンロード有効（1=有効、ファイル全体をバックグラウンドでダウンロード） |
| 8 | `cachePath` | `const char*` | ファイルキャッシュパス（NULL=メモリのみ） |

#### yt-dlp設定 `YtdlpSpoutYtDlpConfig`

`cpp/include/ytdlpspout/ytdlpspout.h:78-81` / `python/ytdlpspout_native.py:74-79`。

| # | フィールド | C型 | 説明 |
|---|---|---|---|
| 1 | `path` | `const char*` | yt-dlpパス（NULL=自動検出） |
| 2 | `preferredHeight` | `int` | 希望解像度 |

#### `ytdlpspout_config_ex_init()` の既定値

`cpp/src/bindings/c_api.cpp:519-541`。`memset(config, 0, sizeof(*config))`で全体を0初期化した後、以下のフィールドのみ明示的に設定する（`source`はゼロ（NULL）のままなので呼び出し側が必ず設定する必要がある）。

| フィールド | 既定値 |
|---|---|
| `senderName` | `"ytdlpSpout"` |
| `loop` | `0` |
| `useHardwareAccel` | `1` |
| `verbose` | `0` |
| `slice.enabled` | `1` |
| `slice.chunkSize` | `2 * 1024 * 1024`（2MB、高解像度向け） |
| `slice.maxCacheMemory` | `256 * 1024 * 1024`（256MB） |
| `slice.maxConcurrentDownloads` | `6` |
| `slice.prefetchChunksAhead` | `24`（48MB先読み相当） |
| `slice.criticalChunksAhead` | `6` |
| `slice.enableContinuousDownload` | `1` |
| `slice.cachePath` | `NULL` |
| `ytdlp.path` | `NULL` |
| `ytdlp.preferredHeight` | `1080` |
| `httpHeaders` | `NULL` |
| `httpHeadersCount` | `0` |
| `isHlsHint` | `-1`（自動判定） |

**使用例（Python）**:
```python
# HTTPヘッダーを含む再生開始
player.start_ex(
    source="https://example.com/video.mp4",
    sender_name="MySpout",
    http_headers={"Cookie": "session=abc123", "User-Agent": "MyApp/1.0"}
)
```

`start_ex()`のPython側シグネチャ（`python/ytdlpspout_native.py:548-567`）:

```python
def start_ex(
    self,
    source: str,
    sender_name: str = "ytdlpSpout",
    output_width: int = 0,
    output_height: int = 0,
    loop: bool = False,
    use_hardware_accel: bool = True,
    verbose: bool = False,
    slice_enabled: bool = True,
    chunk_size: Optional[int] = None,
    max_cache_memory: Optional[int] = None,
    max_concurrent_downloads: Optional[int] = None,
    prefetch_chunks_ahead: Optional[int] = None,
    cache_path: Optional[str] = None,
    ytdlp_path: Optional[str] = None,
    preferred_height: int = 1080,
    http_headers: Optional[dict] = None,
    is_hls: Optional[bool] = None
) -> bool
```

- `chunk_size` / `max_cache_memory` / `max_concurrent_downloads` / `prefetch_chunks_ahead`: `None`（デフォルト）の場合、`ytdlpspout_config_ex_init()`が設定したC++側のチューニング済み既定値（上表）を**上書きしない**。明示的に値を渡した場合のみconfigに反映される（`python/ytdlpspout_native.py:619-629`）。`criticalChunksAhead` / `enableContinuousDownload`にはこれに対応するPython引数がなく、常にC++側の既定値（`6` / `1`）がそのまま使われる。
- `is_hls`: `None`（デフォルト）の場合`isHlsHint = -1`（自動判定）。`True`/`False`を指定すると呼び出し側（yt-dlp側）の判定結果を`isHlsHint = 1`/`0`として強制する（`python/ytdlpspout_native.py:674-678`）。

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

#### 再生状態 `YtdlpSpoutState`（`cpp/include/ytdlpspout/ytdlpspout.h:106-111`）

| 値 | 名前 |
|----|------|
| 0 | `YTDLPSPOUT_STATE_STOPPED` |
| 1 | `YTDLPSPOUT_STATE_PLAYING` |
| 2 | `YTDLPSPOUT_STATE_PAUSED` |
| 3 | `YTDLPSPOUT_STATE_ERROR` |

Python側は`python/ytdlpspout_native.py:121-125`の`YtdlpSpoutState`クラス（`STOPPED` / `PLAYING` / `PAUSED` / `ERROR`のクラス属性）として同じ整数値を提供する（実行時型チェックのない簡易列挙）。

#### 動画情報構造体 `YtdlpSpoutVideoInfo`（`cpp/include/ytdlpspout/ytdlpspout.h:114-120` / `python/ytdlpspout_native.py:45-53`）

| # | フィールド | C型 | 説明 |
|---|-----------|-----|------|
| 1 | `width` | `int` | 幅（ピクセル） |
| 2 | `height` | `int` | 高さ（ピクセル） |
| 3 | `fps` | `double` | フレームレート |
| 4 | `duration` | `double` | 再生時間（秒） |
| 5 | `totalFrames` | `int64_t` | 総フレーム数 |

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

**エラー通知の方式**: 専用のエラーコードenumは存在しない。各関数は戻り値で成否を伝える。

- `int`を返す関数: `0`=成功、非0=失敗。負の値の具体的な意味は関数ごとに異なる内部実装都合の詳細であり、安定したAPI契約として個々の値に依存すべきではない。例えば基本版`ytdlpspout_start()`は失敗要因ごとに`-1`〜`-7`を返す（`cpp/src/bindings/c_api.cpp:156-238`）のに対し、拡張版`ytdlpspout_start_ex()`は失敗時に一律`-1`を返す（`cpp/src/bindings/c_api.cpp:543-679`）。
- 値を返す関数（`double` / `float` / ハンドル等）: 無効なハンドルやエラー時は`0` / `0.0` / `NULL` / `YTDLPSPOUT_STATE_ERROR`などの安全なデフォルト値を返す。
- 失敗時は`ytdlpspout_get_last_error()`（スレッドローカル）で詳細な文字列メッセージを取得できる。

**C ABI境界での例外安全性**: すべてのエクスポート関数は内部で`try/catch`し、C++例外（`std::exception`およびそれ以外すべて）を関数境界の外に漏らさない（`YTDLPSPOUT_CATCH_RETURN` / `YTDLPSPOUT_CATCH_VOID`マクロ、`cpp/src/bindings/c_api.cpp:86-100`）。捕捉した例外は`SetLastError()`でスレッドローカルエラーに記録され、戻り値のある関数はエラー値を返し、`void`関数は何もせず正常終了する。そのため、Python（ctypes）側でC++例外がそのままPython例外として飛んでくることはない。Python `start()` / `start_ex()`はDLLの戻り値が非0の場合に`RuntimeError`を送出する（`python/ytdlpspout_native.py:542-546`, `680-684`）。

### スライスキャッシュ統計（チャンク単位、拡張API）

`cpp/include/ytdlpspout/ytdlpspout.h:269-292` / `cpp/src/bindings/c_api.cpp:681-730`。スライス読み込み（チャンク分割ダウンロード）の進捗を取得する低レベルAPI。HLS再生時の統計は次節の`ytdlpspout_get_hls_cache_stats()`を使うこと。

```c
// ダウンロード進捗を取得（0.0〜1.0）
double ytdlpspout_get_download_progress(YtdlpSpoutHandle handle);

// 帯域幅を取得（bytes/sec）
double ytdlpspout_get_bandwidth(YtdlpSpoutHandle handle);

// 全チャンクがキャッシュ済みか確認
int ytdlpspout_is_fully_cached(YtdlpSpoutHandle handle);

// キャッシュ統計を取得（チャンク数）
void ytdlpspout_get_cache_stats(
    YtdlpSpoutHandle handle,
    size_t* cachedChunks,
    size_t* totalChunks
);
```

**現状の実装上の制約**:
- `ytdlpspout_get_bandwidth()`は未実装で、常に`0.0`を返す（`cpp/src/bindings/c_api.cpp:690-697`のTODOコメント参照）。帯域幅が必要な場合はHLS用の`ytdlpspout_get_hls_cache_stats()`の`bandwidth`フィールドを使用すること。
- `ytdlpspout_get_cache_stats()`は正確なチャンク数を返さない近似実装で、ダウンロード進捗が`1.0`以上なら`(cachedChunks=1, totalChunks=1)`、それ以外は`(0, 0)`を返す（`cpp/src/bindings/c_api.cpp:708-730`）。

Python側は`download_progress` / `bandwidth` / `is_fully_cached`プロパティと`get_cache_stats()`メソッドとして公開される（`python/ytdlpspout_native.py:457-482`）。

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

（`cpp/include/ytdlpspout/ytdlpspout.h:122-130,298-301` / `cpp/src/bindings/c_api.cpp:732-764`。Python側の構造体定義は`python/ytdlpspout_native.py:90-99`で、フィールド順・型が一致することを確認済み）

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

**重要**: HLSストリーム（`.m3u8`等）では、CustomIOContextを使用せず、FFmpegのネイティブHTTPハンドラを使用する。

```
HLS URL判定（isHlsHintが-1なら自動判定、0以上ならヒントを優先）
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

**isHlsHintとURL自動判定の優先順位**（`cpp/src/player/VideoPlayer.cpp:160-169`）: FFI経由の`config.isHlsHint`が`0`以上（`0`または`1`）ならその値を優先し、`-1`（未指定）の場合のみURLヒューリスティックで自動判定する。

**判定ロジック（C++、実装本体）**: URL自動判定は`HlsSliceLoadingManager::IsHlsUrl()`（`cpp/src/hls/HlsSliceLoadingManager.cpp:641-673`）に一本化されており、`cpp/src/decoder/VideoDecoder.cpp:174`と`cpp/src/player/VideoPlayer.cpp:167`の両方がこれを呼び出す（各所で判定ロジックを重複実装してはいない）。小文字化した上で、次のいずれか一つでも真であればHLSと判定する:
- `.m3u8`を含む
- `.m3u`を含む
- `format=m3u8`を含み、かつその直後が文字列末尾／`&`／`#`のいずれか（`format=m3u8xxx`等の誤検知を避けるための境界チェック）
- `mime=application%2fvnd.apple.mpegurl`を含む（URLエンコードされたMIMEタイプ指定）

**protocol_whitelist**: 暗号化HLS対応のため`crypto`プロトコルを追加（`cpp/src/decoder/VideoDecoder.cpp:194`）：
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
2. 依存DLL（FFmpeg、Spout2など）。`python/ytdlpspout_native.py:188-199`がロード順序問題を回避するため明示的にプリロードを試みる既知の依存DLL一覧：
   `zlib1.dll`, `avutil-59.dll`, `swresample-5.dll`, `avcodec-61.dll`, `avformat-61.dll`, `swscale-8.dll`, `fmt.dll`, `spdlog.dll`, `libcurl.dll`, `Spout.dll`

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

gui.pyの先頭にあるフラグで切り替え可能（`gui.py:40-56`）：

```python
# True: C++ DLLを使用（高パフォーマンス、要ビルド済みDLL）
# False: Python Streamerを使用（従来の動作）
USE_NATIVE_BACKEND = True
```

現在のデフォルトは`True`（C++ DLLバックエンド）。`from python.native_streamer_wrapper import NativeStreamerWrapper`のインポートに失敗した場合（例: 依存パッケージ未インストール等）のみ、起動時に自動的に`USE_NATIVE_BACKEND = False`へフォールバックする（`gui.py:47-56`）。

### 注意事項

1. **バックエンド選択のタイミング**: バックエンドの選択は`native_streamer_wrapper`モジュールの**インポート成否**（`gui.py:47-56`の`NATIVE_BACKEND_AVAILABLE`）で決まり、DLLファイルの実在有無をこの時点ではチェックしない。DLL自体が見つからない場合、再生開始時（バックグラウンドスレッド内の`YtdlpSpoutNative()`呼び出し、`python/native_streamer_wrapper.py:588-592`）で初めて失敗し、例外はログ出力後にスレッドが終了する（`stop_cb`は呼ばれるが、Python Streamerへの自動フォールバックは行われない）。
2. **シームレス切替機能**: `NativeStreamerWrapper`は画像マッチングによるPTS同期用のメソッド（`pause_at_pts()` / `resume()` / `wait_for_pts()` / `find_best_match_pts()`、`python/native_streamer_wrapper.py:269-422`）とPTS追跡（`current_frame_pts`）を実装している。
3. **フレームデータ取得**: `get_current_frame()`でBGRA形式のnumpy配列を取得可能。`latest_frame_bgr`はBGR形式（Alphaチャネル削除済み）

### Spout送信機能

**実際の送信経路はC++ DLL側である。** `NativeStreamerWrapper`はPython側のSpoutGLライブラリを使ってSpout送信を行っていない（`python/native_streamer_wrapper.py`全体に`SpoutGL`のimportや`sendImage()`呼び出しは存在しない）。DLLは`Spout.dll`を依存DLLとしてロードし（`python/ytdlpspout_native.py:198`）、内部で独自にSpout Senderを作成・送信する。

**動作フロー**:
1. `_run()`が`start_ex()`でC++ DLLに再生を開始させる。この時点でC++側が`sender_name`のSpout Senderを作成し、以降のフレーム送信をDLL内部で行う（Python側はSpoutSenderを作成しない。`python/native_streamer_wrapper.py:655-656`のコメント「C++ DLLがSpout送信を担当するため、Python側のSpoutSender初期化は不要」を参照）。
2. Python側は`get_current_frame()`でBGRAフレームを取得し、BGRに変換して`latest_frame_bgr`に格納する（`_update_preview_frame()`、`python/native_streamer_wrapper.py:499-550`）。これは**GUIプレビュー表示専用**であり、Spout送信には使われない。
3. `stop()`時、`owns_spout=True`（`external_spout_sender`未指定時）かつ`self.spout`が非`None`であれば`self.spout.releaseSender()`を呼ぶ（`python/native_streamer_wrapper.py:459-465`）。ただし後述の通り、既定構成では`self.spout`は常に`None`のままなので、この解放処理が実際に何かを解放することはない。

**`set_spout_enabled()`（現状の制約）**:
```python
wrapper.set_spout_enabled(False)
wrapper.set_spout_enabled(True)
```
`set_spout_enabled()`は`self.spout_enabled`フラグを設定してログ出力するのみで（`python/native_streamer_wrapper.py:271-275`）、このフラグを読み取ってSpout送信のオン/オフを実際に制御している箇所はクラス内に存在しない。C API側にもSpout送信の有効/無効を切り替えるエクスポート関数はない。したがって現時点でこのメソッドは、C++ DLLが行う実際のSpout出力には影響しない。

**`external_spout_sender`引数（現状の制約）**:
```python
external_sender = SpoutGL.SpoutSender()
external_sender.createOpenGL()
external_sender.setSenderName("MySender")

wrapper = NativeStreamerWrapper(
    video_url="video.mp4",
    sender_name="MySender",
    external_spout_sender=external_sender
)
```
コンストラクタは`external_spout_sender`をAPI互換性のために受け取り`self.spout`に保持し、`owns_spout`を`external_spout_sender is None`から自動判定する（`python/native_streamer_wrapper.py:113-114`）。ただし、この引数はコンストラクタのdocstring自身に明記されている通り「C++ DLLでは無視」される（`python/native_streamer_wrapper.py:72`）。実際のSpout送信先（Sender名を含む）はC++ DLL側の`sender_name`のみで決まる。

## エラーハンドリング

C API自体は例外を送出しない（前述「エラー処理」参照）。Pythonバインディングは`ytdlpspout_start` / `ytdlpspout_start_ex`の戻り値が非0のとき、`get_last_error()`のメッセージを添えて`RuntimeError`を送出する（`python/ytdlpspout_native.py:542-546`, `680-684`）。

```python
try:
    player.start("nonexistent.mp4")
except RuntimeError as e:
    print(f"Error: {e}")
    print(f"Details: {player.get_last_error()}")
```
