# D3D11 Graphics Optimization

このドキュメントはD3D11グラフィックスモジュール（`cpp/src/graphics/`）と、それに関連する
デコーダー/プレイヤー側の最適化（`cpp/src/decoder/FrameConverter`, `cpp/src/player/VideoPlayer`）
の設計を、実装ソースコードを正として説明します。記載内容は対応するソースファイルで
確認できた事実に基づきます。

## ステージングテクスチャプール

### 背景

`ReadbackTexture()` は毎フレームGPUテクスチャからCPUメモリにピクセルデータを読み取る処理です。
以前の実装では毎回ステージングテクスチャを作成・破棄していたため、不要なメモリアロケーションが発生していました。

### 設計

```
┌─────────────────────────────────────────────────────────┐
│                  D3D11Context                           │
├─────────────────────────────────────────────────────────┤
│  m_stagingPool: vector<StagingTextureEntry>             │
│  m_stagingPoolMutex: std::mutex                         │
│  kMaxStagingPoolSize: 4                                 │
├─────────────────────────────────────────────────────────┤
│  + AcquireStagingTexture(w, h, format)                  │
│  + ReleaseStagingTexture(texture)                       │
│  + ClearStagingPool()                                   │
└─────────────────────────────────────────────────────────┘
```

### StagingTextureEntry 構造体

```cpp
struct StagingTextureEntry {
    ComPtr<ID3D11Texture2D> texture;  // テクスチャ本体
    UINT width;                        // 幅
    UINT height;                       // 高さ
    DXGI_FORMAT format;                // フォーマット
    bool inUse;                        // 使用中フラグ
};
```

（`D3D11Context.h:266-272`）

### アルゴリズム

#### AcquireStagingTexture()

1. プール内から未使用かつサイズ・フォーマットが一致するテクスチャを検索
2. 見つかれば `inUse = true` に設定して返却
3. 見つからなければ新規作成し、プールサイズ上限内ならプールに追加

```cpp
ComPtr<ID3D11Texture2D> D3D11Context::AcquireStagingTexture(UINT width, UINT height, DXGI_FORMAT format) {
    std::lock_guard<std::mutex> lock(m_stagingPoolMutex);

    // プール内から再利用可能なテクスチャを検索
    for (auto& entry : m_stagingPool) {
        if (!entry.inUse &&
            entry.width == width &&
            entry.height == height &&
            entry.format == format) {
            entry.inUse = true;
            return entry.texture;
        }
    }

    // なければ新規作成してプールに追加
    auto newTexture = CreateStagingTexture(width, height, format);
    if (newTexture && m_stagingPool.size() < kMaxStagingPoolSize) {
        m_stagingPool.push_back({newTexture, width, height, format, true});
    }
    return newTexture;
}
```

（`D3D11Context.cpp:910-930`）

#### ReleaseStagingTexture()

1. プール内から該当テクスチャを検索
2. 見つかれば `inUse = false` に設定

```cpp
void D3D11Context::ReleaseStagingTexture(ID3D11Texture2D* texture) {
    if (!texture) return;
    std::lock_guard<std::mutex> lock(m_stagingPoolMutex);

    for (auto& entry : m_stagingPool) {
        if (entry.texture.Get() == texture) {
            entry.inUse = false;
            return;
        }
    }
}
```

（`D3D11Context.cpp:932-942`）

### パフォーマンス効果

| 項目 | 旧実装 | 新実装 |
|------|--------|--------|
| 毎フレームのアロケーション | 1回 | 0回（再利用時） |
| GPU→CPUコピー | 変化なし | 変化なし |
| メモリプール上限 | なし | 4テクスチャ（`kMaxStagingPoolSize`, `D3D11Context.h:276`） |

### スレッドセーフティ

- `std::mutex`（`m_stagingPoolMutex`）によりプール操作は排他制御
- 複数スレッドからの同時アクセスに対応

### テスト項目

| テスト名 | 内容 |
|----------|------|
| `Test_StagingPool_AcquireReturnsValidTexture` | 取得したテクスチャが有効かつ正しい属性 |
| `Test_StagingPool_ReusesSameTexture` | 解放後に同じテクスチャが再利用される |
| `Test_StagingPool_DifferentSizeCreatesNew` | 異なるサイズでは新規作成 |
| `Test_StagingPool_ReleaseMakesAvailable` | 解放操作で再利用可能になる |
| `Test_ReadbackTexture_UsesPool` | ReadbackTexture()がプールを使用 |
| `Test_ClearStagingPool_WorksAfterClear` | クリア後も正常動作 |

（`cpp/tests/test_d3d11_staging_pool.cpp:255-260`）

### ファイル

