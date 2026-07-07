# ニコニコ動画対応ガイド

このドキュメントはニコニコ動画の再生サポートに関する技術的な詳細を説明します。

実装（C++/Python）を唯一の正としており、記載内容はすべて `file:行番号` で実装箇所を参照できるようにしています。

## 概要

ytdlpSpoutは、yt-dlpを使用してニコニコ動画（nicovideo.jp）からのストリーミング再生をサポートしています。

## フォーマット選択ロジック

### 背景

ニコニコ動画は、YouTubeとは異なる音声フォーマット（AAC）を使用することがあります。従来のフォーマット指定（`bestvideo[ext=mp4]+bestaudio[ext=m4a]`）では、AAC音声のみ提供される動画で失敗する可能性がありました。

### 改善されたフォーマット文字列

**一般サイト用（`FALLBACK_FORMATS` / `FORMAT_STRING`）**

```python
# python/ytdlp_resolver.py:114-123
FALLBACK_FORMATS: List[str] = [
    'bestvideo[ext=mp4]+bestaudio[ext=m4a]',
    'bestvideo[ext=mp4]+bestaudio[ext=aac]',
    'bestvideo+bestaudio',
    'bv*+ba',
    'b',
    'best',
]

FORMAT_STRING = '/'.join(FALLBACK_FORMATS)
```

**ニコニコ動画専用（単一フォーマット優先: `SINGLE_FORMAT_FIRST_ORDER`）**

ニコニコ（`nicovideo.jp` / `nico.ms` / `live.nicovideo.jp`）はHLSのみを提供するため、`bestvideo+bestaudio` で「Requested format is not available」になることがあります。対象ホストの場合は **単一フォーマット（`best`）を先に試す** 専用フォーマットを使用します。

```python
# python/ytdlp_resolver.py:126-139
SINGLE_FORMAT_FIRST_ORDER: List[str] = [
    'best',  # 単一最良フォーマット（m3u8等）を優先
    'bestvideo[ext=mp4]+bestaudio[ext=m4a]',
    'bestvideo[ext=mp4]+bestaudio[ext=aac]',
    'bestvideo+bestaudio',
    'bv*+ba',
    'b',
]
SINGLE_FORMAT_FIRST_FORMAT_STRING = '/'.join(SINGLE_FORMAT_FIRST_ORDER)

# 単一フォーマット優先を適用するホスト（HLS単体配信など）
_SINGLE_FORMAT_FIRST_HOSTS: frozenset = frozenset((
    'nicovideo.jp', 'nico.ms', 'live.nicovideo.jp',
))
```

`_get_ydl_opts(url)`（`python/ytdlp_resolver.py:273-297`）は、`url` が `_use_single_format_first(url)`（`python/ytdlp_resolver.py:224-232`、サブドメインは `endswith("." + host)` で一致判定）で真になる場合のみ `SINGLE_FORMAT_FIRST_FORMAT_STRING` を使用し、それ以外は `FORMAT_STRING` を使用します。

> 旧ドキュメントでは `NICONICO_FORMAT_ORDER` という変数名で記載していましたが、実装上の名称は `SINGLE_FORMAT_FIRST_ORDER` / `SINGLE_FORMAT_FIRST_FORMAT_STRING` です（ニコニコ専用ではなく「単一フォーマット優先が必要なホスト全般」向けの仕組み）。

### フォーマット選択の優先順位

**`_SINGLE_FORMAT_FIRST_HOSTS` に含まれるホスト（nicovideo.jp / nico.ms / live.nicovideo.jp）**

1. **best**: 単一最良フォーマット（m3u8等）を優先
2. 以降は一般フォーマットと同様（MP4+M4A → MP4+AAC → 任意+任意 → bv\*+ba/b → best）

**その他サイト**

1. **MP4 + M4A**: YouTube等で最も一般的な組み合わせ
2. **MP4 + AAC**: ニコニコ動画等で使用されるAAC音声
3. **任意のビデオ + 任意の音声**: コーデック制限なし
4. **bv\*+ba/b**: yt-dlp公式推奨の汎用フォーマット
5. **best**: 最終フォールバック

`NativeStreamerWrapper._get_fallback_formats()`（`python/native_streamer_wrapper.py:257-267`）はGUI側のログ表示用に `YtDlpAsyncResolver.FALLBACK_FORMATS` をそのまま返します（実際のyt-dlp解決は `YtDlpAsyncResolver` 側で行われます）。

## Cookie認証

### 必要性

ニコニコ動画では以下の場合にCookie認証が必要です：

