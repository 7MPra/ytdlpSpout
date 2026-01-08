// =============================================================================
// D3D11Context.h - DirectX11 デバイス管理
// =============================================================================
//
// 機能:
//   - DirectX11 デバイス・デバイスコンテキストの作成・管理
//   - テクスチャ作成ヘルパー
//   - NV12→RGBA GPU変換（シェーダー使用）
//   - オフスクリーンレンダリング（スワップチェーン不要）
//   - ステージングテクスチャプール（ReadbackTexture最適化）
//
// =============================================================================

#pragma once

#include <d3d11.h>
#include <dxgi.h>
#include <string>
#include <memory>
#include <vector>
#include <mutex>
#include "utils/ComPtr.h"

namespace ytdlpspout {

/// @brief DirectX11コンテキストクラス
/// @details デバイス管理とテクスチャ操作を提供
class D3D11Context {
public:
    // =========================================================================
    // 非同期リードバック用ハンドル
    // =========================================================================

    /// @brief 非同期リードバック用ハンドル
    struct AsyncReadbackHandle {
        ComPtr<ID3D11Texture2D> stagingTexture;
        ComPtr<ID3D11Query> query;  // D3D11_QUERY_EVENT
        UINT width = 0;
        UINT height = 0;
        bool valid = false;
    };

    D3D11Context();
    ~D3D11Context();

    // コピー禁止
    D3D11Context(const D3D11Context&) = delete;
    D3D11Context& operator=(const D3D11Context&) = delete;

    // ムーブ許可
    D3D11Context(D3D11Context&&) noexcept;
    D3D11Context& operator=(D3D11Context&&) noexcept;

    // =========================================================================
    // 初期化・終了
    // =========================================================================

    /// @brief DirectX11デバイスの初期化
    /// @param preferHardware ハードウェアアダプタを優先するか
    /// @return 成功した場合true
    bool Initialize(bool preferHardware = true);

    /// @brief リソースの解放
    void Shutdown();

    /// @brief 初期化済みかどうか
    /// @return 初期化済みの場合true
    bool IsInitialized() const;

    // =========================================================================
    // デバイス取得
    // =========================================================================

    /// @brief D3D11デバイスを取得
    /// @return デバイスポインタ（所有権は移動しない）
    ID3D11Device* GetDevice() const;

    /// @brief D3D11デバイスコンテキストを取得
    /// @return デバイスコンテキストポインタ
    ID3D11DeviceContext* GetDeviceContext() const;

    /// @brief DXGIアダプタを取得
    /// @return アダプタポインタ
    IDXGIAdapter* GetAdapter() const;

    // =========================================================================
    // テクスチャ作成
    // =========================================================================

    /// @brief 2Dテクスチャを作成
    /// @param width 幅
    /// @param height 高さ
    /// @param format ピクセルフォーマット
    /// @param bindFlags バインドフラグ
    /// @param usage 使用方法
    /// @param cpuAccessFlags CPUアクセスフラグ
    /// @return 作成されたテクスチャ
    ComPtr<ID3D11Texture2D> CreateTexture2D(
        UINT width,
        UINT height,
        DXGI_FORMAT format = DXGI_FORMAT_B8G8R8A8_UNORM,
        UINT bindFlags = D3D11_BIND_SHADER_RESOURCE,
        D3D11_USAGE usage = D3D11_USAGE_DEFAULT,
        UINT cpuAccessFlags = 0
    );

    /// @brief ステージングテクスチャを作成（CPU読み書き用）
    /// @param width 幅
    /// @param height 高さ
    /// @param format ピクセルフォーマット
    /// @return 作成されたステージングテクスチャ
    ComPtr<ID3D11Texture2D> CreateStagingTexture(
        UINT width,
        UINT height,
        DXGI_FORMAT format = DXGI_FORMAT_B8G8R8A8_UNORM
    );

    /// @brief レンダーターゲット用テクスチャを作成
    /// @param width 幅
    /// @param height 高さ
    /// @param format ピクセルフォーマット
    /// @return 作成されたレンダーターゲットテクスチャ
    ComPtr<ID3D11Texture2D> CreateRenderTargetTexture(
        UINT width,
        UINT height,
        DXGI_FORMAT format = DXGI_FORMAT_B8G8R8A8_UNORM
    );

    // =========================================================================
    // NV12→RGBA 変換
    // =========================================================================

    /// @brief NV12変換用シェーダーリソースを初期化
    /// @return 成功した場合true
    bool InitializeNV12Converter();

    /// @brief NV12テクスチャをRGBAに変換
    /// @param srcNV12 NV12フォーマットのソーステクスチャ（配列テクスチャ対応）
    /// @param srcIndex ソーステクスチャ配列内のインデックス
    /// @param dstRGBA 出力RGBAテクスチャ
    /// @return 成功した場合true
    bool ConvertNV12ToRGBA(
        ID3D11Texture2D* srcNV12,
        UINT srcIndex,
        ID3D11Texture2D* dstRGBA
    );

    /// @brief CPU側データからテクスチャへコピー
    /// @param texture 対象テクスチャ
    /// @param data ソースデータ
    /// @param rowPitch 行ピッチ（バイト）
    /// @return 成功した場合true
    bool UpdateTexture(
        ID3D11Texture2D* texture,
        const void* data,
        UINT rowPitch
    );