- ヘッダー: `cpp/src/graphics/D3D11Context.h`
- 実装: `cpp/src/graphics/D3D11Context.cpp`（`AcquireStagingTexture`: 910-930, `ReleaseStagingTexture`: 932-942, `ClearStagingPool`: 944-947）
- テスト: `cpp/tests/test_d3d11_staging_pool.cpp`

### 使用例

```cpp
D3D11Context ctx;
ctx.Initialize(true);

// ReadbackTexture()は内部でプールを使用
std::vector<uint8_t> buffer(width * height * 4);
ctx.ReadbackTexture(texture, buffer.data(), buffer.size());

// シャットダウン時に自動でプールがクリアされる
ctx.Shutdown();
```

### 注意事項

- `AcquireStagingTexture()` で取得したテクスチャは必ず `ReleaseStagingTexture()` で返却すること
- `ReadbackTexture()` は内部で取得・返却を行うため、明示的な操作は不要
- `Shutdown()` 時に自動で `ClearStagingPool()` が呼ばれる

## 非同期リードバック（GPU→CPU転送）

### 背景

同期的な `ReadbackTexture()` は毎フレームGPUの完了を待機するため、CPUとGPUのパイプラインが直列化されていました。
非同期リードバックにより、GPUコピーの完了を待たずに次フレームの処理を開始できます。

### 設計

```
┌─────────────────────────────────────────────────────────┐
│                AsyncReadbackHandle                       │
├─────────────────────────────────────────────────────────┤
│  stagingTexture: ComPtr<ID3D11Texture2D>                │
│  query: ComPtr<ID3D11Query>  // D3D11_QUERY_EVENT       │
│  width, height: UINT                                     │
│  valid: bool                                             │
└─────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────┐
│                  D3D11Context                           │
├─────────────────────────────────────────────────────────┤
│  + BeginAsyncReadback(texture) -> AsyncReadbackHandle   │
│  + IsReadbackComplete(handle) -> bool                   │
│  + CompleteAsyncReadback(handle, outData, size) -> bool │
└─────────────────────────────────────────────────────────┘
```

（`D3D11Context.h:34-41, 178-190`）

### API

#### BeginAsyncReadback()

GPUテクスチャからステージングテクスチャへの非同期コピーを開始し、完了検知用のクエリを発行します。

```cpp
D3D11Context::AsyncReadbackHandle D3D11Context::BeginAsyncReadback(ID3D11Texture2D* texture) {
    AsyncReadbackHandle handle;
    if (!texture || !m_context) return handle;

    D3D11_TEXTURE2D_DESC srcDesc;
    texture->GetDesc(&srcDesc);

    // ステージングテクスチャ取得（プールから）
    handle.stagingTexture = AcquireStagingTexture(srcDesc.Width, srcDesc.Height, srcDesc.Format);
    if (!handle.stagingTexture) return handle;

    // 非同期コピー開始
    m_context->CopyResource(handle.stagingTexture.Get(), texture);

    // GPU完了クエリを発行（CreateQuery失敗時はqueryがnullのままhandle.validはtrueになる）
    D3D11_QUERY_DESC queryDesc = {};
    queryDesc.Query = D3D11_QUERY_EVENT;
    if (SUCCEEDED(m_device->CreateQuery(&queryDesc, &handle.query))) {
        m_context->End(handle.query.Get());
    }

    handle.width = srcDesc.Width;
    handle.height = srcDesc.Height;
    handle.valid = true;
    return handle;
}
```

（`D3D11Context.cpp:839-864`）

#### IsReadbackComplete()

GPUコピーが完了したかをノンブロッキングでチェックします。

```cpp
bool D3D11Context::IsReadbackComplete(const AsyncReadbackHandle& handle) {
    if (!handle.valid || !handle.query || !m_context) return false;

    BOOL queryData = FALSE;
    HRESULT hr = m_context->GetData(
        handle.query.Get(), &queryData, sizeof(BOOL),
        D3D11_ASYNC_GETDATA_DONOTFLUSH);
    return (hr == S_OK && queryData == TRUE);
}
```

（`D3D11Context.cpp:866-872`）

#### CompleteAsyncReadback()

完了したリードバックの結果を取得し、ステージングテクスチャをプールに返却します。

```cpp
bool D3D11Context::CompleteAsyncReadback(AsyncReadbackHandle& handle, void* outData, size_t bufferSize) {
    if (!handle.valid || !handle.stagingTexture || !m_context) return false;

    // バッファサイズチェック
    size_t requiredSize = static_cast<size_t>(handle.width) * handle.height * 4;
    if (bufferSize < requiredSize) {
        return false;
    }

    // Map/Unmapでデータ取得
    D3D11_MAPPED_SUBRESOURCE mapped;
    HRESULT hr = m_context->Map(handle.stagingTexture.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) return false;

    // ピクセルデータをコピー（RowPitch考慮、BGRA想定）
    uint8_t* dstRow = static_cast<uint8_t*>(outData);
    const uint8_t* srcRow = static_cast<const uint8_t*>(mapped.pData);
    UINT copyPitch = handle.width * 4;
    for (UINT y = 0; y < handle.height; ++y) {
        memcpy(dstRow, srcRow, copyPitch);
        dstRow += copyPitch;
        srcRow += mapped.RowPitch;
    }
    m_context->Unmap(handle.stagingTexture.Get(), 0);

    // プールに返却
    ReleaseStagingTexture(handle.stagingTexture.Get());
    handle.valid = false;
    return true;
}
```

