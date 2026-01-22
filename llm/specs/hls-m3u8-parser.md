# HLS (HTTP Live Streaming) パーサー実装

## 概要

HLS m3u8プレイリストをパースし、セグメント情報を抽出するためのパーサーモジュール。

## ファイル構成

```
cpp/src/hls/
├── M3U8Parser.h    # ヘッダーファイル（データ構造・API定義）
└── M3U8Parser.cpp  # 実装
```

## データ構造

### HlsSegment

```cpp
struct HlsSegment {
    int64_t index;           // プレイリスト内のインデックス（0から）
    std::string url;         // 絶対URL
    double duration;         // セグメント長（秒）
    int64_t byteRangeStart;  // バイト範囲開始位置（-1 = 未指定）
    int64_t byteRangeLength; // バイト範囲長
    int64_t mediaSequence;   // メディアシーケンス番号
};
```

### HlsEncryptionKey

```cpp
struct HlsEncryptionKey {
    std::string method;        // "NONE", "AES-128", "SAMPLE-AES"
    std::string keyUrl;        // キーファイルURL
    std::vector<uint8_t> iv;   // 初期化ベクトル（16バイト）
};
```

### M3U8Playlist

```cpp
struct M3U8Playlist {
    int version;                          // プレイリストバージョン
    double targetDuration;                // 最大セグメント長
    int64_t mediaSequence;                // 開始シーケンス番号
    bool isEndList;                       // #EXT-X-ENDLIST存在
    bool isLive;                          // ライブストリーム判定
    std::vector<HlsSegment> segments;     // セグメントリスト
    std::optional<HlsEncryptionKey> encryptionKey;
    double totalDuration;                 // 合計時間
};
```

## API

### Parse

```cpp
static std::optional<M3U8Playlist> M3U8Parser::Parse(
    const std::string& m3u8Content,
    const std::string& baseUrl
);
```

m3u8コンテンツをパースし、プレイリスト情報を返す。

**パラメータ:**
- `m3u8Content`: m3u8ファイルの内容（UTF-8文字列）
- `baseUrl`: 相対URL解決用のベースURL

**戻り値:**
- `std::optional<M3U8Playlist>`: 成功時はプレイリスト情報、失敗時は`nullopt`

**失敗条件:**
- 空のコンテンツ
- `#EXTM3U`タグがない

### ResolveUrl

```cpp
static std::string M3U8Parser::ResolveUrl(
    const std::string& baseUrl,
    const std::string& relativeUrl
);
```

相対URLを絶対URLに変換する。

**対応パターン:**
- 相対パス: `segment0.ts` → `https://example.com/video/segment0.ts`
- 親ディレクトリ: `../other/seg.ts` → `https://example.com/other/seg.ts`
- ルート相対: `/absolute/seg.ts` → `https://example.com/absolute/seg.ts`
- 絶対URL: 変更なしで返す

## 対応タグ

| タグ | 説明 | 例 |
|------|------|-----|
| `#EXTM3U` | プレイリスト識別（必須） | `#EXTM3U` |
| `#EXT-X-VERSION` | バージョン | `#EXT-X-VERSION:3` |
| `#EXT-X-TARGETDURATION` | 最大セグメント長 | `#EXT-X-TARGETDURATION:10` |
| `#EXT-X-MEDIA-SEQUENCE` | 開始シーケンス番号 | `#EXT-X-MEDIA-SEQUENCE:100` |
| `#EXTINF` | セグメント情報 | `#EXTINF:9.009,` |
| `#EXT-X-KEY` | 暗号化キー | `#EXT-X-KEY:METHOD=AES-128,URI="...",IV=0x...` |
| `#EXT-X-BYTERANGE` | バイト範囲 | `#EXT-X-BYTERANGE:500000@0` |
| `#EXT-X-ENDLIST` | VOD終端マーカー | `#EXT-X-ENDLIST` |

## 使用例

