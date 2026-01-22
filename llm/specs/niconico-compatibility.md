# ニコニコ動画対応ガイド

このドキュメントはニコニコ動画の再生サポートに関する技術的な詳細を説明します。

## 概要

ytdlpSpoutは、yt-dlpを使用してニコニコ動画（nicovideo.jp）からのストリーミング再生をサポートしています。

## フォーマット選択ロジック

### 背景

ニコニコ動画は、YouTubeとは異なる音声フォーマット（AAC）を使用することがあります。従来のフォーマット指定（`bestvideo[ext=mp4]+bestaudio[ext=m4a]`）では、AAC音声のみ提供される動画で失敗する可能性がありました。

### 改善されたフォーマット文字列

```python
FORMAT_STRING = (
    'bestvideo[ext=mp4]+bestaudio[ext=m4a]/'    # YouTube向け優先
    'bestvideo[ext=mp4]+bestaudio[ext=aac]/'    # ニコニコ動画等のAAC音声対応
    'bestvideo+bestaudio/'                       # 一般的なフォーマット
    'bv*+ba/b/'                                  # yt-dlp推奨の汎用フォーマット
    'best'                                       # 最終フォールバック
)
```

### フォーマット選択の優先順位

1. **MP4 + M4A**: YouTube等で最も一般的な組み合わせ
2. **MP4 + AAC**: ニコニコ動画等で使用されるAAC音声
3. **任意のビデオ + 任意の音声**: コーデック制限なし
4. **bv\*+ba/b**: yt-dlp公式推奨の汎用フォーマット
5. **best**: 最終フォールバック

## Cookie認証

### 必要性

ニコニコ動画では以下の場合にCookie認証が必要です：

- プレミアム会員限定動画
- 一部の年齢制限動画
- ログインユーザーのみ視聴可能な動画

### Cookieファイルの配置

```
data/cookies.txt
```

Netscape形式のCookieファイルを配置してください。

### Cookieの取得方法

1. ブラウザでニコニコ動画にログイン
2. ブラウザ拡張機能（例: Get cookies.txt）でCookieをエクスポート
3. `data/cookies.txt`に保存

### セキュリティ注意事項

- Cookieファイルの内容は**ログに出力されません**
- Cookieファイルはリポジトリにコミットしないでください（`.gitignore`に追加済み）

## HLSストリーム対応

ニコニコ動画はHLSプロトコル（`.m3u8`）でストリームを配信します。

### 対応状況

- C++ DLL（FFmpeg/libav）: HLS再生対応
- Python側: yt-dlpが解決したHLS URLを直接DLLに渡す

### HLSストリームの特別な処理

HLSストリームでは、FFmpegのHLSデマクサが内部的に複数のHTTPリクエスト（マニフェスト、キーファイル、セグメント）を実行します。これらの内部リクエストにHTTPヘッダー（Cookie等）を継承させるため、**HLSストリームではCustomIOContextを使用せず、FFmpegのネイティブHTTPハンドラを使用します**。

#### 処理の分岐

```
URLタイプ判定
    ↓
┌─────────────────────────────────────────────────────┐
│ HLS URL（.m3u8, format=m3u8, /hls/）の場合:         │
│   → CustomIOContext不使用                           │
│   → スライスローディング不使用                       │
│   → FFmpegネイティブHTTPハンドラで直接オープン       │
│   → HTTPヘッダーがすべての内部リクエストに継承       │
├─────────────────────────────────────────────────────┤
│ 非HLS URLの場合:                                    │
│   → CustomIOContext使用（従来通り）                 │
│   → スライスローディング有効（設定依存）             │
└─────────────────────────────────────────────────────┘
```

#### HLS判定ロジック（C++）

```cpp
// VideoDecoder.cpp / VideoPlayer.cpp
static bool IsHlsUrl(const std::string& url) {
    std::string lower = url;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    return (lower.find(".m3u8") != std::string::npos) ||
           (lower.find("format=m3u8") != std::string::npos) ||
           (lower.find("/hls/") != std::string::npos);
}
```

#### protocol_whitelistの設定

暗号化HLS（AES-128）に対応するため、`crypto`プロトコルをホワイトリストに追加：

```cpp
av_dict_set(&opts, "protocol_whitelist", "file,http,https,tcp,tls,crypto,data", 0);
```

### 技術的詳細

```
ニコニコ動画URL
    ↓ yt-dlp解決
HLS manifest URL (.m3u8)
    ↓ C++ DLL (FFmpeg)
動画フレーム
```

## トラブルシューティング