（`D3D11Context.cpp:874-904`）

### VideoPlayerでのダブルバッファリング

VideoPlayer::Impl構造体に以下を保持:

```cpp
D3D11Context::AsyncReadbackHandle pendingReadback;
bool hasPendingReadback = false;
```

（`VideoPlayer.cpp:107-108`）

ProcessFrame()での使用パターン（実際には後述の「省電力最適化（P-10）」による
ゲートの内側で実行される）:

```cpp
// P-10: 直近一定時間以内にGUI側からフレーム取得要求があった場合のみ実行
bool readbackActive = (NowNanos() - lastFrameRequestNanos.load()) < kReadbackActiveWindowNanos;

if (readbackActive) {
    // 前フレームの非同期リードバックが完了していれば結果を取得
    if (hasPendingReadback) {
        if (d3dContext->IsReadbackComplete(pendingReadback)) {
            if (d3dContext->CompleteAsyncReadback(pendingReadback, buffer.data(), bufferSize)) {
                hasValidFrame = true;
            }
            hasPendingReadback = false;
        }
        // 完了していなければ次のフレームへ持ち越し（ダブルバッファリング）
    }

    // 新しい非同期リードバックを開始（ペンディングがなければ）
    if (!hasPendingReadback) {
        pendingReadback = d3dContext->BeginAsyncReadback(currentTexture);
        hasPendingReadback = pendingReadback.valid;
    }
}
// readbackActive が false の場合、このフレームはリードバック処理自体を行わない
```

（`VideoPlayer.cpp:628-665`）

### パフォーマンス効果

| 項目 | 同期リードバック | 非同期リードバック |
|------|-----------------|-------------------|
| CPUブロック | 毎フレーム | なし（完了時のみ） |
| レイテンシ | 1フレーム | 1-2フレーム |
| スループット | GPU依存 | パイプライン化で向上 |

### テスト項目

| テスト名 | 内容 |
|----------|------|
| `Test_AsyncReadback_BeginReturnsValidHandle` | ハンドルが有効で正しい属性 |
| `Test_AsyncReadback_IsCompleteReturnsFalseInitially` | 開始直後はfalse、Flush後はtrue |
| `Test_AsyncReadback_CompleteRetrievesCorrectData` | 正しいピクセルデータが取得できる |
| `Test_AsyncReadback_DoubleBufferNoStall` | ダブルバッファリングでストールなし |

（`cpp/tests/test_d3d11_async_readback.cpp:311-314`）

### ファイル

- ヘッダー: `cpp/src/graphics/D3D11Context.h`
- 実装: `cpp/src/graphics/D3D11Context.cpp`
- VideoPlayer統合: `cpp/src/player/VideoPlayer.cpp`
- テスト: `cpp/tests/test_d3d11_async_readback.cpp`

### 注意事項

- `BeginAsyncReadback()` で取得したハンドルは必ず `CompleteAsyncReadback()` で完了させるか、停止時にステージングテクスチャを返却すること
- `VideoPlayer::Stop()` ではペンディングのリードバックをクリーンアップする（`GPU処理完了待機（Flush） → ステージングテクスチャの返却` の順、`VideoPlayer.cpp:362-373`）
- 1フレームの遅延が発生するため、フレーム正確性が必要な場合は同期版（`ReadbackTexture()`）を使用
- GUI側が直近一定時間フレームを要求していない場合、このリードバック自体が実行されない（Spout送信には影響しない）。詳細は「省電力最適化: フレーム取得要求ウィンドウ（P-10）」節を参照

## NV12 SRVテクスチャプール（D3D11VAデコーダー対応）

### 背景

D3D11VAハードウェアデコーダーが返すNV12テクスチャは、`D3D11_BIND_SHADER_RESOURCE`フラグを持たないため、
直接シェーダーリソースビュー（SRV）を作成できません。
このため、GPUシェーダーを使用したNV12→RGBA変換ができず、ハードウェアアクセラレーションの恩恵を受けられませんでした。

### 解決策

SRV作成可能な中間NV12テクスチャを用意し、`CopySubresourceRegion()`でGPU内コピーしてからシェーダー変換を行います。