```cpp
#include "hls/M3U8Parser.h"

using namespace ytdlpspout::hls;

// m3u8コンテンツを取得（HttpClientなどで）
std::string content = R"(#EXTM3U
#EXT-X-VERSION:3
#EXT-X-TARGETDURATION:10
#EXT-X-MEDIA-SEQUENCE:0
#EXTINF:9.009,
segment0.ts
#EXTINF:9.009,
segment1.ts
#EXT-X-ENDLIST
)";

std::string baseUrl = "https://example.com/video/playlist.m3u8";

auto playlist = M3U8Parser::Parse(content, baseUrl);
if (playlist) {
    LOG_INFO("Total segments: {}", playlist->segments.size());
    LOG_INFO("Total duration: {:.2f}s", playlist->totalDuration);
    LOG_INFO("Is live: {}", playlist->isLive ? "yes" : "no");
    
    for (const auto& seg : playlist->segments) {
        LOG_DEBUG("Segment {}: {} ({:.3f}s)", 
                  seg.mediaSequence, seg.url, seg.duration);
    }
}
```

## ライブ/VOD判定

- **VOD**: `#EXT-X-ENDLIST` が存在
- **ライブ**: `#EXT-X-ENDLIST` が存在しない

```cpp
playlist->isEndList  // true = VOD終端マーカーあり
playlist->isLive     // true = ライブストリーム（ENDLISTなし）
```

## 暗号化対応

AES-128暗号化されたストリームの場合：

```cpp
if (playlist->encryptionKey) {
    const auto& key = playlist->encryptionKey.value();
    LOG_INFO("Encryption method: {}", key.method);
    LOG_INFO("Key URL: {}", key.keyUrl);
    
    // IVは16バイトの配列
    if (!key.iv.empty()) {
        // IVを使用してセグメントを復号
    }
}
```

**注意**: 実際の復号処理は別途実装が必要。パーサーはメタデータの抽出のみ。

## バイト範囲対応

同一ファイル内の複数セグメント（fMP4など）：

```cpp
for (const auto& seg : playlist->segments) {
    if (seg.byteRangeStart >= 0) {
        // Range Request用のヘッダー
        std::string range = fmt::format("bytes={}-{}", 
            seg.byteRangeStart, 
            seg.byteRangeStart + seg.byteRangeLength - 1);
    }
}
```

## 実装上の注意

### 行分割
- `\n` (Unix) と `\r\n` (Windows) の両方に対応
- 空行はスキップ

### 大文字小文字
- タグは大文字小文字を区別しない（安全のため）
- 内部で大文字に正規化して比較

### シーケンス番号
- `mediaSequence` は `#EXT-X-MEDIA-SEQUENCE` + セグメントインデックス
- ライブストリームでは重要（同一シーケンスを再取得しないため）

### エラー処理
- 不正なm3u8は `nullopt` を返す
- 個々のタグのパースエラーは警告ログを出力して続行

## テスト

```bash
# ビルド
cd cpp/build
cmake --build . --target test_m3u8_parser --config Debug

# 実行
./bin/Debug/test_m3u8_parser.exe
```

テストケース：
1. シンプルなVODプレイリスト
2. AES-128暗号化付き
3. バイト範囲指定
4. ライブストリーム（ENDLISTなし）
5. CRLF改行
6. 不正なm3u8（#EXTMなし）
7. 相対URL解決
8. 大文字小文字混在タグ
9. 複数のEXT-X-KEY
10. オフセット省略のバイト範囲

## 今後の拡張

- [ ] マスタープレイリスト（#EXT-X-STREAM-INF）対応
- [ ] 字幕トラック（#EXT-X-MEDIA）対応
- [ ] SAMPLE-AES復号対応
- [ ] プレイリスト更新（ライブストリーム用）

## AES-128-CBC復号ユーティリティ

### 概要

HLSセグメントのAES-128-CBC暗号化を復号するためのユーティリティクラス。Windows BCrypt APIを使用。

### ファイル構成