### エラー: "Requested format is not available"

**原因**: 指定したフォーマットが利用できない

**解決策**: 新しいフォーマット文字列が自動的にフォールバックを試みます

### エラー: "This video is for premium members only"

**原因**: プレミアム会員限定動画

**解決策**: 
1. プレミアム会員アカウントでログイン
2. Cookieをエクスポートして`data/cookies.txt`に配置

### エラー: "Login required"

**原因**: ログインが必要な動画

**解決策**: Cookieファイルを設定

## HTTPヘッダーのC++ DLLへの転送

### 背景

ニコニコ動画などの一部のサービスでは、動画ストリームへのアクセス時に特定のHTTPヘッダー（Cookie等）が必要です。yt-dlpで取得したHTTPヘッダー情報をC++ DLLに渡す機能を実装しています。

### データフロー

```
yt-dlp URL解決 (Python)
    ↓ ResolvedInfo.http_headers
NativeStreamerWrapper
    ↓ pre_resolved_headers引数
YtdlpSpoutNative.start_ex()
    ↓ http_headers引数 (dict → C配列)
C++ DLL (PlayerConfig.httpHeaders)
    ↓
SliceLoadingManager → CustomIOContext
    ↓
┌─────────────────────────────────────┐
│ HttpClient (HEADリクエスト)         │ ← ヘッダー適用
│ ChunkDownloader (GETリクエスト)     │ ← ヘッダー適用
└─────────────────────────────────────┘
    ↓
HTTP Range Request with headers
```

### C++ DLL内のHTTPヘッダー適用箇所

HTTPヘッダーは以下のすべてのHTTPリクエストに適用されます：

1. **CustomIOContext::InitializeHttp()内のHEADリクエスト**
   - Content-Length取得用の初期HEADリクエスト
   - HttpClientConfig.headersにヘッダーを設定してから実行

2. **ChunkDownloader内のGETリクエスト**
   - 各ワーカースレッドがHttpClientを作成する際にヘッダーを設定
   - Range Requestでチャンクをダウンロード
   - **HLSセグメントダウンロード時も最新のヘッダーを適用** (2026-01-20修正)
     - `SetHttpHeaders()`で設定されたヘッダーは`DownloadSegment()`呼び出し時に毎回適用
     - ワーカースレッド起動後にヘッダーを設定しても正しく反映される

3. **VideoDecoder::Open() / OpenWithCustomIO()内のFFmpegリクエスト** (NEW)
   - HLSストリーム再生時、FFmpegが内部的に実行するHTTPリクエストにも適用
   - キーファイル（.key）、セグメント（.ts）取得にヘッダーが継承される
   - `avformat_open_input()`のAVDictionary経由で`headers`オプションを設定

### HLSストリームでのHTTPヘッダー適用

HLSストリーム（.m3u8）を再生する場合、FFmpegのHLSデマクサは内部的に以下のHTTPリクエストを実行します：

```
メインマニフェスト (.m3u8)
    ↓ HTTPヘッダー適用
キーファイル取得 (.key)  ← 暗号化されたHLSの場合
    ↓ HTTPヘッダー適用
セグメント取得 (.ts)
    ↓ HTTPヘッダー適用
次のセグメント...
```

これらすべての内部リクエストに、`VideoDecoder::Open()`で設定したHTTPヘッダーが適用されます。

#### 技術的実装

**カスタムio_openコールバック**

FFmpegのHLSデマクサは、デフォルトでは`avformat_open_input()`に渡したHTTPヘッダーを内部リクエスト（セグメント、キーファイル）に自動的に継承しません。この問題を解決するため、`AVFormatContext::io_open`コールバックをカスタマイズしています：

```cpp
// VideoDecoder.cpp

// HTTPヘッダーを保持する構造体
struct HttpHeaderContext {
    std::string headers;  // "Key: Value\r\n" 形式
};

// カスタムio_openコールバック
static int custom_io_open(AVFormatContext* s, AVIOContext** pb,
                          const char* url, int flags, AVDictionary** options) {
    HttpHeaderContext* ctx = static_cast<HttpHeaderContext*>(s->opaque);
    AVDictionary* merged_opts = nullptr;
    
    // 既存のオプションをコピー
    if (options && *options) {
        av_dict_copy(&merged_opts, *options, 0);
    }
    
    // HTTPヘッダーを追加（HLS内部リクエストに伝播）
    if (ctx && !ctx->headers.empty()) {
        av_dict_set(&merged_opts, "headers", ctx->headers.c_str(), 0);
    }
    
    // avio_open2でURLを開く
    int ret = avio_open2(pb, url, flags, &s->interrupt_callback, &merged_opts);
    
    av_dict_free(&merged_opts);
    return ret;
}

// Open()メソッド内でHLSストリームの場合に設定
if (isHls && !httpHeaders.empty()) {
    m_impl->formatCtx = avformat_alloc_context();
    
    // HTTPヘッダーコンテキストを作成
    m_impl->httpHeaderCtx = new HttpHeaderContext();
    for (const auto& [key, value] : httpHeaders) {
        m_impl->httpHeaderCtx->headers += key + ": " + value + "\r\n";
    }
    
    // カスタムio_openコールバックを設定
    m_impl->formatCtx->opaque = m_impl->httpHeaderCtx;
    m_impl->formatCtx->io_open = custom_io_open;
}
```

