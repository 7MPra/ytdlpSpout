# HLS (HTTP Live Streaming) パーサー実装

## 概要

HLS m3u8プレイリストをパースし、セグメント情報を抽出するためのパーサーモジュール。
メディアプレイリスト（セグメント列）に加えて、マスタープレイリスト（`#EXT-X-STREAM-INF`によるビットレート違いのバリアント選択）、fMP4の初期化セグメント（`#EXT-X-MAP`）、AES-128-CBC暗号化にも対応する。

このモジュール単体では「解析」までしか行わない。実際のダウンロード・復号・FFmpegへのデータ供給は`HlsSegmentCache`・`AesCbcDecryptor`・`HlsCustomAVIOContext`・`HlsSliceLoadingManager`（いずれも本ドキュメント後半で解説）が担う。

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
    int64_t index = 0;                    // セグメントインデックス（プレイリスト内、0から）
    std::string url;                      // セグメントURL（絶対URL）
    double duration = 0.0;                // セグメントの長さ（秒）
    int64_t byteRangeStart = -1;          // バイト範囲開始位置（-1 = 未指定）
    int64_t byteRangeLength = 0;          // バイト範囲の長さ
    int64_t mediaSequence = 0;            // メディアシーケンス番号
};
```

### HlsEncryptionKey

```cpp
struct HlsEncryptionKey {
    std::string method;        // "NONE", "AES-128", "SAMPLE-AES" など（#EXT-X-KEYのMETHOD値をそのまま保持）
    std::string keyUrl;        // キーファイルURL（絶対URLに解決済み）
    std::vector<uint8_t> iv;   // 初期化ベクトル（16バイト、IV属性がなければ空）
};
```

`method`はパース時点では値を検証・制限しない。`METHOD=SAMPLE-AES`のようにAES-128以外の値もそのまま保持される（`M3U8Parser.cpp:98-116`, `ParseKeyTag`: `M3U8Parser.cpp:345-382`）。実際に復号可能なのはAES-128のみで、それ以外は後述の通り上位層（`HlsSliceLoadingManager`）がフォールバック判断に使う。

### HlsMap（fMP4初期化セグメント）

```cpp
struct HlsMap {
    std::string url;                      // 初期化セグメントURL（絶対URL）
    int64_t byteRangeStart = -1;          // バイト範囲開始位置
    int64_t byteRangeLength = 0;          // バイト範囲の長さ
};
```

`#EXT-X-MAP:URI="...",BYTERANGE="..."`から生成される（`M3U8Parser.cpp:118-149`）。fMP4形式のHLSで、実データセグメントより先に読み込む必要があるmoov/ftyp等のヘッダー情報を指す。

### M3U8Playlist

```cpp
struct M3U8Playlist {
    int version = 0;                      // プレイリストバージョン
    double targetDuration = 0.0;          // 最大セグメント長（秒）
    int64_t mediaSequence = 0;            // 開始メディアシーケンス番号
    bool isEndList = false;               // #EXT-X-ENDLISTが存在するか
    bool isLive = false;                  // ライブストリームか（= !isEndList）
    std::vector<HlsSegment> segments;     // セグメントリスト
    std::optional<HlsEncryptionKey> encryptionKey;  // 暗号化キー情報（最後に現れたEXT-X-KEY）
    std::optional<HlsMap> map;            // 初期化セグメント情報（#EXT-X-MAP）
    double totalDuration = 0.0;           // 全セグメントの合計時間
    bool hasKeyRotation = false;          // プレイリスト内でEXT-X-KEYのURI/IVが途中で変化したか
};
```

### HlsVariant / MasterPlaylist（マスタープレイリスト用）

```cpp
/// @brief HLS品質バリアント情報（マスタープレイリスト用）
struct HlsVariant {
    std::string url;                      // サブプレイリストURL（絶対URL）
    int64_t bandwidth = 0;                // ビットレート（bps）
    int width = 0;                        // 解像度: 幅
    int height = 0;                       // 解像度: 高さ
    std::string codecs;                   // コーデック情報
    std::string name;                     // バリアント名（任意）
};

/// @brief マスタープレイリスト情報
struct MasterPlaylist {
    std::vector<HlsVariant> variants;     // 品質バリアントリスト
    bool isMaster = true;                 // マスタープレイリストフラグ
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

`M3U8Parser.cpp:19`。**メディアプレイリスト**（セグメント列を持つ末端のm3u8）をパースし、プレイリスト情報を返す。

**パラメータ:**
- `m3u8Content`: m3u8ファイルの内容（UTF-8文字列）
- `baseUrl`: 相対URL解決用のベースURL

**戻り値:**
- `std::optional<M3U8Playlist>`: 成功時はプレイリスト情報、失敗時は`nullopt`

**失敗条件:**
- 空のコンテンツ
- 1行目（トリム・大文字化後）が`#EXTM3U`と一致しない