- プレミアム会員限定動画
- 一部の年齢制限動画
- ログインユーザーのみ視聴可能な動画

### Refererヘッダー

ニコニコのCDNはRefererを要求する場合があります。`_REFERER_BY_HOST`（`python/ytdlp_resolver.py:142-146`）は `nicovideo.jp` / `nico.ms` / `live.nicovideo.jp` を `https://www.nicovideo.jp/` にマッピングしており、`resolve_sync()` は `_referer_for_url(url)`（`python/ytdlp_resolver.py:234-238`）の結果を **`http_headers` に既存の `Referer` がない場合のみ** 付与します（`python/ytdlp_resolver.py:470-473`）。

### Cookieファイルの配置

```
data/cookies.txt
```

Netscape形式のCookieファイルを配置してください。Cookieファイルが未指定の場合は `get_default_cookie_file()`（`python/ytdlp_resolver.py:154-171`）が `data/cookies.txt` → `cookies.txt` の順に自動探索します。

### Cookieの取得方法

1. ブラウザでニコニコ動画にログイン
2. ブラウザ拡張機能（例: Get cookies.txt）でCookieをエクスポート
3. `data/cookies.txt`に保存

### Cookieヘッダーのドメインスコープ

yt-dlpは `cookiefile` から読み込んだCookieを元に、解決したストリームURLへの `Cookie` ヘッダーを自動計算することがあります。`resolve_sync()`（`python/ytdlp_resolver.py:461-468`）はこれを尊重し、**yt-dlpが既に `Cookie` ヘッダーを設定済みの場合は上書きしません**。

`Cookie` ヘッダーが未設定の場合のみ、`_build_cookie_header(cookiejar, host)`（`python/ytdlp_resolver.py:251-271`）が `ydl.cookiejar` を走査し、`_cookie_domain_matches_host()`（`python/ytdlp_resolver.py:240-249`、先頭 `.` を除去した上でのホスト末尾一致）で **解決後のストリームURLのホストにドメインマッチするCookieのみ** を連結します。cookiejar全体を無条件に連結すると他サイトのCookieが漏えいするため、このドメインスコープ化が重要です。

### セキュリティ注意事項

- Cookieファイルの内容は**ログに出力されません**
- Cookieファイルはリポジトリにコミットしないでください（`.gitignore`に追加済み）

## HLSストリーム対応

ニコニコ動画はHLSプロトコル（`.m3u8`）でストリームを配信します。

### 対応状況

- C++ DLL（FFmpeg/libav）: HLS再生対応。**HLSスライスローディング**（主経路）と**FFmpegネイティブHLS**（フォールバック/代替経路）の2経路がある（詳細は後述）
- Python側: yt-dlpが解決したHLS URL・HTTPヘッダー・HLS判定結果（`protocol`由来）をC++ DLLに渡す

### HLS判定の優先順位（isHlsHint）

HLSかどうかの判定は、**yt-dlp側の判定結果（FFIヒント）を最優先**し、未指定の場合のみC++側のURLヒューリスティックにフォールバックします。

1. `ResolvedInfo.is_hls`（`python/ytdlp_resolver.py:57-69`）: yt-dlpが返す `protocol` に `'m3u8'` が含まれるかどうかで判定（`protocol` が空なら `None` ＝判定不能）
2. `gui.py:1338`: `pre_resolved_is_hls=result.is_hls` として `NativeStreamerWrapper` に渡す
3. `NativeStreamerWrapper._run()`（`python/native_streamer_wrapper.py:611-624`）: `start_ex(..., is_hls=self._pre_resolved_is_hls)` を呼び出す（ログ出力用に `.m3u8` を含むかの簡易判定も別途行うが、これはログ表示専用で実際の判定には使わない）
4. `YtdlpSpoutNative.start_ex()`（`python/ytdlpspout_native.py:675-678`）: `is_hls is None` なら `config.isHlsHint = -1`（自動判定）、それ以外は `1 if is_hls else 0` に変換
5. `c_api.cpp:592`: `playerConfig.isHlsHint = config->isHlsHint` としてC++の `PlayerConfig`（`cpp/src/player/VideoPlayer.h:68`）に伝搬
6. `VideoPlayer::Start()`（`cpp/src/player/VideoPlayer.cpp:162-169`）: `config.isHlsHint >= 0` ならヒントを最優先で採用し、`-1`（未指定）の場合のみ `hls::HlsSliceLoadingManager::IsHlsUrl(source)` のURLヒューリスティックで自動判定する