**ポイント:**
- `ffio_open_whitelist`は内部APIのため、`avio_open2`を使用
- `AVFormatContext::opaque`にヘッダーコンテキストを格納
- Close時に`HttpHeaderContext`を確実に解放してメモリリーク防止

### 使用方法

```python
from python.ytdlp_resolver import YtDlpAsyncResolver, ResolvedInfo
from python.native_streamer_wrapper import NativeStreamerWrapper

# URL解決
resolver = YtDlpAsyncResolver(cookie_file="data/cookies.txt")
result = resolver.resolve_sync(url)  # ResolvedInfo

# ストリーマー作成時にヘッダーを渡す
wrapper = NativeStreamerWrapper(
    video_url=url,
    sender_name="ytdlpSpout",
    pre_resolved_url=result.stream_url,
    pre_resolved_headers=result.http_headers  # Cookie等のヘッダー
)
```

### セキュリティ考慮事項

- **ログ出力**: HTTPヘッダーの値（特にCookie）はログに出力されません。ヘッダー数のみ表示されます。
- **メモリ管理**: Python側で文字列参照が適切に保持され、DLL呼び出し中に解放されません。

## サポートされるURL

| URLパターン | 説明 |
|------------|------|
| `https://www.nicovideo.jp/watch/sm*` | 通常の動画 |
| `https://nicovideo.jp/watch/sm*` | www なし |
| `https://nico.ms/sm*` | 短縮URL |
| `https://live.nicovideo.jp/watch/lv*` | ニコニコ生放送 |

## 開発者向け情報

### テスト

```bash
# ニコニコ動画互換性テスト
python -m pytest tests/test_niconico_compat.py -v

# HTTPヘッダー機能テスト (Python)
python -m pytest tests/test_http_headers.py -v

# HLS HTTPヘッダー伝播テスト (C++)
cd cpp/build && ctest -R TestHlsHttpHeaders -V

# HLSスライスローディングマネージャーテスト (C++)
cd cpp/build && ctest -R TestHlsSliceLoadingManager -V
```

## HLSスライスローディングの初期化待機

### 背景

HLSストリームの再生開始時、C++ DLL内の `HlsSliceLoadingManager::Open()` がセグメントのダウンロードを非同期で開始します。しかし、FFmpegの `avformat_find_stream_info()` が呼ばれると、`ReadPacket()` コールバックがセグメントデータを要求します。最初のセグメントがダウンロードされる前にこれが発生すると、タイムアウトまでブロックし、再生が開始されないように見える問題がありました。

### 解決策

`HlsSliceLoadingManager::Open()` の最後で、最初のセグメントがダウンロードされるまで待機するようにしました：

```cpp
// HlsSliceLoadingManager.cpp - Open()内

// 8. 先頭セグメントのダウンロードを開始し、完了を待機
m_impl->isOpen = true;
UpdatePlaybackPosition(0.0);

// 最初のセグメントがダウンロードされるまで待機
const int maxWaitMs = config.readTimeoutMs > 0 ? config.readTimeoutMs : 30000;
const int pollIntervalMs = 100;
int elapsedMs = 0;

while (elapsedMs < maxWaitMs) {
    if (m_impl->cache->IsSegmentCached(0)) {
        LOG_DEBUG("First segment is now available");
        break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(pollIntervalMs));
    elapsedMs += pollIntervalMs;
}
```

### 処理フロー

```
HlsSliceLoadingManager::Open()
    ↓
1. m3u8ダウンロード・パース
2. セグメントキャッシュ初期化
3. ChunkDownloader開始
4. AVIOContext初期化
5. UpdatePlaybackPosition(0.0) でセグメント0をリクエスト
6. セグメント0がダウンロードされるまで待機 ← NEW
    ↓
セグメント0が利用可能になったらOpen()完了
    ↓
VideoDecoder::OpenWithAVIOContext()
    ↓
avformat_find_stream_info() → ReadPacket() → セグメント0を取得（即座に利用可能）
```