**注意（マスタープレイリストは非対応）**: `Parse()`はマスタープレイリスト（`#EXT-X-STREAM-INF`を含むもの）を渡してもエラーにはならないが、バリアントURL行は`#EXTINF`を伴わないため無視され、`segments`は空になる。マスタープレイリストは必ず`IsMasterPlaylist()`→`ParseMaster()`→`SelectBestVariant()`でサブプレイリストURLを得てから、そのURLをダウンロードした内容を改めて`Parse()`に渡す必要がある（後述、`HlsSliceLoadingManager::Open()`が実際にこの手順を踏む）。

### ResolveUrl

```cpp
static std::string M3U8Parser::ResolveUrl(
    const std::string& baseUrl,
    const std::string& relativeUrl
);
```

`M3U8Parser.cpp:234-285`。相対URLを絶対URLに変換する。判定順序（実装通り）:

1. `relativeUrl`が空 → `baseUrl`をそのまま返す
2. `http://`または`https://`で始まる → 既に絶対URLなのでそのまま返す
3. `//`で始まる（スキーム相対URL） → `GetUrlScheme(baseUrl) + relativeUrl`（例: `//cdn.example.com/seg.ts` + `https://...`ベース → `https://cdn.example.com/seg.ts`）。`//`は`/`で始まるため、ルート相対判定より**先に**チェックする必要がある。
4. `/`で始まる（ルート相対パス） → `GetUrlOrigin(baseUrl) + relativeUrl`（例: `/absolute/seg.ts` → `https://example.com/absolute/seg.ts`）
5. それ以外（相対パス） → `GetBasePath(baseUrl) + relativeUrl`のあと、`/../`を繰り返し畳み込んで親ディレクトリ参照を解決する。畳み込みはURLのスキーム部分（`MIN_SCHEME_LENGTH = 8`、`"https://"`の長さ）より前には遡らない。

**ベースURLにクエリ文字列が含まれる場合**: `GetBasePath()`（後述）はクエリ文字列（`?`以降）を解決前に切り捨てるため、`https://example.com/video/playlist.m3u8?token=abc/def&sig=xyz`のようにクエリ内に`/`を含むURLでも、正しく`https://example.com/video/`がベースパスとなる（クエリの`/`に惑わされない）。ただし結果のURLにはベースURLのクエリ文字列は**引き継がれない**。

**対応パターン（実測）:**
- 相対パス: `segment0.ts` → `https://example.com/video/segment0.ts`
- サブディレクトリ: `segments/seg0.ts` → `https://example.com/video/segments/seg0.ts`
- 親ディレクトリ: `../segment0.ts` → 1階層上へ、`../../other/seg.ts` → 2階層上へ
- ルート相対: `/absolute/seg0.ts` → `https://example.com/absolute/seg0.ts`
- スキーム相対: `//cdn.example.com/seg0.ts` → ベースのスキームを継承して`https://cdn.example.com/seg0.ts`（httpベースなら`http://...`）
- 絶対URL（`http://`/`https://`）: 変更なしで返す

### GetBasePath / GetUrlOrigin / GetUrlScheme（内部ヘルパー）

```cpp
static std::string GetBasePath(const std::string& url);   // M3U8Parser.cpp:445 (公開API)
static std::string GetUrlOrigin(const std::string& url);  // M3U8Parser.cpp:459 (private)
static std::string GetUrlScheme(const std::string& url);  // M3U8Parser.cpp:478 (private)
```

- `GetBasePath`: `?`または`#`以降を除去した上で、最後の`/`までを返す（末尾に`/`を含む）。`HlsSliceLoadingManager`がマスター→サブプレイリスト解決時のベースURL算出にも利用する（`HlsSliceLoadingManager.cpp:245`, `:277`）。
- `GetUrlOrigin`: `scheme://host`部分のみを返す（`scheme://host/path` → `scheme://host`）。ルート相対パスの解決に使用。
- `GetUrlScheme`: `"https://..."` → `"https:"`のようにコロンを含みスラッシュを含まない形で返す。スキーム相対URLの解決に使用。スキームが見つからない場合は`"https:"`にフォールバックする。

### マスタープレイリスト対応

```cpp
static bool IsMasterPlaylist(const std::string& m3u8Content);           // M3U8Parser.cpp:491
static std::optional<MasterPlaylist> ParseMaster(
    const std::string& m3u8Content, const std::string& baseUrl);        // M3U8Parser.cpp:496
static std::optional<HlsVariant> SelectBestVariant(
    const MasterPlaylist& master, int64_t preferredBandwidth = 0);      // M3U8Parser.cpp:599
```