```cpp
// cpp/src/player/VideoPlayer.cpp:160-169
bool isHls;
if (config.isHlsHint >= 0) {
    isHls = (config.isHlsHint != 0);
    LOG_INFO("HLS detection - using FFI hint: {}", isHls);
} else {
    isHls = hls::HlsSliceLoadingManager::IsHlsUrl(source);
    LOG_INFO("HLS detection - HlsSliceLoadingManager::IsHlsUrl: {}", isHls);
}
```

`YtdlpSpoutConfigEx.isHlsHint`（`cpp/include/ytdlpspout/ytdlpspout.h:102`）の既定値は `-1`（自動判定、`c_api.cpp:539`）。

### HLS URL判定ロジック（IsHlsUrl、一本化済み）

URLヒューリスティック判定は `hls::HlsSliceLoadingManager::IsHlsUrl()` に一本化されており、`VideoPlayer.cpp:167` と `VideoDecoder.cpp:174` の両方がこの同一関数を呼び出します（重複実装はありません）。

```cpp
// cpp/src/hls/HlsSliceLoadingManager.cpp:641-673（要旨）
bool HlsSliceLoadingManager::IsHlsUrl(const std::string& url) {
    if (url.empty()) return false;
    std::string lower = Impl::ToLower(url);

    if (lower.find(".m3u8") != std::string::npos) return true;
    if (lower.find(".m3u") != std::string::npos) return true;

    // "format=m3u8" は値の誤検知（例: "format=m3u8xxx"）を避けるため、
    // 値の終端（'&'・'#'・文字列末尾）まで確認する
    size_t formatPos = lower.find("format=m3u8");
    if (formatPos != std::string::npos) {
        size_t afterPos = formatPos + std::string("format=m3u8").length();
        if (afterPos >= lower.size() || lower[afterPos] == '&' || lower[afterPos] == '#') {
            return true;
        }
    }

    // MIMEタイプでHLSを指定しているケース
    if (lower.find("mime=application%2fvnd.apple.mpegurl") != std::string::npos) return true;

    return false;
}
```

> 旧ドキュメントに記載していた `/hls/` パスによる判定は**現在の実装には存在しません**。`.m3u8` / `.m3u` / `format=m3u8`（境界チェック付き） / MIMEタイプ指定の4条件のみです。

### スライス読み込みとFFmpegネイティブの使い分け

HLS再生には次の2経路があります（`VideoPlayer::Start()`, `cpp/src/player/VideoPlayer.cpp:186-287`）。

| 経路 | 条件 | ログ例 | シーク |
|------|------|--------|--------|
| **HLSスライス読み込み** | `slice.enabled=true` かつ `HlsSliceLoadingManager::Open()` が成功 | `Using HLS slice loading with AVIOContext for:`（`VideoPlayer.cpp:229`） | 自前のセグメント選択で対応 |
| **FFmpegネイティブHLS** | 上記が失敗したときのフォールバック、または `slice.enabled=false` | `HLS slice loading failed, falling back to FFmpeg native HLS`（`VideoPlayer.cpp:205`）または `Using FFmpeg native HLS (no HLS slice loading for this source)`（`VideoPlayer.cpp:281`） | FFmpegのHLSデマクサでシーク |

`HlsSliceLoadingManager::Open()` は、スライスローダーが正しく扱えないHLSの特性を検出すると **明示的に `false` を返し、呼び出し元（`VideoPlayer::Start()`）にFFmpegネイティブHLSへのフォールバックを促します**（`cpp/src/hls/HlsSliceLoadingManager.cpp:295-315`）。具体的なフォールバック条件は以下の3つです。

1. **ライブプレイリスト**: `#EXT-X-ENDLIST` が無い（`playlist.isLive == true`）（`HlsSliceLoadingManager.cpp:298-302`）
2. **AES-128以外の暗号化方式**: 例えば SAMPLE-AES（`playlist.encryptionKey->method != "AES-128"`）（`HlsSliceLoadingManager.cpp:304-309`）
3. **鍵ローテーション**: プレイリスト途中で `#EXT-X-KEY` が変化する（`playlist.hasKeyRotation == true`）（`HlsSliceLoadingManager.cpp:311-315`）

ニコニコのm3u8はDMS等で構造が異なる場合があり、上記条件に該当して `HlsSliceLoadingManager::Open()` が失敗し **FFmpegネイティブにフォールバック** することがあります。その場合でも再生・シークはFFmpeg側で行われるため、問題なく動作します。どちらの経路かは上記ログで判別できます。

### FFmpegネイティブHLSフォールバック時のCustomIOContext不使用

この節で説明する「CustomIOContext不使用・FFmpegネイティブHTTPハンドラの直接使用」は、`VideoDecoder::Open()`（`cpp/src/decoder/VideoDecoder.cpp:159-272`）が呼ばれる経路、つまり

