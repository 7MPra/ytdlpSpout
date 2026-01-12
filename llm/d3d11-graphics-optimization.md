# D3D11 Graphics Optimization

このドキュメントはD3D11グラフィックスモジュールの最適化設計を説明します。

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

### アルゴリズム

#### AcquireStagingTexture()

1. プール内から未使用かつサイズ・フォーマットが一致するテクスチャを検索
2. 見つかれば `inUse = true` に設定して返却
3. 見つからなければ新規作成し、プールサイズ上限内ならプールに追加

```cpp
ComPtr<ID3D11Texture2D> AcquireStagingTexture(UINT width, UINT height, DXGI_FORMAT format) {
    std::lock_guard<std::mutex> lock(m_stagingPoolMutex);

    // 再利用可能なテクスチャを検索
    for (auto& entry : m_stagingPool) {
        if (!entry.inUse && 
            entry.width == width && 
            entry.height == height && 
            entry.format == format) {
            entry.inUse = true;
            return entry.texture;
        }
    }

    // 新規作成
    auto newTexture = CreateStagingTexture(width, height, format);
    if (newTexture && m_stagingPool.size() < kMaxStagingPoolSize) {
        m_stagingPool.push_back({newTexture, width, height, format, true});
    }
    return newTexture;
}
```

#### ReleaseStagingTexture()

1. プール内から該当テクスチャを検索
2. 見つかれば `inUse = false` に設定