- `IsMasterPlaylist`: コンテンツ中に`#EXT-X-STREAM-INF`が含まれるかどうかだけで判定する単純な文字列検索。
- `ParseMaster`: `#EXT-X-STREAM-INF`行から`BANDWIDTH`・`RESOLUTION`（`WxH`）・`CODECS`・`NAME`属性を正規表現で抽出し、直後の非コメント行をサブプレイリストURLとして`HlsVariant`を組み立てる。
- `SelectBestVariant`: `preferredBandwidth <= 0`（デフォルト）の場合は**帯域幅が最も高いバリアントを選択する**。`preferredBandwidth > 0`の場合はその値以下で最大のものを選び、全バリアントが予算超過なら最小のものにフォールバックする。

現時点でこの3関数を対象にした`M3U8Parser`単体のユニットテストは存在しないが（`cpp/tests/test_m3u8_parser.cpp`にmaster/variant関連のテストケースはない）、`HlsSliceLoadingManager::Open()`が実際にこの経路を通っており（`HlsSliceLoadingManager.cpp:242-273`）、動作は統合レベルで使用されている。

## 対応タグ

`M3U8Parser::Parse()`（メディアプレイリスト）が処理するタグ:

| タグ | 説明 | 例 |
|------|------|-----|
| `#EXTM3U` | プレイリスト識別（必須、1行目） | `#EXTM3U` |
| `#EXT-X-VERSION` | バージョン | `#EXT-X-VERSION:3` |
| `#EXT-X-TARGETDURATION` | 最大セグメント長 | `#EXT-X-TARGETDURATION:10` |
| `#EXT-X-MEDIA-SEQUENCE` | 開始シーケンス番号 | `#EXT-X-MEDIA-SEQUENCE:100` |
| `#EXTINF` | セグメント情報 | `#EXTINF:9.009,` |
| `#EXT-X-KEY` | 暗号化キー（METHOD/URI/IV） | `#EXT-X-KEY:METHOD=AES-128,URI="...",IV=0x...` |
| `#EXT-X-MAP` | fMP4初期化セグメント（URI/BYTERANGE） | `#EXT-X-MAP:URI="init.mp4",BYTERANGE="800@0"` |
| `#EXT-X-BYTERANGE` | バイト範囲（`length[@offset]`） | `#EXT-X-BYTERANGE:500000@0` |
| `#EXT-X-ENDLIST` | VOD終端マーカー | `#EXT-X-ENDLIST` |

それ以外の`#`で始まる行（`#EXT-X-STREAM-INF`を含む）は未知タグ／コメントとして無視される。

`M3U8Parser::ParseMaster()`（マスタープレイリスト）が処理するタグ:

| タグ | 説明 | 例 |
|------|------|-----|
| `#EXTM3U` | プレイリスト識別（必須、1行目） | `#EXTM3U` |
| `#EXT-X-STREAM-INF` | 品質バリアント（BANDWIDTH/RESOLUTION/CODECS/NAME） | `#EXT-X-STREAM-INF:BANDWIDTH=1280000,RESOLUTION=1920x1080,CODECS="avc1.640028"` |

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

マスタープレイリストを含む一連の解決（マスター判定→バリアント選択→サブプレイリスト取得→パース）の実際のコード例は、後述の`HlsSliceLoadingManager::Open()`を参照。

## ライブ/VOD判定

- **VOD**: `#EXT-X-ENDLIST` が存在
- **ライブ**: `#EXT-X-ENDLIST` が存在しない

```cpp
playlist->isEndList  // true = VOD終端マーカーあり
playlist->isLive     // true = ライブストリーム（ENDLISTなし）
```

判定ロジックは`playlist.isLive = !playlist.isEndList;`（`M3U8Parser.cpp:222`）のみで、パーサー自体はそれ以上のライブ固有処理（プレイリスト再取得等）は行わない。

**重要**: `HlsSliceLoadingManager::Open()`は`isLive == true`のプレイリストを検出すると、その場で`Open()`を失敗させる（`HlsSliceLoadingManager.cpp:298-302`）。呼び出し元（VideoPlayer）はこの失敗を受けてFFmpegネイティブのHLSデマクサへフォールバックする設計になっており、このスライスローディング機構自体はライブストリームの再生を（自前実装としては）サポートしない。

## 暗号化対応

`M3U8Parser`自体は暗号化キーの**メタデータ抽出のみ**を行い、復号処理は含まない（実際の復号は`HlsSegmentCache`が内部で`AesCbcDecryptor`を使って行う。後述）。

```cpp
if (playlist->encryptionKey) {
    const auto& key = playlist->encryptionKey.value();
    LOG_INFO("Encryption method: {}", key.method);
    LOG_INFO("Key URL: {}", key.keyUrl);

    // IVは16バイトの配列（IV属性がなければ空）
    if (!key.iv.empty()) {
        // 明示IV。なければメディアシーケンス番号からIVを生成する（AesCbcDecryptor::GenerateIvFromSequence）
    }
}
```