```
┌──────────────────────┐     CopySubresourceRegion     ┌──────────────────────┐
│  D3D11VA NV12        │  ─────────────────────────►   │  中間NV12テクスチャ   │
│  (SRV不可)           │         GPU内コピー            │  (SRV可能)           │
│  BindFlags: 0        │                                │  BindFlags: SRV      │
└──────────────────────┘                               └──────────────────────┘
                                                               │
                                                               ▼ SRV作成
                                                        ┌──────────────────────┐
                                                        │  ピクセルシェーダー   │
                                                        │  NV12→RGBA変換       │
                                                        └──────────────────────┘
```

### 設計

```
┌─────────────────────────────────────────────────────────┐
│                  D3D11Context                           │
├─────────────────────────────────────────────────────────┤
│  m_nv12Pool: vector<NV12TextureEntry>                   │
│  m_nv12PoolMutex: std::mutex                            │
│  kMaxNV12PoolSize: 4                                    │
├─────────────────────────────────────────────────────────┤
│  + AcquireNV12SRVTexture(w, h) -> ID3D11Texture2D*      │
│  + ReleaseNV12SRVTexture(texture)                       │
│  + ClearNV12Pool()                                      │
└─────────────────────────────────────────────────────────┘
```

### NV12TextureEntry 構造体

```cpp
struct NV12TextureEntry {
    ComPtr<ID3D11Texture2D> texture;  // テクスチャ本体
    UINT width;                        // 幅
    UINT height;                       // 高さ
    bool inUse;                        // 使用中フラグ
};
```

（`D3D11Context.h:279-284`）

### アルゴリズム

#### AcquireNV12SRVTexture()

1. プール内から未使用かつサイズが一致するテクスチャを検索
2. 見つかれば `inUse = true` に設定して返却
3. 見つからなければ`D3D11_BIND_SHADER_RESOURCE`フラグ付きで新規作成

```cpp
ComPtr<ID3D11Texture2D> D3D11Context::AcquireNV12SRVTexture(UINT width, UINT height) {
    std::lock_guard<std::mutex> lock(m_nv12PoolMutex);

    // プール内から再利用可能なテクスチャを検索
    for (auto& entry : m_nv12Pool) {
        if (!entry.inUse && entry.width == width && entry.height == height) {
            entry.inUse = true;
            return entry.texture;
        }
    }

    // なければ新規作成（D3D11_BIND_SHADER_RESOURCE付き）
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_NV12;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;  // 重要: SRV作成可能

    ComPtr<ID3D11Texture2D> newTexture;
    HRESULT hr = m_device->CreateTexture2D(&desc, nullptr, &newTexture);
    if (FAILED(hr)) return nullptr;

    if (m_nv12Pool.size() < kMaxNV12PoolSize) {
        m_nv12Pool.push_back({newTexture, width, height, true});
    }
    return newTexture;
}
```

（`D3D11Context.cpp:953-986`）

### ConvertNV12ToRGBA()の修正

```cpp
bool D3D11Context::ConvertNV12ToRGBA(ID3D11Texture2D* srcNV12, UINT srcIndex, ID3D11Texture2D* dstRGBA) {
    D3D11_TEXTURE2D_DESC srcDesc;
    srcNV12->GetDesc(&srcDesc);

    ComPtr<ID3D11Texture2D> srvSourceTexture;
    ComPtr<ID3D11Texture2D> intermediateTexture;

    if (srcDesc.BindFlags & D3D11_BIND_SHADER_RESOURCE) {
        // 直接SRV作成可能
        srvSourceTexture = srcNV12;
    } else {
        // 中間テクスチャ経由
        intermediateTexture = AcquireNV12SRVTexture(srcDesc.Width, srcDesc.Height);

        // GPU内コピー
        UINT srcSubresource = D3D11CalcSubresource(0, srcIndex, 1);
        m_context->CopySubresourceRegion(
            intermediateTexture.Get(), 0, 0, 0, 0,
            srcNV12, srcSubresource, nullptr
        );

        srvSourceTexture = intermediateTexture;
    }

    // Y平面/UV平面それぞれのSRV作成 → RTV作成 → フルスクリーンクアッド描画でシェーダー変換
    // （詳細は D3D11Context.cpp:629-721）

    if (intermediateTexture) {
        ReleaseNV12SRVTexture(intermediateTexture.Get());
    }
    return true;
}
```

（要点抜粋、実装全体は `D3D11Context.cpp:574-734`）

### 色空間について（既知の制限）

NV12→RGBA変換用のピクセルシェーダーは、色変換行列とオフセットを次のように
**定数として埋め込んで**います（`D3D11Context.cpp:41-71`）。

```hlsl
static const float3x3 YUVtoRGB_BT709 = float3x3(
    1.164384f,  0.000000f,  1.792741f,
    1.164384f, -0.213249f, -0.532909f,
    1.164384f,  2.112402f,  0.000000f
);

static const float Y_OFFSET  = 16.0f  / 255.0f;  // limited-range(16-235)の黒レベル
static const float UV_OFFSET = 128.0f / 255.0f;
```