```
cpp/src/hls/
├── AesCbcDecryptor.h    # ヘッダーファイル
└── AesCbcDecryptor.cpp  # 実装
```

### API

```cpp
class AesCbcDecryptor {
public:
    /// 16バイトのAES-128キーで初期化
    bool Initialize(const std::vector<uint8_t>& key);
    
    /// リソース解放
    void Close();
    
    /// AES-128-CBC復号（PKCS7パディング除去済み）
    std::vector<uint8_t> Decrypt(
        const std::vector<uint8_t>& ciphertext,
        const std::vector<uint8_t>& iv
    );
    
    /// HLS用: メディアシーケンス番号からIV生成
    static std::vector<uint8_t> GenerateIvFromSequence(int64_t mediaSequence);
    
    /// 16進文字列からIV解析（"0x"プレフィックス対応）
    static std::optional<std::vector<uint8_t>> ParseHexIv(const std::string& hexStr);
};
```

### 使用例

```cpp
#include "hls/AesCbcDecryptor.h"

// キーを取得（例: HTTPからダウンロード）
std::vector<uint8_t> keyData = ...; // 16バイト

AesCbcDecryptor decryptor;
if (!decryptor.Initialize(keyData)) {
    // エラー処理
}

// EXT-X-KEY の IV属性がある場合
auto iv = AesCbcDecryptor::ParseHexIv("0x00000000000000000000000000000001");

// IV属性がない場合はメディアシーケンス番号から生成
auto iv = AesCbcDecryptor::GenerateIvFromSequence(segment.mediaSequence);

// 復号
auto plaintext = decryptor.Decrypt(encryptedSegment, *iv);
if (plaintext.empty()) {
    // 復号失敗
}

decryptor.Close();
```

### IV生成ルール（HLS仕様 RFC 8216）

`#EXT-X-KEY` タグにIV属性がない場合、メディアシーケンス番号からIVを生成：

```
mediaSequence = 123
IV = [0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,123]  // ビッグエンディアン16バイト
```

### テスト

```bash
# ビルド
cd cpp
cmake --build build --target test_aes_decryptor --config Debug

# 実行
.\build\bin\Debug\test_aes_decryptor.exe
```

テストケース：
1. 初期化とクローズ
2. 不正なキー長でのエラー
3. NIST AES-128-CBCテストベクターでの復号
4. PKCS7パディング除去
5. 不正なIV長でのエラー
6. 空の暗号文
7. 非ブロックアライメント暗号文
8. シーケンス番号からのIV生成
9. 16進文字列のIV解析（0xプレフィックスあり/なし）
10. 複数回の連続復号

## HLSセグメントキャッシュ（HlsSegmentCache）

### ファイル構成

```
cpp/src/hls/
├── HlsSegmentCache.h    # ヘッダーファイル
└── HlsSegmentCache.cpp  # 実装
```

### 設定構造体

```cpp
struct HlsSegmentCacheConfig {
    size_t maxMemoryBytes = 256 * 1024 * 1024;  // 256MB
    size_t maxSegments = 100;
};
```

### API

```cpp
class HlsSegmentCache {
public:
    explicit HlsSegmentCache(const HlsSegmentCacheConfig& config);
    
    // 初期化
    void Initialize(const M3U8Playlist& playlist);
    void SetEncryptionKey(const std::vector<uint8_t>& keyData, 
                          const std::optional<std::vector<uint8_t>>& explicitIv = std::nullopt);
    
    // 読み書き
    bool WriteSegment(int64_t segmentIndex, std::vector<uint8_t>&& data, bool isEncrypted);
    std::optional<std::vector<uint8_t>> ReadSegment(int64_t segmentIndex, int timeoutMs = 0);
    
    // 状態確認
    bool IsSegmentCached(int64_t segmentIndex) const;
    const HlsSegment* GetSegmentInfo(int64_t index) const;
    size_t GetTotalSegmentCount() const;
    size_t GetCachedSegmentCount() const;
    size_t GetCacheMemoryUsage() const;
    
    // 時間変換
    int64_t GetSegmentIndexFromTime(double seconds) const;
    double GetSegmentStartTime(int64_t segmentIndex) const;
    
    // 最適化
    void OptimizeForPlayback(int64_t currentSegmentIndex, int64_t prefetchCount);
};
```