**METHOD=NONE**が現れた場合、`playlist.encryptionKey`は`std::nullopt`にリセットされる（`M3U8Parser.cpp:110-113`）。

**複数の`#EXT-X-KEY`**が現れた場合は最後に出現したキーが有効になる（last-wins）。同時に、URIまたはIVが直前のキーと異なる場合は`playlist.hasKeyRotation = true`が立てられる（`M3U8Parser.cpp:98-116`）。同一のURI/IVが繰り返し出現してもローテーションとはみなされない。

**対応状況（実装で確認済み）:**
- **AES-128**: `HlsSegmentCache`が`AesCbcDecryptor`（AES-128-CBC、Windows BCrypt API）で実際に復号する。対応。
- **SAMPLE-AES**: パーサーは`method`をそのまま保持する（`METHOD=SAMPLE-AES`でも`ParseKeyTag`はエラーにしない）が、復号処理自体は実装されていない。`HlsSliceLoadingManager::Open()`は`encryptionKey->method != "AES-128"`を検出すると`Open()`を失敗させ、FFmpegネイティブHLSデマクサへフォールバックする（`HlsSliceLoadingManager.cpp:304-309`）。
- **鍵ローテーション**（`hasKeyRotation == true`）: このスライスローダーは単一鍵前提のため非対応。検出時は同様に`Open()`を失敗させてFFmpegネイティブへフォールバックする（`HlsSliceLoadingManager.cpp:311-315`）。

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

`#EXT-X-BYTERANGE:length@offset`形式でオフセットを明示できるほか、`#EXT-X-BYTERANGE:length`のようにオフセットを省略した場合は**直前のセグメントの終端（`currentByteOffset`）から連続する**ものとして扱われる（`M3U8Parser.cpp:151-168`）。例えば`500000@0`の次に`600000`（オフセット省略）が続く場合、2番目のセグメントは`byteRangeStart=500000, byteRangeLength=600000`となる。

## #EXT-X-MAP対応（fMP4初期化セグメント）

`#EXT-X-MAP:URI="...",BYTERANGE="..."`は`playlist.map`（`std::optional<HlsMap>`）として抽出される。これはfMP4のmoov/ftypなど、メディアセグメント本体より先にFFmpegへ渡す必要がある初期化データを指す。

この初期化セグメントは、キャッシュ・ダウンローダ・AVIOコンテキストの各層で**インデックス`-1`という特別な予約値**として扱われる：

- `HlsSegmentCache::WriteSegment`/`ReadSegment`/`IsSegmentCached`/`WaitForSegment`は`segmentIndex == -1`を常に有効なインデックスとして受け付ける（`HlsSegmentCache.h:13-16`のインデックス契約コメント参照）。`-1`はfMP4初期化セグメント専用の予約値で、`-2`以下または総セグメント数以上は無効。
- `HlsSliceLoadingManager::Open()`は`playlist.map`が存在する場合、最優先（`ChunkPriority::Critical`）で初期化セグメントのダウンロードをリクエストし（`HlsSliceLoadingManager.cpp:371-385`）、初期化セグメントのキャッシュ完了を`WaitForSegment(-1, timeout)`で待ってから通常の先頭セグメント（インデックス0）の待機に進む（`HlsSliceLoadingManager.cpp:540-553`）。
- `HlsCustomAVIOContext`は仮想ファイルの先頭に初期化セグメントを配置し（`hasInitSegment`）、読み取りが完了すると`currentSegmentIndex`を`-1`から`0`へ遷移させる（`HlsCustomAVIOContext.cpp:646-652`, `:674-681`）。

## 実装上の注意

### 行分割
- `\n` (Unix) と `\r\n` (Windows) の両方に対応（`SplitLines`が`\r`を除去、`M3U8Parser.cpp:291-305`）
- 空行はスキップ

### 大文字小文字
- タグは大文字小文字を区別しない（`ToUpper`で正規化してから比較）

### シーケンス番号
- `mediaSequence` は `#EXT-X-MEDIA-SEQUENCE` + セグメントインデックス
- ライブストリームでは重要（同一シーケンスを再取得しないため）。ただしこのスライスローダー自体はライブを非対応としフォールバックする（前述）。

### エラー処理
- 不正なm3u8（空コンテンツ、`#EXTM3U`欠如）は `nullopt` を返す
- 個々のタグのパースエラー（数値変換失敗等）は警告ログを出力して該当タグをスキップし、パース自体は続行する

## テスト

```bash
# ビルド
cd cpp/build
cmake --build . --target test_m3u8_parser --config Debug

# 実行
./bin/Debug/test_m3u8_parser.exe
```

`cpp/tests/test_m3u8_parser.cpp`に24件のテストケースがある。主なカテゴリ：