- ローカルファイル再生
- 非HLSでスライス読み込みが無効/失敗した場合
- **HLSスライスローディングが無効、または失敗してFFmpegネイティブHLSにフォールバックした場合**

に該当します。HLSスライスローディングが成功する主経路（前節）では `HlsSliceLoadingManager` が独自にHTTPリクエスト（`ChunkDownloader`/`HttpClient` 経由）を行い、その結果を `HlsCustomAVIOContext` 経由でFFmpegに渡す（`VideoDecoder::OpenWithAVIOContext()`, `cpp/src/decoder/VideoDecoder.cpp:609-663`）ため、`CustomIOContext` もFFmpegネイティブHTTPハンドラも使用しません。

`VideoDecoder::Open()` 内部では、FFmpegのHLSデマクサが内部的に複数のHTTPリクエスト（マニフェスト、キーファイル、セグメント）を実行します。これらの内部リクエストにHTTPヘッダー（Cookie等）を継承させるため、**この経路ではCustomIOContextを使用せず、FFmpegのネイティブHTTPハンドラ＋カスタム`io_open`コールバックを使用します**。

#### 処理の分岐（VideoDecoder::Open()内部、`cpp/src/decoder/VideoDecoder.cpp:159-272`）

```
VideoDecoder::Open(filePath, ...) 呼び出し
    ↓
isHls = HlsSliceLoadingManager::IsHlsUrl(filePath) で再判定
    ↓
┌─────────────────────────────────────────────────────┐
│ isUrl && !isHls （非HLSのHTTP(S) URL）の場合:        │
│   → io::CustomIOContext を使用                       │
│   → HTTPヘッダーはAVDictionaryの"headers"オプションで │
│     avformat_open_input()に渡す                       │
├─────────────────────────────────────────────────────┤
│ ローカルファイル、または isHls（HLS）の場合:          │
│   → CustomIOContext不使用                            │
│   → FFmpegネイティブHTTPハンドラで直接オープン        │
│   → isHls && ヘッダーありの場合のみカスタムio_open     │
│     コールバックを設定し、内部リクエストにも継承       │
└─────────────────────────────────────────────────────┘
```

> 注意: 上記は `VideoDecoder::Open()` 単体の分岐です。HLSスライスローディング成功時に使う `OpenWithAVIOContext()` や、非HLSスライス読み込み成功時に使う `OpenWithCustomIO()`（`cpp/src/decoder/VideoDecoder.cpp:407-472`）はどちらも外部から供給済みのAVIOContextをアタッチするだけで、この分岐やカスタム`io_open`コールバックの設定は行いません（そもそもFFmpeg自身がHTTPリクエストを発行しないため不要）。

#### protocol_whitelistの設定

暗号化HLS（AES-128）に対応するため、`crypto`プロトコルをホワイトリストに追加：

```cpp
// cpp/src/decoder/VideoDecoder.cpp:194
av_dict_set(&opts, "protocol_whitelist", "file,http,https,tcp,tls,crypto,data", 0);
```

### 技術的詳細

```
ニコニコ動画URL
    ↓ yt-dlp解決（Referer/Cookie付与、is_hls判定）
HLS manifest URL (.m3u8) + HTTPヘッダー + isHlsヒント
    ↓ C++ DLL
HlsSliceLoadingManager（成功）/ FFmpegネイティブHLS（フォールバック）
    ↓
動画フレーム
```

## トラブルシューティング

### エラー: "Requested format is not available"

**原因**: 指定したフォーマットが利用できない

**解決策**: フォーマット文字列が自動的にフォールバックを試みます（ニコニコ系ホストは `SINGLE_FORMAT_FIRST_FORMAT_STRING` を使用）

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
    ↓ ResolvedInfo.http_headers / ResolvedInfo.is_hls
NativeStreamerWrapper
    ↓ pre_resolved_headers / pre_resolved_is_hls 引数
YtdlpSpoutNative.start_ex()
    ↓ http_headers引数 (dict → C配列) / is_hls引数 (bool|None → isHlsHint)
C++ DLL (PlayerConfig.httpHeaders / PlayerConfig.isHlsHint)
    ↓
    ┌─ isHls=true  → HlsSliceLoadingManager → HlsCustomAVIOContext ─┐
    └─ isHls=false → SliceLoadingManager    → CustomIOContext ──────┘
    ↓