### LRUエビクション

- `maxMemoryBytes`を超えた場合、最も古いアクセスのセグメントを削除
- `ReadSegment()`呼び出しでLRU順序を更新
- `OptimizeForPlayback()`で再生位置周辺のセグメントを保護

### スレッドセーフティ

- `std::mutex`で全操作を保護
- 複数スレッドからの同時読み書きに対応
- 条件変数でセグメント到着を待機可能（`ReadSegment`のタイムアウト）

### 使用例

```cpp
HlsSegmentCacheConfig config;
config.maxMemoryBytes = 256 * 1024 * 1024;  // 256MB

HlsSegmentCache cache(config);
cache.Initialize(playlist);

// 暗号化キーを設定（必要な場合）
if (playlist.encryptionKey) {
    cache.SetEncryptionKey(downloadedKeyData);
}

// セグメントを書き込み
cache.WriteSegment(0, std::move(segmentData), isEncrypted);

// セグメントを読み取り（タイムアウト付き）
auto data = cache.ReadSegment(0, 1000);  // 1秒待機
if (data) {
    // データを使用
}
```

### テスト

```bash
# ビルド
cd cpp
cmake --build build --target test_hls_segment_cache --config Debug

# 実行
.\build\bin\Debug\test_hls_segment_cache.exe
```

テストケース：
1. 基本的な初期化
2. 基本的な書き込み・読み取り
3. LRUエビクション動作確認
4. AES復号との統合
5. 時間→セグメントインデックス変換
6. スレッドセーフティ（並行アクセス）
7. メモリ制限超過時の動作
8. セグメント情報取得
9. OptimizeForPlayback
10. タイムアウト付き読み取り
11. 無効なインデックスのハンドリング

---

## ChunkDownloaderのHLSセグメントダウンロード機能

### 概要

既存の`ChunkDownloader`クラスにHLSセグメント単位のダウンロード機能を追加。
Range Requestベースのチャンクダウンロードと、URL単位のセグメントダウンロードの両方をサポート。

### 追加API

```cpp
/// @brief HLSセグメントのダウンロード完了コールバック
/// @param segmentIndex セグメントインデックス
/// @param data ダウンロードしたデータ（失敗時は空）
/// @param success 成功フラグ
using SegmentDownloadCallback = std::function<void(int64_t segmentIndex, std::vector<uint8_t>&& data, bool success)>;

/// @brief セグメントダウンロード完了コールバックを設定
void SetSegmentDownloadCallback(SegmentDownloadCallback callback);

/// @brief HLSセグメントURLのダウンロードをリクエスト
/// @param url セグメントURL（完全URL）
/// @param segmentIndex セグメントインデックス
/// @param priority 優先度（Critical, High, Medium, Low）
void RequestSegment(const std::string& url, int64_t segmentIndex, ChunkPriority priority);

/// @brief キューに入っているセグメントの優先度を変更
void ReprioritizeSegment(int64_t segmentIndex, ChunkPriority newPriority);

/// @brief セグメントダウンロードキューをクリア
void ClearSegmentQueue();

/// @brief HTTPヘッダーを設定（全リクエストに適用）
void SetHttpHeaders(const std::map<std::string, std::string>& headers);
```

### 内部実装

#### SegmentRequest構造体
```cpp
struct SegmentRequest {
    std::string url;
    int64_t segmentIndex;
    ChunkPriority priority;
    std::chrono::steady_clock::time_point requestTime;
};
```

### 動作仕様
1. `RequestSegment()`でセグメントキューに追加
2. ワーカースレッドがURLをダウンロード（Range Requestなし、セグメント全体を取得）
3. ダウンロード完了時にコールバック呼び出し
4. 優先度順に処理（Critical > High > Medium > Low）
5. チャンクリクエストとセグメントリクエストは優先度で比較され、高い方から処理