- 基本パース: シンプルなVOD、AES-128暗号化付き、バイト範囲指定、ライブストリーム（ENDLISTなし）、CRLF改行、不正なm3u8（`#EXTM3U`なし）、空コンテンツ
- URL解決: 相対パス、親ディレクトリ（`../`）、ルート相対パス、絶対URL、末尾スラッシュ付きベース、クエリ文字列中に`/`を含むベースURL、スキーム相対URL（`//host/path`）、プレイリスト内での相対URL解決
- タグの大文字小文字混在、EXTINFのタイトル文字列付与
- `#EXT-X-KEY`: METHOD=NONE、複数キー（後勝ち）、METHOD=SAMPLE-AESの保持、鍵ローテーションの検出／非検出（同一キー繰り返しでは非検出）
- ライブプレイリストで`isLive`が立つことのパーサーレベル確認
- `#EXT-X-BYTERANGE`のオフセット省略（前セグメント終端からの継続）

## 今後の拡張

- [ ] 字幕トラック（`#EXT-X-MEDIA`）対応 — 未実装（パーサーはこのタグを認識しない）
- [ ] SAMPLE-AESの自前復号 — 未実装。現状は検出後にFFmpegネイティブHLSデマクサへフォールバックする
- [ ] 鍵ローテーションへの自前対応 — 未実装。現状は検出後にFFmpegネイティブHLSデマクサへフォールバックする
- [ ] ライブストリームの自前対応（プレイリスト再取得） — 未実装。現状は検出後にFFmpegネイティブHLSデマクサへフォールバックする
- [ ] マスタープレイリスト（`#EXT-X-STREAM-INF`）対応 — **実装済み**（`IsMasterPlaylist`/`ParseMaster`/`SelectBestVariant`。前述）

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

（`AesCbcDecryptor::Decrypt`: `AesCbcDecryptor.cpp:167`、`GenerateIvFromSequence`: `AesCbcDecryptor.cpp:255`、`ParseHexIv`: `AesCbcDecryptor.cpp:274`）

`Decrypt()`は暗号文が16バイト境界に整列していること、IVがちょうど16バイトであることを要求し、いずれか違反時は空ベクターを返す。復号後は`RemovePkcs7Padding`でPKCS7パディングを検証・除去する（パディング長が不正な場合も空ベクターを返す）。

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

実際の適用優先順位（`HlsSegmentCache::Impl::GetIvForSegment`, `HlsSegmentCache.cpp:156-175`）:
1. `SetEncryptionKey()`に明示IVを渡していればそれを使用
2. プレイリストの`#EXT-X-KEY`にIV属性があればそれを使用
3. どちらもなければ、セグメントの`mediaSequence`から`GenerateIvFromSequence()`で生成

なお、`M3U8Parser`が`#EXT-X-KEY`のIV属性を16進文字列からバイト配列へ変換する処理（`ParseHexString`, `M3U8Parser.cpp:415-443`）と、`AesCbcDecryptor::ParseHexIv`は別実装であり挙動が異なる点に注意（前者は32文字に満たない場合ゼロパディングして続行、後者は長さ不一致で`nullopt`を返す）。

### テスト

```bash
# ビルド
cd cpp
cmake --build build --target test_aes_decryptor --config Debug

# 実行
.\build\bin\Debug\test_aes_decryptor.exe
```

`cpp/tests/test_aes_decryptor.cpp`に16件のテストケースがある。主なカテゴリ：

1. 初期化とクローズ
2. 不正なキー長でのエラー
3. AES-128-CBCでの基本的な復号
4. PKCS7パディング除去
5. 不正なIV長でのエラー
6. 空の暗号文
7. 非ブロックアライメント暗号文
8. シーケンス番号からのIV生成（基本値・大きい値）
9. 16進文字列のIV解析（0xプレフィックスあり/なし/大文字/不正長/不正文字/空文字列）
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

### セグメントインデックスの契約

（`HlsSegmentCache.h:13-16`のコメントより）

- `0`以上: プレイリスト上の通常セグメント（`GetTotalSegmentCount()`未満であること）
- `-1`: fMP4初期化セグメント（`#EXT-X-MAP`）を表す特別な予約値。常に許可される。
- `-2`以下、または総セグメント数以上: 無効なインデックス。書き込み/読み取りは失敗する。

### API