    /// @brief テクスチャ間コピー
    /// @param dst コピー先
    /// @param src コピー元
    void CopyTexture(ID3D11Texture2D* dst, ID3D11Texture2D* src);

    /// @brief テクスチャからピクセルデータをCPUに読み取る
    /// @param texture ソーステクスチャ
    /// @param outData 出力バッファ（呼び出し側で確保、width*height*4バイト必要）
    /// @param bufferSize バッファサイズ
    /// @return 成功した場合true
    bool ReadbackTexture(ID3D11Texture2D* texture, void* outData, size_t bufferSize);

    // =========================================================================
    // 非同期リードバック
    // =========================================================================

    /// @brief 非同期リードバックを開始
    /// @param texture ソーステクスチャ
    /// @return 非同期ハンドル
    AsyncReadbackHandle BeginAsyncReadback(ID3D11Texture2D* texture);

    /// @brief 非同期リードバックが完了したかチェック
    /// @param handle ハンドル
    /// @return 完了していればtrue
    bool IsReadbackComplete(const AsyncReadbackHandle& handle);

    /// @brief 非同期リードバックの結果を取得
    /// @param handle ハンドル
    /// @param outData 出力バッファ
    /// @param bufferSize バッファサイズ
    /// @return 成功時true
    bool CompleteAsyncReadback(AsyncReadbackHandle& handle, void* outData, size_t bufferSize);

    // =========================================================================
    // ステージングテクスチャプール
    // =========================================================================

    /// @brief プールからステージングテクスチャを取得（または新規作成）
    /// @param width 幅
    /// @param height 高さ
    /// @param format ピクセルフォーマット
    /// @return ステージングテクスチャ（使用後はReleaseStagingTextureで返却）
    ComPtr<ID3D11Texture2D> AcquireStagingTexture(UINT width, UINT height, DXGI_FORMAT format);

    /// @brief ステージングテクスチャをプールに返却
    /// @param texture 返却するテクスチャ
    void ReleaseStagingTexture(ID3D11Texture2D* texture);

    /// @brief ステージングテクスチャプールをクリア
    void ClearStagingPool();

    // =========================================================================
    // NV12 SRVテクスチャプール（D3D11VAデコーダー用中間テクスチャ）
    // =========================================================================

    /// @brief SRV作成可能なNV12中間テクスチャを取得
    /// @param width 幅
    /// @param height 高さ
    /// @return NV12テクスチャ（使用後はReleaseNV12SRVTextureで返却）
    ComPtr<ID3D11Texture2D> AcquireNV12SRVTexture(UINT width, UINT height);

    /// @brief NV12中間テクスチャをプールに返却
    /// @param texture 返却するテクスチャ
    void ReleaseNV12SRVTexture(ID3D11Texture2D* texture);

    /// @brief NV12プールをクリア
    void ClearNV12Pool();

    /// @brief GPU処理完了を待機（コマンドバッファをフラッシュ）
    void Flush();

    // =========================================================================
    // デバイス情報
    // =========================================================================

    /// @brief GPUアダプタの名前を取得
    /// @return アダプタ名
    std::string GetAdapterName() const;

    /// @brief VRAM容量を取得（バイト）
    /// @return VRAM容量
    size_t GetVideoMemorySize() const;

    /// @brief フィーチャーレベルを取得
    /// @return D3D_FEATURE_LEVEL値
    D3D_FEATURE_LEVEL GetFeatureLevel() const;

private:
    // D3D11デバイス関連
    ComPtr<ID3D11Device> m_device;
    ComPtr<ID3D11DeviceContext> m_context;
    ComPtr<IDXGIAdapter> m_adapter;
    D3D_FEATURE_LEVEL m_featureLevel = D3D_FEATURE_LEVEL_11_0;

    // NV12変換用シェーダーリソース
    ComPtr<ID3D11VertexShader> m_vertexShader;
    ComPtr<ID3D11PixelShader> m_pixelShader;
    ComPtr<ID3D11SamplerState> m_sampler;
    ComPtr<ID3D11Buffer> m_vertexBuffer;
    ComPtr<ID3D11InputLayout> m_inputLayout;
    ComPtr<ID3D11RasterizerState> m_rasterizerState;
    ComPtr<ID3D11BlendState> m_blendState;

    bool m_initialized = false;
    bool m_nv12ConverterInitialized = false;

    // ステージングテクスチャプール
    struct StagingTextureEntry {
        ComPtr<ID3D11Texture2D> texture;
        UINT width;
        UINT height;
        DXGI_FORMAT format;
        bool inUse;
    };

    std::vector<StagingTextureEntry> m_stagingPool;
    std::mutex m_stagingPoolMutex;
    static constexpr size_t kMaxStagingPoolSize = 4;

    // NV12 SRVテクスチャプール（D3D11VA中間テクスチャ用）
    struct NV12TextureEntry {
        ComPtr<ID3D11Texture2D> texture;
        UINT width;
        UINT height;
        bool inUse;
    };

    std::vector<NV12TextureEntry> m_nv12Pool;
    std::mutex m_nv12PoolMutex;
    static constexpr size_t kMaxNV12PoolSize = 4;

    // 内部ヘルパー
    bool CreateShaderResources();
    bool CreateFullscreenQuad();
};

} // namespace ytdlpspout