```cpp
void ReleaseStagingTexture(ID3D11Texture2D* texture) {
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

### パフォーマンス効果

| 項目 | 旧実装 | 新実装 |
|------|--------|--------|
| 毎フレームのアロケーション | 1回 | 0回（再利用時） |
| GPU→CPUコピー | 変化なし | 変化なし |
| メモリプール上限 | なし | 4テクスチャ |

### スレッドセーフティ

- `std::mutex` によりプール操作は排他制御
- 複数スレッドからの同時アクセスに対応

### テスト項目

| テスト名 | 内容 |
|----------|------|
| `StagingPool_AcquireReturnsValidTexture` | 取得したテクスチャが有効かつ正しい属性 |
| `StagingPool_ReusesSameTexture` | 解放後に同じテクスチャが再利用される |
| `StagingPool_DifferentSizeCreatesNew` | 異なるサイズでは新規作成 |
| `StagingPool_ReleaseMakesAvailable` | 解放操作で再利用可能になる |
| `ReadbackTexture_UsesPool` | ReadbackTexture()がプールを使用 |
| `ClearStagingPool_WorksAfterClear` | クリア後も正常動作 |

### ファイル

- ヘッダー: `cpp/src/graphics/D3D11Context.h`
- 実装: `cpp/src/graphics/D3D11Context.cpp`
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

### API

#### BeginAsyncReadback()

GPUテクスチャからステージングテクスチャへの非同期コピーを開始し、完了検知用のクエリを発行します。

```cpp
AsyncReadbackHandle BeginAsyncReadback(ID3D11Texture2D* texture) {
    AsyncReadbackHandle handle;
    
    // ステージングテクスチャをプールから取得
    handle.stagingTexture = AcquireStagingTexture(width, height, format);
    
    // 非同期コピー開始
    m_context->CopyResource(handle.stagingTexture.Get(), texture);
    
    // GPU完了クエリを発行
    D3D11_QUERY_DESC queryDesc = { D3D11_QUERY_EVENT };
    m_device->CreateQuery(&queryDesc, &handle.query);
    m_context->End(handle.query.Get());
    
    handle.valid = true;
    return handle;
}
```

#### IsReadbackComplete()

GPUコピーが完了したかをノンブロッキングでチェックします。

```cpp
bool IsReadbackComplete(const AsyncReadbackHandle& handle) {
    BOOL queryData = FALSE;
    HRESULT hr = m_context->GetData(
        handle.query.Get(), &queryData, sizeof(BOOL),
        D3D11_ASYNC_GETDATA_DONOTFLUSH);
    return (hr == S_OK && queryData == TRUE);
}
```

#### CompleteAsyncReadback()

完了したリードバックの結果を取得し、ステージングテクスチャをプールに返却します。

```cpp
bool CompleteAsyncReadback(AsyncReadbackHandle& handle, void* outData, size_t bufferSize) {
    // Map/Unmapでデータ取得
    D3D11_MAPPED_SUBRESOURCE mapped;
    m_context->Map(handle.stagingTexture.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    
    // ピクセルデータをコピー（RowPitch考慮）
    // ...
    
    m_context->Unmap(handle.stagingTexture.Get(), 0);
    
    // プールに返却
    ReleaseStagingTexture(handle.stagingTexture.Get());
    handle.valid = false;
    return true;
}
```

### VideoPlayerでのダブルバッファリング

VideoPlayer::Impl構造体に以下を追加:

```cpp
D3D11Context::AsyncReadbackHandle pendingReadback;
bool hasPendingReadback = false;
```

ProcessFrame()での使用パターン:

```cpp
// 前フレームの非同期リードバックが完了していれば結果を取得
if (hasPendingReadback && d3dContext->IsReadbackComplete(pendingReadback)) {
    d3dContext->CompleteAsyncReadback(pendingReadback, buffer.data(), bufferSize);
    hasValidFrame = true;
    hasPendingReadback = false;
}

// 新しい非同期リードバックを開始
if (!hasPendingReadback) {
    pendingReadback = d3dContext->BeginAsyncReadback(currentTexture);
    hasPendingReadback = pendingReadback.valid;
}
```

### パフォーマンス効果

| 項目 | 同期リードバック | 非同期リードバック |
|------|-----------------|-------------------|
| CPUブロック | 毎フレーム | なし（完了時のみ） |
| レイテンシ | 1フレーム | 1-2フレーム |
| スループット | GPU依存 | パイプライン化で向上 |

### テスト項目

| テスト名 | 内容 |
|----------|------|
| `AsyncReadback_BeginReturnsValidHandle` | ハンドルが有効で正しい属性 |
| `AsyncReadback_IsCompleteReturnsFalseInitially` | 開始直後はfalse、Flush後はtrue |
| `AsyncReadback_CompleteRetrievesCorrectData` | 正しいピクセルデータが取得できる |
| `AsyncReadback_DoubleBufferNoStall` | ダブルバッファリングでストールなし |

### ファイル

- ヘッダー: `cpp/src/graphics/D3D11Context.h`
- 実装: `cpp/src/graphics/D3D11Context.cpp`
- VideoPlayer統合: `cpp/src/player/VideoPlayer.cpp`
- テスト: `cpp/tests/test_d3d11_async_readback.cpp`

### 注意事項

- `BeginAsyncReadback()` で取得したハンドルは必ず `CompleteAsyncReadback()` で完了させるか、停止時にステージングテクスチャを返却すること
- VideoPlayer::Stop()ではペンディングのリードバックをクリーンアップする
- 1フレームの遅延が発生するため、フレーム正確性が必要な場合は同期版を使用

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
│  BindFlags: 0        │            <1ms               │  BindFlags: SRV      │
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

### アルゴリズム

#### AcquireNV12SRVTexture()

1. プール内から未使用かつサイズが一致するテクスチャを検索
2. 見つかれば `inUse = true` に設定して返却
3. 見つからなければ`D3D11_BIND_SHADER_RESOURCE`フラグ付きで新規作成

```cpp
ComPtr<ID3D11Texture2D> AcquireNV12SRVTexture(UINT width, UINT height) {
    std::lock_guard<std::mutex> lock(m_nv12PoolMutex);

    // 再利用可能なテクスチャを検索
    for (auto& entry : m_nv12Pool) {
        if (!entry.inUse && entry.width == width && entry.height == height) {
            entry.inUse = true;
            return entry.texture;
        }
    }

    // 新規作成（SRVフラグ付き）
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = width;
    desc.Height = height;
    desc.Format = DXGI_FORMAT_NV12;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;  // 重要！
    // ...
    
    if (m_nv12Pool.size() < kMaxNV12PoolSize) {
        m_nv12Pool.push_back({newTexture, width, height, true});
    }
    return newTexture;
}
```

### ConvertNV12ToRGBA()の修正

```cpp
bool ConvertNV12ToRGBA(ID3D11Texture2D* srcNV12, UINT srcIndex, ID3D11Texture2D* dstRGBA) {
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
    
    // SRV作成 & シェーダー変換
    // ...
    
    if (intermediateTexture) {
        ReleaseNV12SRVTexture(intermediateTexture.Get());
    }
    return true;
}
```

### パフォーマンス効果

| 項目 | CPU変換 | GPU変換（中間テクスチャ経由） |
|------|---------|------------------------------|
| CopySubresourceRegion | - | <1ms（GPU内コピー） |
| NV12→RGBA変換 | 5-10ms | <1ms（シェーダー） |
| 合計 | 5-10ms | <2ms |

### テスト項目

| テスト名 | 内容 |
|----------|------|
| `NV12Pool_AcquireReturnsValidTexture` | 取得したテクスチャが有効かつ正しい属性 |
| `NV12Pool_TextureHasSRVBindFlag` | SRVフラグがあり、SRV作成が成功 |
| `NV12Pool_ReusesSameTexture` | 解放後に同じテクスチャが再利用される |
| `NV12Pool_DifferentSizeCreatesNew` | 異なるサイズでは新規作成 |
| `NV12Pool_ReleaseMakesAvailable` | 解放操作で再利用可能になる |
| `ClearNV12Pool_WorksAfterClear` | クリア後も正常動作 |

### ファイル

- ヘッダー: `cpp/src/graphics/D3D11Context.h`
- 実装: `cpp/src/graphics/D3D11Context.cpp`
- テスト: `cpp/tests/test_nv12_srv_pool.cpp`

### 注意事項

- `Shutdown()` 時に自動で `ClearNV12Pool()` が呼ばれる
- `CopySubresourceRegion()` はGPU内コピーのため高速（<1ms）
- 中間テクスチャはArraySizeが1のため、`D3D11_SRV_DIMENSION_TEXTURE2D`を使用