```cpp
class HlsSegmentCache {
public:
    explicit HlsSegmentCache(const HlsSegmentCacheConfig& config = HlsSegmentCacheConfig{});

    // 初期化
    void Initialize(const M3U8Playlist& playlist);
    void SetEncryptionKey(const std::vector<uint8_t>& keyData,
                          const std::optional<std::vector<uint8_t>>& explicitIv = std::nullopt);

    // 読み書き
    bool WriteSegment(int64_t segmentIndex, std::vector<uint8_t>&& data, bool isEncrypted);
    std::optional<std::vector<uint8_t>> ReadSegment(int64_t segmentIndex, int timeoutMs = 0);

    // 状態確認
    bool IsSegmentCached(int64_t segmentIndex) const;

    // 条件変数によるセグメント到着待ち（ポーリングではない。後述）
    bool WaitForSegment(int64_t segmentIndex, int timeoutMs = 0);

    // 恒久ダウンロード失敗の伝搬
    void MarkSegmentFailed(int64_t segmentIndex);
    bool IsSegmentFailed(int64_t segmentIndex) const;
    void ClearSegmentFailed(int64_t segmentIndex);

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

### 待機は条件変数ベース（ポーリングではない）

`WaitForSegment()`（`HlsSegmentCache.cpp:453-519`）は`std::condition_variable::wait`/`wait_for`に「キャッシュ済み、または恒久失敗マーク済み」を述語として渡す実装であり、一定間隔でのポーリング（例: 100msスリープループ）ではない。`WriteSegment()`が成功する度に`segmentAvailableCv.notify_all()`（`HlsSegmentCache.cpp:381`）、`MarkSegmentFailed()`が呼ばれた際にも同様に`notify_all()`（`HlsSegmentCache.cpp:537`）が行われ、待機中のスレッドを即座に起床させる。`ReadSegment()`のタイムアウト待機（`timeoutMs != 0`の場合）も同じ条件変数を使う。

### 恒久ダウンロード失敗の伝搬

- `MarkSegmentFailed(index)`: ダウンロードのリトライが尽きて最終的に失敗した場合に呼ぶ。`failedSegments`に追加し、待機中の`WaitForSegment`/`ReadSegment`を即座に起床させる。
- `IsSegmentFailed(index)`: 失敗マーク済みかを確認する。
- `ClearSegmentFailed(index)`: 再ダウンロード試行時にマークを解除する。`WriteSegment()`が成功した場合も自動的にマークが解除される（`HlsSegmentCache.cpp:374`）。

この失敗マーク機構と、実際に「1回だけ自動リトライ→それでも失敗ならEIO」を行うのは`HlsCustomAVIOContext::ReadPacket`側である（後述）。

### LRUエビクション

- `maxMemoryBytes`を超えた場合、最も古いアクセスのセグメントを削除
- `ReadSegment()`呼び出しでLRU順序を更新
- `OptimizeForPlayback()`で再生位置周辺のセグメントを保護（削除対象から除外）

### スレッドセーフティ

- `std::mutex`で全操作を保護
- 複数スレッドからの同時読み書きに対応
- `std::condition_variable`でセグメント到着（または恒久失敗マーク）を待機可能

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

// ダウンロードが恒久的に失敗した場合
cache.MarkSegmentFailed(0);
// -> WaitForSegment(0, ...) は待機を打ち切って false を返す
```

### テスト

```bash
# ビルド
cd cpp
cmake --build build --target test_hls_segment_cache --config Debug

# 実行
.\build\bin\Debug\test_hls_segment_cache.exe
```

`cpp/tests/test_hls_segment_cache.cpp`に19件のテストケースがある。主なカテゴリ：

1. 基本的な初期化
2. 基本的な書き込み・読み取り
3. LRUエビクション動作確認
4. AES復号との統合
5. 時間→セグメントインデックス変換
6. スレッドセーフティ（並行アクセス）
7. メモリ制限超過時の動作
8. セグメント情報取得（バイト範囲を含む）
9. OptimizeForPlayback
10. タイムアウト付き読み取り（ReadSegment）
11. 無効なインデックスのハンドリング
12. セグメント失敗マーキング（MarkSegmentFailed）
13. 失敗マークが待機中のスレッドを起床させること
14. 書き込み成功時に失敗マークが解除されること
15. 無効インデックスに対する失敗マーク操作
16. WaitForSegmentが既にキャッシュ済みの場合に即座に返ること
17. WaitForSegmentのタイムアウト
18. WaitForSegmentが書き込みで起床すること
19. WaitForSegmentの無期限待機（タイムアウトなし）

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
/// @param byteRangeStart バイト範囲開始位置（-1の場合は範囲指定なし）
/// @param byteRangeLength バイト範囲の長さ
void RequestSegment(const std::string& url, int64_t segmentIndex, ChunkPriority priority,
                   int64_t byteRangeStart = -1, int64_t byteRangeLength = 0);

/// @brief キューに入っているセグメントの優先度を変更
void ReprioritizeSegment(int64_t segmentIndex, ChunkPriority newPriority);

/// @brief セグメントダウンロードキューをクリア
void ClearSegmentQueue();

/// @brief HTTPヘッダーを設定（全リクエストに適用）
void SetHttpHeaders(const std::map<std::string, std::string>& headers);
```

（`cpp/src/io/ChunkDownloader.h:131-140`）。`RequestSegment`は`byteRangeStart`/`byteRangeLength`を受け取る5引数版で、`#EXT-X-MAP`や`#EXT-X-BYTERANGE`で範囲指定されたセグメントもこの1つのAPIでダウンロードできる（範囲指定不要なら`byteRangeStart`はデフォルトの`-1`のまま）。