（`D3D11Context.cpp:51-58`）

**既知の制限:**

- 変換行列は常に **BT.709**、レンジは常に **limited-range（16-235 / 16-240）** を前提とした
  固定値であり、入力`AVFrame`の`colorspace` / `color_range` / `color_primaries`等の
  色空間メタデータを一切参照しない。`ConvertNV12ToRGBA()`のシグネチャにも色空間を
  指定する引数は存在しない（`D3D11Context.h:142-146`）。
- そのため、BT.601系（SD解像度の動画に多い）やfull-range（PC record）でエンコードされた
  入力に対しては、色再現が実際の色空間と一致せず、若干のずれが生じ得る。
- ソフトウェアフォールバック経路（`FrameConverter::ConvertSoftware`が呼ぶ`libswscale`の
  `sws_getContext`）側も、`sws_setColorspaceDetails`等による明示的な色空間指定を
  行っておらず（`FrameConverter.cpp`に該当呼び出しなし）、libswscaleのデフォルト推定に
  委ねられている。したがってGPU経路とソフトウェア経路とで色の再現結果が異なる
  可能性がある。
- 現時点でこの制限に対する自動判定・切り替えロジックは実装されていない。

### パフォーマンス効果

| 項目 | CPU変換 | GPU変換（中間テクスチャ経由） |
|------|---------|------------------------------|
| CopySubresourceRegion | - | GPU内コピー（追加のCPU同期なし） |
| NV12→RGBA変換 | `sws_scale`によるCPU変換 | ピクセルシェーダーによるGPU変換 |

> 上記は処理内容の定性的な比較であり、具体的なms単位の数値はリポジトリ内で
> 実測・ベンチマークされていない（未検証の見積もりを断定的に記載することを避けるため、
> 数値は記載しない）。

### テスト項目

| テスト名 | 内容 |
|----------|------|
| `Test_NV12Pool_AcquireReturnsValidTexture` | 取得したテクスチャが有効かつ正しい属性 |
| `Test_NV12Pool_TextureHasSRVBindFlag` | SRVフラグがあり、SRV作成が成功 |
| `Test_NV12Pool_ReusesSameTexture` | 解放後に同じテクスチャが再利用される |
| `Test_NV12Pool_DifferentSizeCreatesNew` | 異なるサイズでは新規作成 |
| `Test_NV12Pool_ReleaseMakesAvailable` | 解放操作で再利用可能になる |
| `Test_ClearNV12Pool_WorksAfterClear` | クリア後も正常動作 |

（`cpp/tests/test_nv12_srv_pool.cpp:269-274`）

### ファイル

- ヘッダー: `cpp/src/graphics/D3D11Context.h`
- 実装: `cpp/src/graphics/D3D11Context.cpp`（`ConvertNV12ToRGBA`: 574-734, `AcquireNV12SRVTexture`: 953-986, `ReleaseNV12SRVTexture`: 988-998, `ClearNV12Pool`: 1000-1003）
- テスト: `cpp/tests/test_nv12_srv_pool.cpp`

### 注意事項

- `Shutdown()` 時に自動で `ClearNV12Pool()` が呼ばれる
- `CopySubresourceRegion()` はGPU内コピーであり、`ReadbackTexture()`のようなCPU側での
  Map/Unmapは発生しない
- 中間テクスチャはArraySizeが1のため、`D3D11_SRV_DIMENSION_TEXTURE2D`を使用

## 省電力最適化: フレーム取得要求ウィンドウ（P-10）

### 背景

非同期リードバック（前節）はGPU→CPU転送によるCPU側のストールを回避しますが、
それでも毎フレーム「ステージングテクスチャへのコピー・クエリ発行・（完了時の）
Map/Unmap」というGPU/CPUの作業が発生します。GUI（Python側）がプレビュー表示用に
フレームを取得していない期間（例: プレビューウィンドウ非表示など）は、この
リードバック処理自体が不要な電力・帯域消費になります。

`VideoPlayer` はGUI側の最終フレーム取得要求時刻を記録し、一定時間（実装上の既定値は
3秒）操作がなければ非同期リードバックの開始そのものを省略します。実装コメントでは
この最適化を `P-10` と呼称しています。

### 実装

```cpp
// VideoPlayer.cpp:29-37 （匿名namespace内）
int64_t NowNanos() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
// 直近このウィンドウ内にフレーム取得要求があった場合のみ非同期リードバックを行う
constexpr int64_t kReadbackActiveWindowNanos = 3LL * 1000 * 1000 * 1000;  // 3秒

// VideoPlayer::Impl（VideoPlayer.cpp:96）
std::atomic<int64_t> lastFrameRequestNanos{ 0 };
```