### 関連ファイル

- `python/ytdlp_resolver.py`: URL解決、フォーマット選択ロジック
- `python/native_streamer_wrapper.py`: ストリーミングラッパー
- `python/ytdlpspout_native.py`: C++ DLLバインディング（HTTPヘッダー構造体含む）
- `cpp/src/decoder/VideoDecoder.cpp`: カスタムio_openコールバック実装
- `tests/test_niconico_compat.py`: 互換性テスト
- `tests/test_http_headers.py`: HTTPヘッダー機能テスト（Python）
- `cpp/tests/test_hls_http_headers.cpp`: HLS HTTPヘッダー伝播テスト（C++）

### C++ 構造体

```cpp
// cpp/include/ytdlpspout/ytdlpspout.h
typedef struct YtdlpSpoutHttpHeader {
    const char* key;    // ヘッダーキー（例: "Cookie"）
    const char* value;  // ヘッダー値
} YtdlpSpoutHttpHeader;

// YtdlpSpoutConfigExに追加されたフィールド
const YtdlpSpoutHttpHeader* httpHeaders;  // HTTPヘッダー配列
int httpHeadersCount;                      // ヘッダー数
```

### メソッドリファレンス

#### YtDlpAsyncResolver

```python
# フォーマット文字列（クラス属性）
YtDlpAsyncResolver.FORMAT_STRING

# yt-dlpオプション取得
resolver._get_ydl_opts() -> dict

# Cookie情報ログ出力（内容は非公開）
resolver._log_cookie_info() -> None
```

#### YtdlpSpoutNative

```python
# 拡張設定で再生開始（HTTPヘッダー対応）
player.start_ex(
    source="url",
    http_headers={"Cookie": "session=abc123"}  # オプション
)
```

#### NativeStreamerWrapper

```python
# コンストラクタでHTTPヘッダーを受け取り
wrapper = NativeStreamerWrapper(
    video_url="url",
    sender_name="Test",
    pre_resolved_url="resolved_url",
    pre_resolved_headers={"Cookie": "..."}  # オプション
)
```

## デバッグログ

### HLS再生問題の調査

HLS再生が開始されない場合、以下のログが出力されます：

#### Python側（native_streamer_wrapper.py）

```
[Native] HTTPヘッダー数: 5
[Native] URL概要: https://delivery.domand.nicovideo.jp/.../playlist.m3u8?...
[Native] HLS判定: True
```

#### C++側（VideoPlayer.cpp）

```
HLS detection - IsHlsUrl: true, HlsSliceLoadingManager::IsHlsUrl: true, final: true
Source URL preview: https://delivery.domand.nicovideo.jp/...
Slice loading enabled in config: true
HTTP headers count: 5
```

#### C++側（HlsSliceLoadingManager.cpp）

```
Opening HLS URL: https://...
HTTP headers configured: 5 entries
Downloading m3u8 from: https://...
M3U8 downloaded successfully, size: 1234 bytes
Parsed playlist: 10 segments, total duration: 120.00s
First segment URL: https://...
First segment duration: 10.00s
Waiting for first segment to be available (timeout: 30000ms)...
First segment available after 150ms wait
```

#### C++側（ChunkDownloader.cpp）

```
ChunkDownloader: Starting download of segment 0 from https://...
ChunkDownloader: Configuring HTTP headers for segment 0 (5 headers available)
ChunkDownloader: Sending HTTP GET request for segment 0
ChunkDownloader: HTTP response for segment 0: status=200, success=true, size=123456, duration=150ms
ChunkDownloader: Segment 0 downloaded successfully (123456 bytes in 150ms, 822.4 KB/s)
```

### よくある問題と対処法

| ログメッセージ | 原因 | 対処法 |
|--------------|------|-------|
| `HLS判定: False` | URLに.m3u8が含まれていない | yt-dlp解決結果を確認 |
| `HTTP headers count: 0` | Cookieが渡されていない | cookie_file設定を確認 |
| `Failed to download m3u8` | ネットワークエラーまたは認証失敗 | Cookie/ネットワークを確認 |
| `No HTTP headers available` | ヘッダーが設定されていない | start_ex()の引数を確認 |
| `Timeout waiting for segment 0` | セグメントダウンロード失敗 | HTTPステータスを確認 |
| `status=403` | 認証エラー | Cookie有効期限を確認 |