### 使用例

```cpp
#include "io/ChunkDownloader.h"
#include "io/SparseFileCache.h"

// キャッシュ初期化
SparseFileCache cache(config);
cache.Initialize(fileSize);

// ダウンローダー初期化（4ワーカー）
ChunkDownloader downloader(&cache, 4);

// HTTPヘッダー設定（認証情報など）
std::map<std::string, std::string> headers;
headers["User-Agent"] = "Mozilla/5.0 ...";
headers["Cookie"] = "session=...";
headers["Referer"] = "https://example.com/";
downloader.SetHttpHeaders(headers);

// セグメントダウンロード完了コールバック設定
downloader.SetSegmentDownloadCallback(
    [&](int64_t segmentIndex, std::vector<uint8_t>&& data, bool success) {
        if (success) {
            // セグメントキャッシュに書き込み
            segmentCache.WriteSegment(segmentIndex, std::move(data), isEncrypted);
        } else {
            LOG_WARN("Segment {} download failed", segmentIndex);
        }
    }
);

// ダウンロード開始
downloader.Start();

// HLSセグメントをリクエスト
for (const auto& seg : playlist.segments) {
    ChunkPriority priority = (seg.index < 3) ? ChunkPriority::High : ChunkPriority::Medium;
    downloader.RequestSegment(seg.url, seg.index, priority);
}

// 再生位置に応じて優先度を変更
downloader.ReprioritizeSegment(currentSegment, ChunkPriority::Critical);

// 停止
downloader.Stop();
```

### スレッドセーフティ
- すべてのAPI呼び出しはスレッドセーフ
- 内部で2つのmutexを使用:
  - `queueMutex`: チャンクリクエストキュー用
  - `segmentMutex`: セグメントリクエストキュー用

### チャンクダウンロードとの共存
- 既存のRange Requestベースのチャンクダウンロード機能はそのまま使用可能
- セグメントリクエストは別キューで管理
- ワーカースレッドは両方のキューから取り出して処理
- 優先度で比較し、より高い優先度のリクエストを先に処理

---

## HlsCustomAVIOContext

キャッシュされたHLSセグメントを連結してFFmpegに提供するカスタムAVIOContext。

### ファイル
- `cpp/src/hls/HlsCustomAVIOContext.h`
- `cpp/src/hls/HlsCustomAVIOContext.cpp`

### 概要
FFmpegがHLSセグメントを単一のストリームとして読み取れるようにする。
セグメントキャッシュ（HlsSegmentCache）から読み取り、仮想的に連結したファイルとして提供。

### インターフェース

```cpp
namespace ytdlpspout::hls {

class HlsCustomAVIOContext {
public:
    HlsCustomAVIOContext();
    ~HlsCustomAVIOContext();
    
    // 初期化
    bool Initialize(HlsSegmentCache* cache, const M3U8Playlist& playlist);
    
    // クローズ
    void Close();
    
    // FFmpeg AVIOContextを取得
    AVIOContext* GetAVIOContext() const;
    
    // サイズ・位置
    int64_t GetSize() const;
    int64_t GetPosition() const;
    int64_t GetCurrentSegmentIndex() const;
    
    // シーク
    int64_t Seek(int64_t offset, int whence);
    int64_t SeekToTime(double seconds);
    
    // 設定
    void SetReadTimeout(int timeoutMs);
};

}
```

### 仮想ファイル構造

```
[セグメント0データ][セグメント1データ][セグメント2データ]...
^                  ^                  ^
オフセット0        オフセットA        オフセットA+B
(サイズA)          (サイズB)          (サイズC)
```

### サイズ推定と更新
- **初期**: セグメントサイズが不明なため、`duration * 1MB/s` で推定
- **更新**: セグメントを読み込むたびに実サイズで更新
- オフセットテーブルが動的に更新される