- `VideoPlayer::GetCurrentFrameData()`（GUIがフレームを取得するために呼び出すAPI）は、
  呼び出しの都度 `lastFrameRequestNanos` を現在時刻で更新する（`VideoPlayer.cpp:966-969`）。
- `ProcessFrame()` は各フレームで
  `NowNanos() - lastFrameRequestNanos.load(std::memory_order_relaxed) < kReadbackActiveWindowNanos`
  を判定し（`readbackActive`、`VideoPlayer.cpp:633-634`）、`true`の場合のみ
  ペンディングリードバックの完了確認・新規`BeginAsyncReadback()`開始の一連の処理を
  実行する（`VideoPlayer.cpp:631-665`）。`false`の場合はリードバック関連の処理を
  完全にスキップする。**Spout送信自体はこの判定の影響を受けず、毎フレーム継続する**
  （`VideoPlayer.cpp:667-677`、readbackのブロックの外側）。
- `Start()` 完了直後に `lastFrameRequestNanos` を現在時刻で初期化するため
  （`VideoPlayer.cpp:343-345`）、GUIがまだ一度もフレームを要求していない起動直後
  数秒間もリードバックは有効になる（プレビューが空白のまま固まるのを防ぐ猶予期間）。

### 効果

| 項目 | GUI要求が直近3秒以内にある場合 | GUI要求が3秒以上ない場合 |
|------|---------------------------------|------------------------------|
| ステージングテクスチャへのコピー（`BeginAsyncReadback`内） | 毎フレーム実行 | 実行しない |
| クエリ発行・Map/Unmap | 毎フレーム実行 | 実行しない |
| Spout送信 | 実行（影響なし） | 実行（影響なし） |

### 注意事項

- ウィンドウ時間 `kReadbackActiveWindowNanos`（3秒）は`VideoPlayer.cpp`内のコンパイル時
  定数であり、実行時に変更するAPIは現状存在しない。
- GUIが3秒以上フレームを取得しなかった後に取得を再開した場合、`GetCurrentFrameData()`
  呼び出し時点で`lastFrameRequestNanos`が更新されるため、次の`ProcessFrame()`から
  即座にリードバックが再開される（復帰のための追加の待機時間はない）。
- この省電力化はGPU→CPUリードバック（プレビュー用フレーム取得経路）のみが対象であり、
  Spout配信そのもの・動画再生自体には影響しない。

### ファイル

- 実装: `cpp/src/player/VideoPlayer.cpp`（定数/ヘルパー: 29-37、状態フィールド: 96、
  起動時初期化: 343-345、判定とゲート: 628-665、GUI要求記録: 966-969）

## テクスチャプール（TexturePool / PooledTexture）

### 背景

`D3D11Context`内部の「ステージングテクスチャプール」「NV12 SRVテクスチャプール」とは
別に、デコード後のRGBA変換先テクスチャ（Spout送信元）を再利用するための独立した
プールクラス`TexturePool`（`cpp/src/graphics/TexturePool.h/.cpp`）が存在します。
`VideoPlayer`は毎フレーム、このプールから`PooledTexture`を1つ取得し（`Acquire()`）、
変換・リードバック・Spout送信に使い回します。

### 設計

- `TexturePool`は`std::enable_shared_from_this<TexturePool>`を継承しており
  （`TexturePool.h:72`）、`Acquire()`内部で`weak_from_this()`を使ってプールへの弱参照を
  持つ`PooledTexture`を生成して返します（`TexturePool.cpp:208`）。
- そのため`TexturePool`は必ず`std::shared_ptr`管理下で生成する必要があります
  （スタック上や生ポインタで生成すると`Acquire()`呼び出し時に`std::bad_weak_ptr`で
  落ちる。クラスコメント`TexturePool.h:68-71`に明記）。実際に`VideoPlayer::Start()`は
  `m_impl->texturePool = std::make_shared<TexturePool>();`として生成しています
  （`VideoPlayer.cpp:310`、フィールド宣言は`std::shared_ptr<TexturePool> texturePool;`、
  `VideoPlayer.cpp:51-53`）。

### PooledTextureのuse-after-free回避（weak_ptr）

`PooledTexture`はデストラクタ（またはムーブ代入時の解放）で自動的に
`TexturePool::Release()`を呼んでテクスチャを返却します。プールへの参照は生ポインタ
ではなく`std::weak_ptr<TexturePool>`として保持しており（`TexturePool.h:62`）、返却時には
`lock()`でプールがまだ生存しているかを確認してから呼び出します（`TexturePool.cpp:46-57`）。

```cpp
void PooledTexture::Release() {
    // weak_ptr::lock()でプールがまだ生存しているか確認してから返却する。
    // Stop()等でTexturePoolが既に破棄済み（reset済み）の場合はlock()がnullptrを返すため、
    // 解放済みメモリへのアクセス（use-after-free）を避け、ComPtrの解放のみ行う。
    if (m_texture) {
        if (auto pool = m_pool.lock()) {
            pool->Release(m_index);
        }
        m_pool.reset();
    }
    m_texture.Reset();
}
```