┌─────────────────────────────────────────────────────────────────┐
│ HttpClient / ChunkDownloader（HLS・非HLS両経路で共用）           │ ← ヘッダー適用
│  - 非HLS: CustomIOContext::InitializeHttp()内のHEADリクエスト     │
│  - 両経路: ChunkDownloaderのGETリクエスト（セグメント/チャンク）  │
└─────────────────────────────────────────────────────────────────┘
    ↓
HTTP (Range) Request with headers
```

HLSスライスローディングと非HLSスライス読み込みは、どちらも内部で同じ `io::ChunkDownloader` / `io::HttpClient`（`cpp/src/hls/HlsSliceLoadingManager.cpp:10-11`, `cpp/src/io/ChunkDownloader.cpp`）を使ってセグメント/チャンクをダウンロードします。違いはFFmpegに渡すAVIOContextの実装（`HlsCustomAVIOContext` vs `io::CustomIOContext`）と、非HLS側にのみ存在するContent-Length取得用HEADリクエスト（`CustomIOContext::InitializeHttp()`, `cpp/src/io/CustomIOContext.cpp:188-242`）の有無です。

### C++ DLL内のHTTPヘッダー適用箇所

HTTPヘッダーは以下のHTTPリクエストに適用されます：

1. **`CustomIOContext::InitializeHttp()`内のHEADリクエスト**（非HLSスライス読み込み経路のみ、`cpp/src/io/CustomIOContext.cpp:188-242`）
   - Content-Length取得用の初期HEADリクエスト
   - HttpClientConfig.headersにヘッダーを設定してから実行

2. **`ChunkDownloader::DownloadSegment()`内のGETリクエスト**（HLS・非HLS両経路で共用、`cpp/src/io/ChunkDownloader.cpp:424-500`）
   - 各ワーカースレッドがHttpClientを作成する際にヘッダーを設定
   - Range Requestでチャンク／HLSセグメントをダウンロード
   - `SetHttpHeaders()`（`ChunkDownloader.cpp:548`）で設定されたヘッダーは`DownloadSegment()`呼び出し時に毎回適用されるため、ワーカースレッド起動後にヘッダーを設定しても正しく反映される

3. **`VideoDecoder::Open()`内のカスタム`io_open`コールバック経由のFFmpegリクエスト**（FFmpegネイティブHLSフォールバック限定、`cpp/src/decoder/VideoDecoder.cpp:159-272`）
   - HLSスライスローディングが無効/失敗し、FFmpegが自前でHLSを解釈する場合にのみ適用される
   - キーファイル（.key）、セグメント（.ts）取得にヘッダーが継承される
   - `avformat_open_input()`のAVDictionary経由で`headers`オプションを設定し、内部リクエストには`io_open`コールバックで伝播する
   - `OpenWithCustomIO()` / `OpenWithAVIOContext()`（HLSスライスローディング成功時・非HLSスライス読み込み成功時に使用）は外部AVIOContextをアタッチするだけで、FFmpeg自身はHTTPリクエストを発行しないため、この`io_open`コールバックは設定されない

### HLSストリームでのHTTPヘッダー適用（FFmpegネイティブHLSフォールバック時）

HLSスライスローディングが無効または失敗し、`VideoDecoder::Open()` がFFmpegネイティブHLSとして開く場合、FFmpegのHLSデマクサは内部的に以下のHTTPリクエストを実行します：

```
メインマニフェスト (.m3u8)
    ↓ HTTPヘッダー適用
キーファイル取得 (.key)  ← 暗号化されたHLSの場合
    ↓ HTTPヘッダー適用
セグメント取得 (.ts)
    ↓ HTTPヘッダー適用
次のセグメント...
```

これらすべての内部リクエストに、`VideoDecoder::Open()`で設定したHTTPヘッダーが適用されます。HLSスライスローディングが成功する主経路では、ヘッダー適用は`ChunkDownloader`側（前節2.）が担当します。

#### 技術的実装

**カスタムio_openコールバック**

FFmpegのHLSデマクサは、デフォルトでは`avformat_open_input()`に渡したHTTPヘッダーを内部リクエスト（セグメント、キーファイル）に自動的に継承しません。この問題を解決するため、`AVFormatContext::io_open`コールバックをカスタマイズしています：

```cpp
// cpp/src/decoder/VideoDecoder.cpp:44-81

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