### 使用例

```cpp
#include "hls/HlsCustomAVIOContext.h"
#include "hls/HlsSegmentCache.h"
#include "hls/M3U8Parser.h"

// セグメントキャッシュを準備
HlsSegmentCacheConfig cacheConfig;
cacheConfig.maxMemoryBytes = 256 * 1024 * 1024;  // 256MB
HlsSegmentCache cache(cacheConfig);

// プレイリストをパース
auto playlist = M3U8Parser::Parse(m3u8Content, baseUrl);
cache.Initialize(*playlist);

// AVIOContextを作成
HlsCustomAVIOContext avioContext;
avioContext.Initialize(&cache, *playlist);
avioContext.SetReadTimeout(5000);  // 5秒タイムアウト

// FFmpegに接続
AVFormatContext* formatCtx = avformat_alloc_context();
formatCtx->pb = avioContext.GetAVIOContext();
formatCtx->flags |= AVFMT_FLAG_CUSTOM_IO;

// フォーマット検出とストリーム情報取得
avformat_open_input(&formatCtx, nullptr, nullptr, nullptr);
avformat_find_stream_info(formatCtx, nullptr);

// 時間ベースシーク（10秒へ移動）
avioContext.SeekToTime(10.0);
```

### 注意事項
- `HlsSegmentCache`のライフタイムは`HlsCustomAVIOContext`より長く保つこと
- シーク時は現在のセグメントの先頭へ移動（セグメント内の正確なバイト位置へはシーク不可）
- 読み取りタイムアウトを設定しないと、セグメントがキャッシュされるまでブロックする
- AVIOContextのバッファサイズは32KB（内部で固定）

---

## HlsSliceLoadingManager

HLSストリームのすべてのコンポーネント（M3U8Parser, HlsSegmentCache, HlsCustomAVIOContext, ChunkDownloader）を統合管理するマネージャークラス。

### ファイル
- `cpp/src/hls/HlsSliceLoadingManager.h`
- `cpp/src/hls/HlsSliceLoadingManager.cpp`

### 概要
HLSストリームの再生に必要なすべての機能を統合し、簡単なAPIで利用可能にする。
m3u8 URLを渡すだけで、プレイリストのダウンロード・パース、暗号化キーの取得、
セグメントのプリフェッチ、FFmpegへのデータ提供までを自動で行う。

### インターフェース

```cpp
namespace ytdlpspout::hls {

/// @brief HLSスライス読み込み設定
struct HlsSliceConfig {
    size_t maxCacheMemory = 256 * 1024 * 1024;  // 256MB
    int maxConcurrentDownloads = 4;
    int prefetchSegmentsAhead = 5;
    int readTimeoutMs = 30000;
    std::map<std::string, std::string> httpHeaders;
};

class HlsSliceLoadingManager {
public:
    HlsSliceLoadingManager();
    ~HlsSliceLoadingManager();
    
    // 初期化・終了
    bool Open(const std::string& hlsUrl, const HlsSliceConfig& config);
    void Close();
    bool IsOpen() const;
    
    // FFmpeg統合
    AVIOContext* GetAVIOContext() const;
    
    // URL判定（静的）
    static bool IsHlsUrl(const std::string& url);
    
    // 再生情報
    double GetDuration() const;
    
    // 統計情報
    double GetDownloadProgress() const;
    double GetBandwidth() const;
    bool IsFullyCached() const;
    size_t GetCachedSegmentCount() const;
    size_t GetTotalSegmentCount() const;
    
    // 再生位置連携
    void UpdatePlaybackPosition(double seconds);
    void NotifySeek(double seconds);
};

}
```

### Open()の処理フロー

```
1. HttpClientで m3u8 をダウンロード
2. M3U8Parser でパース
3. セグメントがあるか確認
4. 暗号化キー（#EXT-X-KEY）がある場合、キーをダウンロード
5. HlsSegmentCache を初期化
6. ChunkDownloader を設定（HTTPヘッダー、コールバック）
7. HlsCustomAVIOContext を初期化
8. 先頭セグメントのダウンロードを開始（UpdatePlaybackPosition(0.0)）
```