（`TexturePool.cpp:46-57`）

これにより、`VideoPlayer::Stop()`が`m_impl->texturePool.reset()`（`VideoPlayer.cpp:379`）で
プールを破棄した後に、（例えば呼び出し元にまだ残っていた）`PooledTexture`が
デストラクトされても、解放済みの`TexturePool`インスタンスへアクセスすることはありません
（`lock()`が`nullptr`を返し、テクスチャの`ComPtr`解放のみが行われます）。

### 主なメソッド

| メソッド | 内容 |
|----------|------|
| `Acquire()` | 利用可能なテクスチャを取得。枯渇時は`allowGrowth`（既定`true`）なら`maxPoolSize`（既定8）まで動的拡張する（`TexturePool.cpp:158-209`） |
| `Release(index)` | インデックス指定でプールに返却（通常は`PooledTexture`のデストラクタ経由で間接的に呼ばれる） |
| `Reset(config)` | 既存テクスチャを破棄し、新しい設定で再作成する |
| `GetAvailableCount()` / `GetInUseCount()` | 状態監視用 |

### ファイル

- ヘッダー: `cpp/src/graphics/TexturePool.h`
- 実装: `cpp/src/graphics/TexturePool.cpp`
- 利用側: `cpp/src/player/VideoPlayer.cpp`（フィールド: 51-53, 生成: 309-320, 破棄: 379）

## SpoutSender: 解像度変化時の再作成とロールバック

### 背景

動画の解像度は再生開始後に変化し得ます（例: 適応ストリーミングでの解像度切替）。
`SpoutSender::SendTexture()`は渡されたテクスチャの実サイズが前回と異なる場合に、
共有テクスチャとSpout Senderを作り直します。作成処理には（他プロセスとの共有ハンドル
登録を含む）失敗し得るAPI呼び出しが複数関わるため、失敗時に内部状態を不整合な
まま残さない実装になっています。

### 実装

```cpp
bool SpoutSender::SendTexture(ID3D11Texture2D* texture, unsigned int width, unsigned int height) {
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    // ...

    // m_impl->width/height は作成が成功した場合にのみ更新する
    bool needsRecreate = (width != m_impl->width || height != m_impl->height);

    if (needsRecreate) {
        m_impl->ReleaseSender();  // 既存のSender/共有テクスチャを解放

        // 共有テクスチャ作成 → ローカル変数で受け取り、成功するまでメンバへコミットしない
        ID3D11Texture2D* newSharedTexture = nullptr;
        HANDLE newShareHandle = nullptr;
        if (!m_impl->spoutDX.CreateSharedDX11Texture(
                m_impl->device, width, height, format, &newSharedTexture, newShareHandle)) {
            // m_impl->width/height は更新しない（次フレームでも同解像度ならneedsRecreateが
            // 再びtrueになり、自動的に再試行される）
            return false;
        }

        if (!m_impl->senderNames.CreateSender(
                m_impl->senderName.c_str(), width, height, newShareHandle, static_cast<DWORD>(format))) {
            newSharedTexture->Release();
            return false;  // 同上：メンバは未コミットのまま
        }

        // ここまで到達して初めて状態をコミット
        m_impl->sharedTexture = newSharedTexture;
        m_impl->shareHandle = newShareHandle;
        m_impl->width = width;
        m_impl->height = height;
        m_impl->senderCreated = true;
    }

    // 入力テクスチャを共有テクスチャへコピーして送信
    // ...
    return true;
}
```

（`SpoutSender.cpp:157-246`）

### ロールバック挙動の要点

- `m_impl->width` / `m_impl->height` は、共有テクスチャとSenderの両方の作成に
  **成功した場合のみ**更新（コミット）される。失敗時はどちらも更新されない
  （`SpoutSender.cpp:170-173`のコメント参照）。
- この「失敗時は前の値のまま据え置く」設計により、次フレームで同じ解像度の
  テクスチャが渡されても`needsRecreate`は再び`true`と判定され、作成が自動的に
  再試行される。明示的に「以前の値へ戻す」処理を行っているわけではなく、
  「成功するまで新しい値を書き込まない」ことで実質的なロールバックを実現している。
- `SetSize(width, height)`（`SpoutSender.cpp:278-306`）はテクスチャの実サイズを
  事前に予約するAPIではなく、次回`SendTexture()`呼び出しでキャッシュ再作成を
  強制するための無効化専用メソッドである。呼び出すと内部の`width`/`height`を
  `0`にリセットするため、次回`SendTexture()`呼び出し時に渡された実際のテクスチャ
  サイズに基づいて再作成が行われる（ヘッダーコメント`SpoutSender.h:78-86`に明記）。