### 動作仕様
1. `RequestSegment()`でセグメントキューに追加
2. ワーカースレッドがURLをダウンロード（`byteRangeStart >= 0`の場合はRange Requestで部分取得、それ以外はセグメント全体を取得）
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

// HLSセグメントをリクエスト（バイト範囲があれば渡す）
for (const auto& seg : playlist.segments) {
    ChunkPriority priority = (seg.index < 3) ? ChunkPriority::High : ChunkPriority::Medium;
    downloader.RequestSegment(seg.url, seg.index, priority, seg.byteRangeStart, seg.byteRangeLength);
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

    // シーク時に新しいセグメントインデックスを通知するコールバック
    void SetOnSeekCallback(std::function<void(int64_t)> callback);

    // 恒久失敗セグメントの一度限りの自動再ダウンロードを要求するコールバック
    void SetOnSegmentRetryCallback(std::function<void(int64_t)> callback);
};

}
```

### 仮想ファイル構造

```
[初期化セグメント（あれば）][セグメント0データ][セグメント1データ]...
^                          ^                  ^
オフセット0                オフセットA        オフセットA+B
(サイズ: hasInitSegmentなら initSegmentSize)  (サイズB)          (サイズC)
```

### サイズ推定と更新
- **初期**: セグメントサイズが不明なため、`duration * 1MB/s`（`ESTIMATED_BYTES_PER_SECOND`, `HlsCustomAVIOContext.cpp:33`）で推定。初期化セグメントは`BYTERANGE`長が既知ならそれを使用し、なければ4KB（`ESTIMATED_INIT_SEGMENT_SIZE`）で推定
- **更新**: セグメントを読み込むたびに実サイズで更新し、後続オフセットをシフト
- オフセットテーブルが動的に更新される

### 恒久ダウンロード失敗時のリトライ→EIO

`ReadPacket()`（`HlsCustomAVIOContext.cpp:531-688`）は、対象セグメントがキャッシュされておらず`cache->IsSegmentFailed()`が真の場合（恒久失敗マーク済み）に限り、以下の動作をする（`HlsCustomAVIOContext.cpp:593-622`）：

1. そのセグメントについてまだ自動リトライしていなければ（`retriedSegments`に未登録）、`retriedSegments`に登録した上で`onSegmentRetryCallback`を呼び出し（コールバック側で失敗マーク解除＋再ダウンロード要求を行う）、`AVERROR(EAGAIN)`を返す（FFmpegに再試行させる）。
2. 既に一度リトライ済みで再度失敗している場合は、`AVERROR(EIO)`を返して確定的に失敗させる（無限EAGAINループを防ぐ）。

単なるダウンロード中（タイムアウトだが失敗マークなし）の場合は、従来通り`AVERROR(EAGAIN)`を返すのみで、この1回リトライロジックは発動しない。`retriedSegments`は`SeekToTime()`が呼ばれるたびにクリアされ、ユーザー操作によるシーク後は再度リトライの機会が与えられる（`HlsCustomAVIOContext.cpp:512-514`）。

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
- AVIOContextのバッファサイズは32KB（`AVIO_BUFFER_SIZE`、内部で固定）

---

## HlsSliceLoadingManager

HLSストリームのすべてのコンポーネント（M3U8Parser, HlsSegmentCache, HlsCustomAVIOContext, ChunkDownloader）を統合管理するマネージャークラス。

### ファイル
- `cpp/src/hls/HlsSliceLoadingManager.h`
- `cpp/src/hls/HlsSliceLoadingManager.cpp`

### 概要
HLSストリームの再生に必要なすべての機能を統合し、簡単なAPIで利用可能にする。
m3u8 URLを渡すだけで、マスタープレイリスト判定・バリアント選択、プレイリストのダウンロード・パース、暗号化キーの取得、セグメントのプリフェッチ、FFmpegへのデータ提供までを自動で行う。ただしライブ・SAMPLE-AES・鍵ローテーションを検出した場合は`Open()`自体を失敗させ、呼び出し元にFFmpegネイティブHLSデマクサへのフォールバックを促す。

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

（`HlsSliceLoadingManager.cpp:200-580`。失敗時は`false`を返し、呼び出し元がFFmpegネイティブHLSデマクサへフォールバックする想定）

```
 1. 空URL / 非HLS URL（IsHlsUrl == false）なら即失敗
 2. HttpClientで m3u8 をダウンロード
 3. M3U8Parser::IsMasterPlaylist() でマスタープレイリスト判定
    - マスターの場合: ParseMaster() でバリアント一覧を取得し、
      SelectBestVariant()（デフォルトで最高帯域幅）でサブプレイリストURLを選択し、
      そのサブプレイリストを再ダウンロードする
 4. M3U8Parser::Parse() でメディアプレイリストをパース（セグメントが空なら失敗）
 5. フォールバック判定（いずれか該当すればここでOpen()を失敗させる）:
    - playlist.isLive == true（#EXT-X-ENDLISTなし）
    - encryptionKey.method != "AES-128"（SAMPLE-AES等）
    - playlist.hasKeyRotation == true（鍵ローテーション検出）
 6. 暗号化キーがある場合（AES-128確定後）、キーをダウンロード
 7. HlsSegmentCache を初期化し、暗号化キー（＋明示IVがあれば）を設定
 8. ChunkDownloader を設定（HTTPヘッダー、セグメントダウンロードコールバック）して Start()
 9. playlist.map（#EXT-X-MAP）がある場合、初期化セグメントを最優先(Critical)でリクエスト