// Open()メソッド内でHLSストリームの場合に設定（cpp/src/decoder/VideoDecoder.cpp:241-262）
if (isHls && !httpHeaders.empty()) {
    m_impl->formatCtx = avformat_alloc_context();

    // HTTPヘッダーコンテキストを作成（unique_ptrで所有、Close時に自動解放）
    m_impl->httpHeaderCtx = std::make_unique<HttpHeaderContext>();
    for (const auto& [key, value] : httpHeaders) {
        m_impl->httpHeaderCtx->headers += key + ": " + value + "\r\n";
    }

    // カスタムio_openコールバックを設定
    m_impl->formatCtx->opaque = m_impl->httpHeaderCtx.get();
    m_impl->formatCtx->io_open = custom_io_open;
}
```

**ポイント:**
- `ffio_open_whitelist`は内部APIのため、`avio_open2`を使用
- `AVFormatContext::opaque`にヘッダーコンテキストを格納
- `httpHeaderCtx`は`std::unique_ptr`で保持しており、`Open()`の失敗パスや`Close()`時に確実に解放されメモリリークを防止する（`cpp/src/decoder/VideoDecoder.cpp:124, 269, 859`）

### 使用方法

```python
from python.ytdlp_resolver import YtDlpAsyncResolver, ResolvedInfo
from python.native_streamer_wrapper import NativeStreamerWrapper

# URL解決
resolver = YtDlpAsyncResolver(cookie_file="data/cookies.txt")
result = resolver.resolve_sync(url)  # ResolvedInfo