### ファイル

- ヘッダー: `cpp/src/graphics/SpoutSender.h`
- 実装: `cpp/src/graphics/SpoutSender.cpp`（`SendTexture`: 157-246, `SetSenderName`: 252-276, `SetSize`: 278-306）

## FrameConverter: SwsContext追跡とHW/SWフォールバック

### 背景

`FrameConverter::Convert()`は、ハードウェアデコードされたNV12フレーム
（`AV_PIX_FMT_D3D11`）をGPUシェーダー経由でRGBAに変換することを優先しつつ、
失敗時や非HWフレームに対しては`libswscale`（`sws_scale`）によるソフトウェア変換に
フォールバックします。

### GPU→ソフトウェアの恒久フォールバック

```cpp
bool FrameConverter::Convert(AVFrame* frame, ID3D11Texture2D* dstTexture) {
    bool isHWFrame = (frame->format == AV_PIX_FMT_D3D11);

    // GPU変換を試行（一度失敗したらソフトウェアにフォールバック）
    if (isHWFrame && !m_impl->forceSwConversion && m_impl->gpuConversionSupported) {
        if (ConvertHardware(frame, dstTexture)) {
            return true;
        }
        // GPU変換が失敗した場合、以後はソフトウェアのみ使用
        m_impl->gpuConversionSupported = false;
    }

    return ConvertSoftware(frame, dstTexture);
}
```

（`FrameConverter.cpp:99-125`）

- `gpuConversionSupported`は一度`false`になると再び`true`に戻ることはなく、
  以後そのプロセスの生存期間中はソフトウェア変換のみが使われる（1回きりの恒久
  フォールバック）。
- これは`cpp/src/decoder/VideoDecoder.cpp`が行う「HWデコード（D3D11VA）自体の
  初期化/実行時失敗 → ソフトウェアデコードへのフォールバック」（`VideoDecoder.cpp`内
  `MED-2`のコメント、886行目付近）とは別レイヤーの話である点に注意。`VideoDecoder`側の
  フォールバックはデコード自体をCPUで行うか否かを切り替えるものであり、
  `FrameConverter`側のフォールバックはHWデコード済みNV12フレームの色変換を
  GPUシェーダーで行うかCPU（`sws_scale`）で行うかを切り替えるものであって、
  それぞれ独立して発生し得る。

### SwsContextのsrc/dst解像度追跡

`ConvertSoftware()`は入力（src: フレームの実解像度・フォーマット）だけでなく、
出力（dst: `dstTexture`の解像度、または`SetOutputSize()`で明示指定された解像度）が
前回と異なる場合にも`SwsContext`を再生成します。

```cpp
if (!m_impl->swsContext ||
    m_impl->lastSrcWidth  != srcWidth  ||
    m_impl->lastSrcHeight != srcHeight ||
    m_impl->lastSrcFormat != srcFormat ||
    m_impl->lastDstWidth  != dstWidth  ||
    m_impl->lastDstHeight != dstHeight) {
    // sws_freeContext → sws_getContext で再生成
    // 生成失敗時はキャッシュ済み寸法情報（lastSrcWidth等）を0にリセットし、
    // 次回呼び出しで確実に再試行されるようにする
}
```

（`FrameConverter.cpp:164-209`）

dstのみを追跡対象から外すと、古いdst解像度用に構成された`SwsContext`のまま新しい
バッファサイズで`sws_scale`を呼んでしまい、バッファオーバーフローや不正なスケーリング
を招きます（ソースコード中のコメント`MED-3`参照）。

### sws_scale失敗時にConvertはfalseを返す

```cpp
int scaledHeight = sws_scale(
    m_impl->swsContext,
    srcFrame->data, srcFrame->linesize,
    0, srcHeight,
    dstData, dstLinesize
);

// sws_scaleの戻り値（出力スライス高さ）を検証する。
// 失敗(<=0)または期待した高さと一致しない場合は、未初期化/前フレームの
// バッファをそのままアップロードしないよう false を返す。
if (scaledHeight <= 0 || scaledHeight != dstHeight) {
    return false;
}
```

（`FrameConverter.cpp:221-238`）

`sws_scale`の戻り値を検証し、失敗または期待値と異なる場合は`ConvertSoftware()`、
ひいては`Convert()`が`false`を返します。呼び出し元の`VideoPlayer::ProcessFrame()`は
変換失敗時、そのフレームをスキップして次フレームへ進みます（`LOG_WARN`後に
`return true`、`VideoPlayer.cpp:605-608`）。

### ファイル

- ヘッダー: `cpp/src/decoder/FrameConverter.h`
- 実装: `cpp/src/decoder/FrameConverter.cpp`（`Convert`: 99-125, `ConvertSoftware`: 127-252, `ConvertHardware`: 254-277）