10. HlsCustomAVIOContext を初期化し、SetOnSeekCallback / SetOnSegmentRetryCallback を設定
11. 先頭セグメント（インデックス0）を Critical 優先度でリクエスト
12. 初期化セグメントがあれば cache->WaitForSegment(-1, timeout) で到着を待機
13. cache->WaitForSegment(0, timeout) で先頭セグメントの到着を待機
    （いずれも条件変数ベースの待機であり、一定間隔のポーリングではない。前述の
    HlsSegmentCache::WaitForSegment を参照）
14. ロック解放後、UpdatePlaybackPosition(0.0) を呼んでプリフェッチを開始
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

            m_downloader->RequestSegment(segment.url, i, priority,
                                          segment.byteRangeStart, segment.byteRangeLength);
        }
    }

    // キャッシュ最適化
    m_cache->OptimizeForPlayback(currentSegment, prefetchSegmentsAhead);
}
```

（`HlsSliceLoadingManager.cpp:744-801`。実装ではデッドロック回避のため、対象セグメント収集をロック内で行い、実際の`RequestSegment`/`OptimizeForPlayback`呼び出しはロック外で行う2段階構成になっている。）

### シーク時の優先度変更

`NotifySeek()`（`HlsSliceLoadingManager.cpp:803-873`）は`UpdatePlaybackPosition()`を呼び出すのではなく、同等のロジック（対象セグメント収集→キュークリア→リクエスト発行→キャッシュ最適化→AVIOへの時間シーク通知）を自身の中に展開している：

```cpp
void NotifySeek(double seconds) {
    // 1. シーク先セグメントと prefetchSegmentsAhead 個先までの
    //    未キャッシュセグメントをロック内で収集
    // 2. downloader->ClearSegmentQueue() で既存キューをクリア（ロック外）
    // 3. 収集したセグメントを RequestSegment（ロック外）
    // 4. cache->OptimizeForPlayback(targetSegment, prefetchAhead)（ロック外）
    // 5. avioContext->SeekToTime(seconds)（ロック外。Managerロックを
    //    保持したまま呼ぶと OnSeekCallback 経由で再入しデッドロックしうるため）
}
```

### IsHlsUrl()の判定

（`HlsSliceLoadingManager.cpp:641-673`。拡張子だけでなく、yt-dlp等が付与するクエリパラメータ形式のURLも判定する）

```cpp
static bool IsHlsUrl(const std::string& url) {
    if (url.empty()) return false;
    std::string lower = ToLower(url);

    if (lower.find(".m3u8") != std::string::npos) return true;
    if (lower.find(".m3u") != std::string::npos) return true;

    // "format=m3u8" が値の終端（&・#・文字列末尾）で終わる場合のみ真
    // （"format=m3u8xxx" のような誤検知を避ける）
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

// HLS URLで開く（ライブ/SAMPLE-AES/鍵ローテーションの場合はfalseが返るので
// 呼び出し側でFFmpegネイティブHLSデマクサへフォールバックすること）
if (!manager.Open("https://example.com/stream.m3u8", config)) {
    LOG_ERROR("Failed to open HLS stream (falling back to native FFmpeg HLS demuxer)");
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

`cpp/tests/test_hls_slice_loading_manager.cpp`（GoogleTest）に15件のテストケースがある：

1. IsHlsUrl - m3u8拡張子判定
2. IsHlsUrl - m3u拡張子判定
3. IsHlsUrl - 非HLS URL判定
4. IsHlsUrl - エッジケース
5. IsHlsUrl - `format=m3u8`クエリパラメータ判定
6. IsHlsUrl - MIMEタイプクエリパラメータ判定
7. 空URLでの初期化失敗
8. 無効URLでの初期化失敗
9. Open前のClose安全性
10. Close冪等性
11. 未オープン時の統計情報
12. Open前のUpdatePlaybackPosition安全性
13. Open前のNotifySeek安全性
14. デフォルト設定値
15. カスタム設定値