### プリフェッチロジック

```cpp
void UpdatePlaybackPosition(double seconds) {
    int64_t currentSegment = m_cache->GetSegmentIndexFromTime(seconds);
    
    // 現在位置から prefetchSegmentsAhead 個先までリクエスト
    for (int64_t i = currentSegment; i < currentSegment + prefetchSegmentsAhead; ++i) {
        if (!m_cache->IsSegmentCached(i)) {
            ChunkPriority priority;
            if (i == currentSegment) priority = Critical;
            else if (i == currentSegment + 1) priority = High;
            else priority = Medium;
            
            m_downloader->RequestSegment(segment.url, i, priority);
        }
    }
    
    // キャッシュ最適化
    m_cache->OptimizeForPlayback(currentSegment, prefetchSegmentsAhead);
}
```

### シーク時の優先度変更

```cpp
void NotifySeek(double seconds) {
    int64_t targetSegment = m_cache->GetSegmentIndexFromTime(seconds);
    
    // 既存キューをクリア
    m_downloader->ClearSegmentQueue();
    
    // シーク先から優先ダウンロード
    UpdatePlaybackPosition(seconds);
    
    // AVIOにもシークを通知
    m_avioContext->SeekToTime(seconds);
}
```

### IsHlsUrl()の判定

```cpp
static bool IsHlsUrl(const std::string& url) {
    std::string lower = toLower(url);
    if (lower.find(".m3u8") != std::string::npos) return true;
    if (lower.find(".m3u") != std::string::npos) return true;
    return false;
}
```

### 使用例

```cpp
#include "hls/HlsSliceLoadingManager.h"

// マネージャー作成
HlsSliceLoadingManager manager;

// 設定
HlsSliceConfig config;
config.maxCacheMemory = 512 * 1024 * 1024;  // 512MB
config.prefetchSegmentsAhead = 10;
config.httpHeaders["Cookie"] = "session=abc123";

// HLS URLで開く
if (!manager.Open("https://example.com/stream.m3u8", config)) {
    LOG_ERROR("Failed to open HLS stream");
    return;
}

// FFmpegに接続
AVFormatContext* formatCtx = avformat_alloc_context();
formatCtx->pb = manager.GetAVIOContext();
formatCtx->flags |= AVFMT_FLAG_CUSTOM_IO;

avformat_open_input(&formatCtx, nullptr, nullptr, nullptr);
avformat_find_stream_info(formatCtx, nullptr);

// 再生ループ
while (playing) {
    // フレーム読み取り
    AVPacket pkt;
    av_read_frame(formatCtx, &pkt);
    
    // 再生位置を更新（プリフェッチ最適化）
    manager.UpdatePlaybackPosition(currentTime);
    
    // 進捗表示
    LOG_INFO("Download progress: {:.1f}%", manager.GetDownloadProgress() * 100);
    LOG_INFO("Bandwidth: {:.2f} MB/s", manager.GetBandwidth() / (1024 * 1024));
}

// クローズ
manager.Close();
```

### テスト

```bash
# ビルド
cd cpp
cmake --build build --target test_hls_slice_loading_manager --config Debug

# 実行
ctest --test-dir build -C Debug -R "TestHlsSliceLoadingManager" --output-on-failure
```

テストケース：
1. IsHlsUrl - m3u8拡張子判定
2. IsHlsUrl - m3u拡張子判定
3. IsHlsUrl - 非HLS URL判定
4. IsHlsUrl - エッジケース
5. 空URLでの初期化失敗
6. 無効URLでの初期化失敗
7. Open前のClose安全性
8. Close冪等性
9. 未オープン時の統計情報
10. Open前のUpdatePlaybackPosition安全性
11. Open前のNotifySeek安全性
12. デフォルト設定値
13. カスタム設定値