# ストリーマー作成時にヘッダー・HLS判定結果を渡す
wrapper = NativeStreamerWrapper(
    video_url=url,
    sender_name="ytdlpSpout",
    pre_resolved_url=result.stream_url,
    pre_resolved_headers=result.http_headers,  # Cookie等のヘッダー
    pre_resolved_is_hls=result.is_hls          # yt-dlpのprotocolに基づくHLS判定（None=C++側で自動判定）
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

上記3ホスト（`nicovideo.jp` / `nico.ms` / `live.nicovideo.jp`）は、`YTDLP_DOMAINS`（`python/ytdlp_resolver.py:87`、`is_ytdlp_url()`がyt-dlp解決を優先する既知ドメイン）・`_SINGLE_FORMAT_FIRST_HOSTS`（単一フォーマット優先）・`_REFERER_BY_HOST`（Referer付与）の3つの定数すべてに共通して含まれています。

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

### 解決策（現行実装）

`HlsSliceLoadingManager::Open()` は、先頭セグメントを Critical 優先度でリクエストした後、`HlsSegmentCache::WaitForSegment()` で**条件変数（`std::condition_variable`）ベースの待機**を行います（100msポーリングではありません）。セグメントのダウンロード完了時・恒久失敗マーク時のいずれでも `notify_all()` により即座に起床するため、ポーリング間隔ぶんの無駄な遅延がありません（`cpp/src/hls/HlsSegmentCache.cpp:453-519` の `WaitForSegment`、`cpp/src/hls/HlsSegmentCache.cpp:525-` の `MarkSegmentFailed`）。

fMP4（初期化セグメントを持つ）プレイリストの場合は、セグメント0本体だけでなく**初期化セグメント（segmentIndex = -1）**の到着も待機します。

```cpp
// cpp/src/hls/HlsSliceLoadingManager.cpp:515-568（要旨）

m_impl->isOpen = true;

// 先頭セグメントをCritical優先度でリクエスト
m_impl->downloader->RequestSegment(firstSegment.url, 0, io::ChunkPriority::Critical,
                                    firstSegment.byteRangeStart, firstSegment.byteRangeLength);

const int waitTimeoutMs = config.readTimeoutMs > 0 ? config.readTimeoutMs : 30000;

// fMP4の場合は初期化セグメント（index -1）も待機
if (m_impl->playlist.map.has_value()) {
    if (!m_impl->cache->WaitForSegment(-1, waitTimeoutMs)) {
        m_impl->isOpen = false;
        return false;  // 初期化セグメント取得失敗はOpen失敗として扱う
    }
}

// 先頭セグメント（index 0）を条件変数で待機（ポーリングではない）
if (!m_impl->cache->WaitForSegment(0, waitTimeoutMs)) {
    m_impl->isOpen = false;
    return false;  // タイムアウト、または恒久失敗マーク済みならOpen失敗
}
```

`WaitForSegment()` はロック取得後、対象セグメントが既にキャッシュ済みなら即座に `true` を返し、既に「恒久失敗（`MarkSegmentFailed` 済み）」であればタイムアウトを待たずに即座に `false` を返します。それ以外はキャッシュ完了 or 失敗マークのいずれかが起きるまで `condition_variable::wait_for`（タイムアウト付き）でブロックします（`cpp/src/hls/HlsSegmentCache.cpp:453-519`）。

### 失敗マークとフォールバック（自動再ダウンロード）

セグメントのダウンロードが恒久的に失敗した場合、`HlsSegmentCache::MarkSegmentFailed()` が呼ばれ、待機中の `WaitForSegment` を即座に起床させます（`Open()` 側はここでタイムアウト同様に失敗として扱います）。

さらに、再生中に `HlsCustomAVIOContext::ReadPacket()` が恒久失敗セグメントを検出した場合は、`HlsSliceLoadingManager` に設定されたコールバック（`SetOnSegmentRetryCallback`, `cpp/src/hls/HlsSliceLoadingManager.cpp:468-513`）経由で **1回だけ自動的に失敗マークをクリアして Critical 優先度で再ダウンロードを要求**するフォールバックがあります。対象は通常セグメントだけでなく、fMP4 の初期化セグメント（index -1）も含みます。

### 処理フロー

```
HlsSliceLoadingManager::Open()
    ↓
1. m3u8ダウンロード・パース（マスタープレイリストの場合は最適バリアントを再取得）
2. フォールバック条件チェック（ライブ/AES-128以外の暗号化/鍵ローテーション → 該当時はOpen()失敗）
3. セグメントキャッシュ初期化
4. ChunkDownloader開始
5. HlsCustomAVIOContext初期化（SetOnSegmentRetryCallbackで再ダウンロードフォールバックを登録）
6. 先頭セグメント（0）をCritical優先度でリクエスト
7. （fMP4の場合）初期化セグメント（-1）をWaitForSegmentで待機（条件変数、ポーリングではない）
8. 先頭セグメント（0）をWaitForSegmentで待機
    ↓
いずれかがタイムアウト or 恒久失敗マーク済み → Open()失敗（false）→ VideoPlayer側でFFmpegネイティブHLSにフォールバック
セグメントが利用可能になった → Open()完了
    ↓
VideoDecoder::OpenWithAVIOContext()
    ↓
avformat_find_stream_info() → ReadPacket() → セグメントを取得（即座に利用可能）
    ↓
（再生中に恒久失敗を検出した場合）SetOnSegmentRetryCallback → 1回だけ自動再ダウンロード
```

### 関連ファイル

- `python/ytdlp_resolver.py`: URL解決、フォーマット選択ロジック、Referer/Cookieドメインスコープ、`ResolvedInfo.is_hls`
- `python/native_streamer_wrapper.py`: ストリーミングラッパー（`pre_resolved_url`/`pre_resolved_headers`/`pre_resolved_is_hls`の受け渡し）
- `python/ytdlpspout_native.py`: C++ DLLバインディング（HTTPヘッダー構造体、`is_hls`→`isHlsHint`変換を含む）
- `gui.py`: `pre_resolved_is_hls=result.is_hls` の受け渡し元
- `cpp/include/ytdlpspout/ytdlpspout.h`: `YtdlpSpoutHttpHeader` / `YtdlpSpoutConfigEx.isHlsHint` の定義
- `cpp/src/bindings/c_api.cpp`: `isHlsHint` の初期化・`PlayerConfig`への伝搬
- `cpp/src/player/VideoPlayer.cpp`: `isHlsHint`優先のHLS判定、HLSスライスローディング/FFmpegネイティブHLSの分岐とフォールバック
- `cpp/src/decoder/VideoDecoder.cpp`: カスタムio_openコールバック実装（FFmpegネイティブHLSフォールバック限定）
- `cpp/src/hls/HlsSliceLoadingManager.cpp`: `IsHlsUrl()`（URL判定の単一ソース）、`Open()` の待機処理・フォールバック条件判定、失敗時の自動再ダウンロードコールバック
- `cpp/src/hls/HlsSegmentCache.cpp`: `WaitForSegment()`（条件変数ベース待機）、`MarkSegmentFailed()`/`IsSegmentFailed()`/`ClearSegmentFailed()`
- `cpp/src/io/ChunkDownloader.cpp` / `cpp/src/io/CustomIOContext.cpp`: HTTPヘッダー適用箇所（HEADリクエスト・GETリクエスト）
- `tests/test_niconico_compat.py`: 互換性テスト
- `tests/test_http_headers.py`: HTTPヘッダー機能テスト（Python）
- `cpp/tests/test_hls_http_headers.cpp`: HLS HTTPヘッダー伝播テスト（C++）
- `cpp/tests/test_hls_slice_loading_manager.cpp`: `HlsSliceLoadingManager` の待機・失敗フォールバックのテスト

### C++ 構造体

```cpp
// cpp/include/ytdlpspout/ytdlpspout.h:83-103
typedef struct YtdlpSpoutHttpHeader {
    const char* key;    // ヘッダーキー（例: "Cookie"）
    const char* value;  // ヘッダー値
} YtdlpSpoutHttpHeader;

// YtdlpSpoutConfigExに追加されたフィールド
const YtdlpSpoutHttpHeader* httpHeaders;  // HTTPヘッダー配列
int httpHeadersCount;                      // ヘッダー数
int isHlsHint;                             // HLS判定ヒント（-1=自動判定、0=非HLS、1=HLS）
```

### メソッドリファレンス

#### YtDlpAsyncResolver

```python
# フォーマット文字列（クラス属性）
YtDlpAsyncResolver.FORMAT_STRING
YtDlpAsyncResolver.SINGLE_FORMAT_FIRST_FORMAT_STRING

# yt-dlpオプション取得（urlを渡すとホストに応じてフォーマット文字列を切り替える）
resolver._get_ydl_opts(url=None) -> dict

# Cookie情報ログ出力（内容は非公開）
resolver._log_cookie_info() -> None
```

#### YtdlpSpoutNative

```python
# 拡張設定で再生開始（HTTPヘッダー・HLS判定ヒント対応）
player.start_ex(
    source="url",
    http_headers={"Cookie": "session=abc123"},  # オプション
    is_hls=True  # オプション（None=C++側で自動判定）
)
```

#### NativeStreamerWrapper

```python
# コンストラクタでHTTPヘッダー・HLS判定結果を受け取り
wrapper = NativeStreamerWrapper(
    video_url="url",
    sender_name="Test",
    pre_resolved_url="resolved_url",
    pre_resolved_headers={"Cookie": "..."},  # オプション
    pre_resolved_is_hls=True                 # オプション（None=C++側で自動判定）
)
```

## デバッグログ

### HLS再生問題の調査

HLS再生が開始されない場合、以下のログが出力されます：

#### Python側（native_streamer_wrapper.py）

```
[Native] HTTPヘッダー数: 5
[Native] URL概要: https://delivery.domand.nicovideo.jp/.../playlist.m3u8?...
[Native] HLS判定: True (yt-dlp解決結果)
```

（`pre_resolved_is_hls` が `None` の場合は `[Native] HLS判定: True`（`.m3u8`を含むかの簡易判定、`python/native_streamer_wrapper.py:609-614`）になります）

#### C++側（VideoPlayer.cpp）

```
HLS detection - using FFI hint: true
Source URL preview: https://delivery.domand.nicovideo.jp/...
Slice loading enabled in config: true
HTTP headers count: 5
```

（yt-dlp側の判定結果（isHlsHint）が未指定の場合は代わりに `HLS detection - HlsSliceLoadingManager::IsHlsUrl: true` が出力されます）

#### C++側（HlsSliceLoadingManager.cpp）

```
Opening HLS URL: https://...
HTTP headers configured: 5 entries
Downloading m3u8 from: https://...
M3U8 downloaded successfully, size: 1234 bytes
Parsed playlist: 10 segments, total duration: 120.00s
Requesting first segment with highest priority...
First segment URL: https://...
First segment duration: 10.00s
First segment request queued
Waiting for first segment to be available (timeout: 30000ms)...
First segment available after 150ms wait
HLS stream opened successfully: 10 segments
```

#### C++側（ChunkDownloader.cpp）

```
ChunkDownloader: Starting download of segment 0 from https://...
ChunkDownloader: Configuring HTTP headers for segment 0 (5 headers available)
ChunkDownloader: Sending HTTP request for segment 0 (Range: 0-1048575)
ChunkDownloader: HTTP response for segment 0: status=200, success=true, size=123456, duration=150ms
ChunkDownloader: Segment 0 downloaded successfully (123456 bytes in 150ms, 822.4 KB/s)
```

### よくある問題と対処法

| ログメッセージ | 原因 | 対処法 |
|--------------|------|-------|
| `HLS判定: False` | URLに.m3u8が含まれていない（かつyt-dlp側でもHLSと判定されていない） | yt-dlp解決結果（`protocol`）を確認 |
| `HTTP headers count: 0` | Cookieが渡されていない | cookie_file設定を確認 |
| `Failed to download m3u8` | ネットワークエラーまたは認証失敗 | Cookie/ネットワークを確認 |
| `ChunkDownloader: No HTTP headers available for segment N` | ヘッダーが設定されていない | start_ex()の引数を確認 |
| `First segment not available after Nms wait` | セグメントダウンロード失敗またはタイムアウト | HTTPステータス・タイムアウト設定を確認 |
| `status=403` | 認証エラー | Cookie有効期限を確認 |
