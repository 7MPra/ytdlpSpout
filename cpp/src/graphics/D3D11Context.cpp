// =============================================================================
// D3D11Context.cpp - DirectX11 デバイス管理 実装
// =============================================================================

#include "D3D11Context.h"
#include "utils/Logger.h"
#include "utils/ErrorHandling.h"

#include <d3dcompiler.h>
#include <dxgi1_2.h>
#include <array>
#include <vector>

namespace ytdlpspout {

// =============================================================================
// 埋め込みシェーダーソース（ランタイムコンパイル用）
// =============================================================================

static const char* g_PassthroughVS_Source = R"(
struct VSInput {
    float3 position : POSITION;
    float2 texCoord : TEXCOORD0;
};
struct PSInput {
    float4 position : SV_POSITION;
    float2 texCoord : TEXCOORD0;
};
PSInput main(VSInput input) {
    PSInput output;
    output.position = float4(input.position, 1.0f);
    output.texCoord = input.texCoord;
    return output;
}
)";

static const char* g_NV12ToRGBA_PS_Source = R"(
Texture2D<float> luminanceTexture : register(t0);
Texture2D<float2> chrominanceTexture : register(t1);
SamplerState linearSampler : register(s0);

struct PSInput {
    float4 position : SV_POSITION;
    float2 texCoord : TEXCOORD0;
};

static const float3x3 YUVtoRGB_BT709 = float3x3(
    1.164384f,  0.000000f,  1.792741f,
    1.164384f, -0.213249f, -0.532909f,
    1.164384f,  2.112402f,  0.000000f
);

static const float Y_OFFSET = 16.0f / 255.0f;
static const float UV_OFFSET = 128.0f / 255.0f;

float4 main(PSInput input) : SV_TARGET {
    float y = luminanceTexture.Sample(linearSampler, input.texCoord);
    float2 uv = chrominanceTexture.Sample(linearSampler, input.texCoord);
    float y_adjusted = y - Y_OFFSET;
    float u = uv.x - UV_OFFSET;
    float v = uv.y - UV_OFFSET;
    float3 yuv = float3(y_adjusted, u, v);
    float3 rgb = mul(YUVtoRGB_BT709, yuv);
    rgb = saturate(rgb);
    return float4(rgb, 1.0f);
}
)";

// =============================================================================
// 頂点構造体（フルスクリーンクアッド用）
// =============================================================================
struct Vertex {
    float position[3];
    float texCoord[2];
};

// フルスクリーンクアッド頂点データ
static const Vertex g_FullscreenQuadVertices[] = {
    // 位置 (x, y, z),        テクスチャ座標 (u, v)
    { {-1.0f, -1.0f, 0.0f},  {0.0f, 1.0f} },  // 左下
    { {-1.0f,  1.0f, 0.0f},  {0.0f, 0.0f} },  // 左上
    { { 1.0f, -1.0f, 0.0f},  {1.0f, 1.0f} },  // 右下
    { { 1.0f,  1.0f, 0.0f},  {1.0f, 0.0f} },  // 右上
};

// =============================================================================
// コンストラクタ・デストラクタ
// =============================================================================

D3D11Context::D3D11Context() = default;

D3D11Context::~D3D11Context() {
    Shutdown();
}

// ムーブコンストラクタ/代入演算子の実装
// std::mutexはコピー/ムーブできないため、手動で実装
D3D11Context::D3D11Context(D3D11Context&& other) noexcept
    : m_device(std::move(other.m_device))
    , m_context(std::move(other.m_context))
    , m_adapter(std::move(other.m_adapter))
    , m_featureLevel(other.m_featureLevel)
    , m_vertexShader(std::move(other.m_vertexShader))
    , m_pixelShader(std::move(other.m_pixelShader))
    , m_sampler(std::move(other.m_sampler))
    , m_vertexBuffer(std::move(other.m_vertexBuffer))
    , m_inputLayout(std::move(other.m_inputLayout))
    , m_rasterizerState(std::move(other.m_rasterizerState))
    , m_blendState(std::move(other.m_blendState))
    , m_initialized(other.m_initialized)
    , m_nv12ConverterInitialized(other.m_nv12ConverterInitialized)
    // m_stagingPoolMutexは新しいインスタンスで初期化（移動不可）
{
    // プールの内容を移動 - デッドロック防止のためstd::lockを使用
    std::lock(m_stagingPoolMutex, other.m_stagingPoolMutex, m_nv12PoolMutex, other.m_nv12PoolMutex);
    std::lock_guard<std::mutex> lockStagingThis(m_stagingPoolMutex, std::adopt_lock);
    std::lock_guard<std::mutex> lockStagingOther(other.m_stagingPoolMutex, std::adopt_lock);
    std::lock_guard<std::mutex> lockNV12This(m_nv12PoolMutex, std::adopt_lock);
    std::lock_guard<std::mutex> lockNV12Other(other.m_nv12PoolMutex, std::adopt_lock);
    m_stagingPool = std::move(other.m_stagingPool);
    m_nv12Pool = std::move(other.m_nv12Pool);
    other.m_initialized = false;
    other.m_nv12ConverterInitialized = false;
}

D3D11Context& D3D11Context::operator=(D3D11Context&& other) noexcept {
    if (this != &other) {
        Shutdown();
        
        m_device = std::move(other.m_device);
        m_context = std::move(other.m_context);
        m_adapter = std::move(other.m_adapter);
        m_featureLevel = other.m_featureLevel;
        m_vertexShader = std::move(other.m_vertexShader);
        m_pixelShader = std::move(other.m_pixelShader);
        m_sampler = std::move(other.m_sampler);
        m_vertexBuffer = std::move(other.m_vertexBuffer);
        m_inputLayout = std::move(other.m_inputLayout);
        m_rasterizerState = std::move(other.m_rasterizerState);
        m_blendState = std::move(other.m_blendState);
        m_initialized = other.m_initialized;
        m_nv12ConverterInitialized = other.m_nv12ConverterInitialized;

        // プールの内容を移動 - デッドロック防止のためstd::lockを使用
        {
            std::lock(m_stagingPoolMutex, other.m_stagingPoolMutex, m_nv12PoolMutex, other.m_nv12PoolMutex);
            std::lock_guard<std::mutex> lockStagingThis(m_stagingPoolMutex, std::adopt_lock);
            std::lock_guard<std::mutex> lockStagingOther(other.m_stagingPoolMutex, std::adopt_lock);
            std::lock_guard<std::mutex> lockNV12This(m_nv12PoolMutex, std::adopt_lock);
            std::lock_guard<std::mutex> lockNV12Other(other.m_nv12PoolMutex, std::adopt_lock);
            m_stagingPool = std::move(other.m_stagingPool);
            m_nv12Pool = std::move(other.m_nv12Pool);
        }

        other.m_initialized = false;
        other.m_nv12ConverterInitialized = false;
    }
    return *this;
}

// =============================================================================
// 初期化・終了
// =============================================================================

bool D3D11Context::Initialize(bool preferHardware) {
    if (m_initialized) {
        LOG_WARN("D3D11Context already initialized");
        return true;
    }

    LOG_INFO("Initializing D3D11 context...");

    // デバイス作成フラグ
    UINT createDeviceFlags = 0;
#ifdef _DEBUG
    createDeviceFlags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    // フィーチャーレベル（優先順）
    std::array<D3D_FEATURE_LEVEL, 4> featureLevels = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0,
    };

    D3D_DRIVER_TYPE driverType = preferHardware ? D3D_DRIVER_TYPE_HARDWARE : D3D_DRIVER_TYPE_WARP;

    // デバイス作成
    HRESULT hr = D3D11CreateDevice(
        nullptr,                          // アダプタ（nullptrでデフォルト）
        driverType,                       // ドライバタイプ
        nullptr,                          // ソフトウェアラスタライザ
        createDeviceFlags,                // フラグ
        featureLevels.data(),             // フィーチャーレベル配列
        static_cast<UINT>(featureLevels.size()),
        D3D11_SDK_VERSION,
        m_device.GetAddressOf(),
        &m_featureLevel,
        m_context.GetAddressOf()
    );

    // ハードウェア失敗時はWARPにフォールバック
    if (FAILED(hr) && preferHardware) {
        LOG_WARN("Hardware D3D11 device creation failed, falling back to WARP");
        hr = D3D11CreateDevice(
            nullptr,
            D3D_DRIVER_TYPE_WARP,
            nullptr,
            createDeviceFlags,
            featureLevels.data(),
            static_cast<UINT>(featureLevels.size()),
            D3D11_SDK_VERSION,
            m_device.GetAddressOf(),
            &m_featureLevel,
            m_context.GetAddressOf()
        );
    }

    HR_CHECK(hr, "D3D11CreateDevice failed");

    // DXGIアダプタを取得
    ComPtr<IDXGIDevice> dxgiDevice;
    hr = m_device.As(&dxgiDevice);
    if (SUCCEEDED(hr)) {
        dxgiDevice->GetAdapter(m_adapter.GetAddressOf());
    }

    m_initialized = true;

    LOG_INFO("D3D11 context initialized successfully");
    LOG_INFO("  Adapter: {}", GetAdapterName());
    LOG_INFO("  Feature Level: 0x{:X}", static_cast<int>(m_featureLevel));
    LOG_INFO("  Video Memory: {} MB", GetVideoMemorySize() / (1024 * 1024));

    return true;
}

void D3D11Context::Shutdown() {
    if (!m_initialized) {
        return;
    }

    LOG_INFO("Shutting down D3D11 context");

    // ステージングプールをクリア
    ClearStagingPool();

    // NV12プールをクリア
    ClearNV12Pool();

    // NV12変換リソース解放
    m_inputLayout.Reset();
    m_vertexBuffer.Reset();
    m_sampler.Reset();
    m_pixelShader.Reset();
    m_vertexShader.Reset();
    m_rasterizerState.Reset();
    m_blendState.Reset();

    // デバイス解放
    if (m_context) {
        m_context->ClearState();
        m_context->Flush();
    }
    m_context.Reset();
    m_adapter.Reset();
    m_device.Reset();

    m_initialized = false;
    m_nv12ConverterInitialized = false;
}

bool D3D11Context::IsInitialized() const {
    return m_initialized;
}

// =============================================================================
// デバイス取得
// =============================================================================

ID3D11Device* D3D11Context::GetDevice() const {
    return m_device.Get();
}

ID3D11DeviceContext* D3D11Context::GetDeviceContext() const {
    return m_context.Get();
}

IDXGIAdapter* D3D11Context::GetAdapter() const {
    return m_adapter.Get();
}

// =============================================================================
// テクスチャ作成
// =============================================================================

ComPtr<ID3D11Texture2D> D3D11Context::CreateTexture2D(
    UINT width,
    UINT height,
    DXGI_FORMAT format,
    UINT bindFlags,
    D3D11_USAGE usage,
    UINT cpuAccessFlags
) {
    if (!m_initialized) {
        LOG_ERROR("D3D11Context not initialized");
        return nullptr;
    }

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.Usage = usage;
    desc.BindFlags = bindFlags;
    desc.CPUAccessFlags = cpuAccessFlags;
    desc.MiscFlags = 0;

    ComPtr<ID3D11Texture2D> texture;
    HRESULT hr = m_device->CreateTexture2D(&desc, nullptr, texture.GetAddressOf());
    
    if (FAILED(hr)) {
        LOG_ERROR("Failed to create texture2D ({}x{}, format={}): {}", 
                  width, height, static_cast<int>(format), HResultToString(hr));
        return nullptr;
    }

    return texture;
}

ComPtr<ID3D11Texture2D> D3D11Context::CreateStagingTexture(
    UINT width,
    UINT height,
    DXGI_FORMAT format
) {
    return CreateTexture2D(
        width, height, format,
        0,                                          // バインドフラグなし
        D3D11_USAGE_STAGING,                        // ステージング
        D3D11_CPU_ACCESS_READ | D3D11_CPU_ACCESS_WRITE
    );
}

ComPtr<ID3D11Texture2D> D3D11Context::CreateRenderTargetTexture(
    UINT width,
    UINT height,
    DXGI_FORMAT format
) {
    return CreateTexture2D(
        width, height, format,
        D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE,
        D3D11_USAGE_DEFAULT,
        0
    );
}

// =============================================================================
// NV12→RGBA変換
// =============================================================================

bool D3D11Context::InitializeNV12Converter() {
    if (m_nv12ConverterInitialized) {
        return true;
    }

    if (!m_initialized) {
        LOG_ERROR("D3D11Context must be initialized first");
        return false;
    }

    LOG_DEBUG("Initializing NV12 to RGBA converter");

    if (!CreateShaderResources()) {
        LOG_ERROR("Failed to create shader resources");
        return false;
    }

    if (!CreateFullscreenQuad()) {
        LOG_ERROR("Failed to create fullscreen quad");
        return false;
    }

    m_nv12ConverterInitialized = true;
    LOG_INFO("NV12 to RGBA converter initialized");
    return true;
}

bool D3D11Context::CreateShaderResources() {
    HRESULT hr;

    // 頂点シェーダーをランタイムコンパイル
    ComPtr<ID3DBlob> vsBlob;
    ComPtr<ID3DBlob> errorBlob;
    
    hr = D3DCompile(
        g_PassthroughVS_Source,
        strlen(g_PassthroughVS_Source),
        "Passthrough_VS",
        nullptr,
        nullptr,
        "main",
        "vs_5_0",
        D3DCOMPILE_OPTIMIZATION_LEVEL3,
        0,
        vsBlob.GetAddressOf(),
        errorBlob.GetAddressOf()
    );
    
    if (FAILED(hr)) {
        if (errorBlob) {
            LOG_ERROR("Vertex shader compilation failed: {}", 
                      static_cast<const char*>(errorBlob->GetBufferPointer()));
        }
        return false;
    }

    hr = m_device->CreateVertexShader(
        vsBlob->GetBufferPointer(),
        vsBlob->GetBufferSize(),
        nullptr,
        m_vertexShader.GetAddressOf()
    );
    HR_CHECK(hr, "Failed to create vertex shader");

    // 入力レイアウト作成
    D3D11_INPUT_ELEMENT_DESC inputLayout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };

    hr = m_device->CreateInputLayout(
        inputLayout,
        2,
        vsBlob->GetBufferPointer(),
        vsBlob->GetBufferSize(),
        m_inputLayout.GetAddressOf()
    );
    HR_CHECK(hr, "Failed to create input layout");

    // ピクセルシェーダーをランタイムコンパイル
    ComPtr<ID3DBlob> psBlob;
    errorBlob.Reset();
    
    hr = D3DCompile(
        g_NV12ToRGBA_PS_Source,
        strlen(g_NV12ToRGBA_PS_Source),
        "NV12ToRGBA_PS",
        nullptr,
        nullptr,
        "main",
        "ps_5_0",
        D3DCOMPILE_OPTIMIZATION_LEVEL3,
        0,
        psBlob.GetAddressOf(),
        errorBlob.GetAddressOf()
    );
    
    if (FAILED(hr)) {
        if (errorBlob) {
            LOG_ERROR("Pixel shader compilation failed: {}", 
                      static_cast<const char*>(errorBlob->GetBufferPointer()));
        }
        return false;
    }

    hr = m_device->CreatePixelShader(
        psBlob->GetBufferPointer(),
        psBlob->GetBufferSize(),
        nullptr,
        m_pixelShader.GetAddressOf()
    );
    HR_CHECK(hr, "Failed to create pixel shader");

    // サンプラーステート作成
    D3D11_SAMPLER_DESC samplerDesc = {};
    samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    samplerDesc.MinLOD = 0;
    samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;

    hr = m_device->CreateSamplerState(&samplerDesc, m_sampler.GetAddressOf());
    HR_CHECK(hr, "Failed to create sampler state");

    // ラスタライザステート作成
    D3D11_RASTERIZER_DESC rasterizerDesc = {};
    rasterizerDesc.FillMode = D3D11_FILL_SOLID;
    rasterizerDesc.CullMode = D3D11_CULL_NONE;
    rasterizerDesc.FrontCounterClockwise = FALSE;
    rasterizerDesc.DepthClipEnable = TRUE;

    hr = m_device->CreateRasterizerState(&rasterizerDesc, m_rasterizerState.GetAddressOf());
    HR_CHECK(hr, "Failed to create rasterizer state");

    // ブレンドステート作成（無効）
    D3D11_BLEND_DESC blendDesc = {};
    blendDesc.RenderTarget[0].BlendEnable = FALSE;
    blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;

    hr = m_device->CreateBlendState(&blendDesc, m_blendState.GetAddressOf());
    HR_CHECK(hr, "Failed to create blend state");

    return true;
}

bool D3D11Context::CreateFullscreenQuad() {
    D3D11_BUFFER_DESC bufferDesc = {};
    bufferDesc.Usage = D3D11_USAGE_IMMUTABLE;
    bufferDesc.ByteWidth = sizeof(g_FullscreenQuadVertices);
    bufferDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;

    D3D11_SUBRESOURCE_DATA initData = {};
    initData.pSysMem = g_FullscreenQuadVertices;

    HRESULT hr = m_device->CreateBuffer(&bufferDesc, &initData, m_vertexBuffer.GetAddressOf());
    HR_CHECK(hr, "Failed to create vertex buffer");

    return true;
}

bool D3D11Context::ConvertNV12ToRGBA(
    ID3D11Texture2D* srcNV12,
    UINT srcIndex,
    ID3D11Texture2D* dstRGBA
) {
    if (!m_nv12ConverterInitialized) {
        if (!InitializeNV12Converter()) {
            return false;
        }
    }

    if (!srcNV12 || !dstRGBA) {
        LOG_ERROR("Invalid texture pointers");
        return false;
    }

    // ソーステクスチャの情報取得
    D3D11_TEXTURE2D_DESC srcDesc;
    srcNV12->GetDesc(&srcDesc);

    D3D11_TEXTURE2D_DESC dstDesc;
    dstRGBA->GetDesc(&dstDesc);

    // ソーステクスチャのバインドフラグを確認し、SRV作成可能か判定
    ComPtr<ID3D11Texture2D> srvSourceTexture;
    UINT srvSourceIndex = 0;
    ComPtr<ID3D11Texture2D> intermediateTexture;  // スコープ保持用
    bool useTextureArray = false;

    if (srcDesc.BindFlags & D3D11_BIND_SHADER_RESOURCE) {
        // 直接SRV作成可能
        srvSourceTexture = srcNV12;
        srvSourceIndex = srcIndex;
        useTextureArray = (srcDesc.ArraySize > 1);
    } else {
        // D3D11VAデコーダーテクスチャはSRV作成不可 → 中間テクスチャ経由
        intermediateTexture = AcquireNV12SRVTexture(srcDesc.Width, srcDesc.Height);
        if (!intermediateTexture) {
            LOG_ERROR("Failed to acquire intermediate NV12 texture for SRV");
            return false;
        }

        // GPU内コピー（CopySubresourceRegion）
        UINT srcSubresource = D3D11CalcSubresource(0, srcIndex, 1);
        UINT dstSubresource = D3D11CalcSubresource(0, 0, 1);
        m_context->CopySubresourceRegion(
            intermediateTexture.Get(), dstSubresource, 0, 0, 0,
            srcNV12, srcSubresource, nullptr
        );

        srvSourceTexture = intermediateTexture;
        srvSourceIndex = 0;
        useTextureArray = false;
    }

    // シェーダーリソースビュー作成（Y平面）
    D3D11_SHADER_RESOURCE_VIEW_DESC srvDescY = {};
    srvDescY.Format = DXGI_FORMAT_R8_UNORM;
    if (useTextureArray) {
        srvDescY.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
        srvDescY.Texture2DArray.MostDetailedMip = 0;
        srvDescY.Texture2DArray.MipLevels = 1;
        srvDescY.Texture2DArray.FirstArraySlice = srvSourceIndex;
        srvDescY.Texture2DArray.ArraySize = 1;
    } else {
        srvDescY.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srvDescY.Texture2D.MostDetailedMip = 0;
        srvDescY.Texture2D.MipLevels = 1;
    }

    ComPtr<ID3D11ShaderResourceView> srvY;
    HRESULT hr = m_device->CreateShaderResourceView(srvSourceTexture.Get(), &srvDescY, srvY.GetAddressOf());
    if (FAILED(hr)) {
        LOG_ERROR("Failed to create Y plane SRV: {}", HResultToString(hr));
        if (intermediateTexture) {
            ReleaseNV12SRVTexture(intermediateTexture.Get());
        }
        return false;
    }

    // シェーダーリソースビュー作成（UV平面）
    D3D11_SHADER_RESOURCE_VIEW_DESC srvDescUV = {};
    srvDescUV.Format = DXGI_FORMAT_R8G8_UNORM;
    if (useTextureArray) {
        srvDescUV.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
        srvDescUV.Texture2DArray.MostDetailedMip = 0;
        srvDescUV.Texture2DArray.MipLevels = 1;
        srvDescUV.Texture2DArray.FirstArraySlice = srvSourceIndex;
        srvDescUV.Texture2DArray.ArraySize = 1;
    } else {
        srvDescUV.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srvDescUV.Texture2D.MostDetailedMip = 0;
        srvDescUV.Texture2D.MipLevels = 1;
    }

    ComPtr<ID3D11ShaderResourceView> srvUV;
    hr = m_device->CreateShaderResourceView(srvSourceTexture.Get(), &srvDescUV, srvUV.GetAddressOf());
    if (FAILED(hr)) {
        LOG_ERROR("Failed to create UV plane SRV: {}", HResultToString(hr));
        if (intermediateTexture) {
            ReleaseNV12SRVTexture(intermediateTexture.Get());
        }
        return false;
    }

    // レンダーターゲットビュー作成
    ComPtr<ID3D11RenderTargetView> rtv;
    hr = m_device->CreateRenderTargetView(dstRGBA, nullptr, rtv.GetAddressOf());
    if (FAILED(hr)) {
        LOG_ERROR("Failed to create RTV: {}", HResultToString(hr));
        if (intermediateTexture) {
            ReleaseNV12SRVTexture(intermediateTexture.Get());
        }
        return false;
    }

    // ビューポート設定
    D3D11_VIEWPORT viewport = {};
    viewport.Width = static_cast<float>(dstDesc.Width);
    viewport.Height = static_cast<float>(dstDesc.Height);
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 1.0f;

    // パイプライン設定
    m_context->IASetInputLayout(m_inputLayout.Get());
    m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);

    UINT stride = sizeof(Vertex);
    UINT offset = 0;
    m_context->IASetVertexBuffers(0, 1, m_vertexBuffer.GetAddressOf(), &stride, &offset);

    m_context->VSSetShader(m_vertexShader.Get(), nullptr, 0);
    m_context->PSSetShader(m_pixelShader.Get(), nullptr, 0);

    ID3D11ShaderResourceView* srvs[] = { srvY.Get(), srvUV.Get() };
    m_context->PSSetShaderResources(0, 2, srvs);
    m_context->PSSetSamplers(0, 1, m_sampler.GetAddressOf());

    m_context->RSSetState(m_rasterizerState.Get());
    m_context->RSSetViewports(1, &viewport);

    float blendFactor[4] = { 0, 0, 0, 0 };
    m_context->OMSetBlendState(m_blendState.Get(), blendFactor, 0xFFFFFFFF);
    m_context->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);

    // 描画
    m_context->Draw(4, 0);

    // シェーダーリソースをアンバインド
    ID3D11ShaderResourceView* nullSrvs[] = { nullptr, nullptr };
    m_context->PSSetShaderResources(0, 2, nullSrvs);
    ID3D11RenderTargetView* nullRtv = nullptr;
    m_context->OMSetRenderTargets(1, &nullRtv, nullptr);

    // 中間テクスチャ使用時は返却
    if (intermediateTexture) {
        ReleaseNV12SRVTexture(intermediateTexture.Get());
    }

    return true;
}

bool D3D11Context::UpdateTexture(
    ID3D11Texture2D* texture,
    const void* data,
    UINT rowPitch
) {
    if (!texture || !data) {
        return false;
    }

    D3D11_TEXTURE2D_DESC desc;
    texture->GetDesc(&desc);

    if (desc.Usage == D3D11_USAGE_DYNAMIC) {
        // Mapを使用
        D3D11_MAPPED_SUBRESOURCE mapped;
        HRESULT hr = m_context->Map(texture, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        if (FAILED(hr)) {
            LOG_ERROR("Failed to map texture: {}", HResultToString(hr));
            return false;
        }

        // 行ごとにコピー
        const uint8_t* srcRow = static_cast<const uint8_t*>(data);
        uint8_t* dstRow = static_cast<uint8_t*>(mapped.pData);
        UINT copyPitch = std::min(rowPitch, mapped.RowPitch);

        for (UINT y = 0; y < desc.Height; ++y) {
            memcpy(dstRow, srcRow, copyPitch);
            srcRow += rowPitch;
            dstRow += mapped.RowPitch;
        }

        m_context->Unmap(texture, 0);
    } else {
        // UpdateSubresourceを使用
        m_context->UpdateSubresource(texture, 0, nullptr, data, rowPitch, 0);
    }

    return true;
}

void D3D11Context::CopyTexture(ID3D11Texture2D* dst, ID3D11Texture2D* src) {
    if (dst && src) {
        m_context->CopyResource(dst, src);
    }
}

bool D3D11Context::ReadbackTexture(ID3D11Texture2D* texture, void* outData, size_t bufferSize) {
    if (!texture || !outData || bufferSize == 0) {
        return false;
    }

    D3D11_TEXTURE2D_DESC srcDesc;
    texture->GetDesc(&srcDesc);

    size_t requiredSize = static_cast<size_t>(srcDesc.Width) * srcDesc.Height * 4;
    if (bufferSize < requiredSize) {
        LOG_ERROR("Buffer too small for texture readback: {} < {}", bufferSize, requiredSize);
        return false;
    }

    // ステージングテクスチャをプールから取得
    auto stagingTexture = AcquireStagingTexture(srcDesc.Width, srcDesc.Height, srcDesc.Format);
    if (!stagingTexture) {
        LOG_ERROR("Failed to acquire staging texture for readback");
        return false;
    }

    // GPUテクスチャからステージングテクスチャにコピー
    m_context->CopyResource(stagingTexture.Get(), texture);

    // ステージングテクスチャをマップ
    D3D11_MAPPED_SUBRESOURCE mapped;
    HRESULT hr = m_context->Map(stagingTexture.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) {
        LOG_ERROR("Failed to map staging texture: {}", HResultToString(hr));
        ReleaseStagingTexture(stagingTexture.Get());
        return false;
    }

    // ピクセルデータをコピー
    uint8_t* dstRow = static_cast<uint8_t*>(outData);
    const uint8_t* srcRow = static_cast<const uint8_t*>(mapped.pData);
    UINT copyPitch = srcDesc.Width * 4;

    for (UINT y = 0; y < srcDesc.Height; ++y) {
        memcpy(dstRow, srcRow, copyPitch);
        dstRow += copyPitch;
        srcRow += mapped.RowPitch;
    }

    m_context->Unmap(stagingTexture.Get(), 0);

    // ステージングテクスチャをプールに返却
    ReleaseStagingTexture(stagingTexture.Get());

    return true;
}

// =============================================================================
// 非同期リードバック
// =============================================================================

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
    
    // GPU完了クエリを発行
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

bool D3D11Context::IsReadbackComplete(const AsyncReadbackHandle& handle) {
    if (!handle.valid || !handle.query || !m_context) return false;
    
    BOOL queryData = FALSE;
    HRESULT hr = m_context->GetData(handle.query.Get(), &queryData, sizeof(BOOL), D3D11_ASYNC_GETDATA_DONOTFLUSH);
    return (hr == S_OK && queryData == TRUE);
}

bool D3D11Context::CompleteAsyncReadback(AsyncReadbackHandle& handle, void* outData, size_t bufferSize) {
    if (!handle.valid || !handle.stagingTexture || !m_context) return false;
    
    // バッファサイズチェック
    size_t requiredSize = static_cast<size_t>(handle.width) * handle.height * 4;
    if (bufferSize < requiredSize) {
        LOG_ERROR("Buffer too small for async readback: {} < {}", bufferSize, requiredSize);
        return false;
    }
    
    // Map/Unmap でデータ取得
    D3D11_MAPPED_SUBRESOURCE mapped;
    HRESULT hr = m_context->Map(handle.stagingTexture.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) return false;
    
    // ピクセルデータコピー
    uint8_t* dstRow = static_cast<uint8_t*>(outData);
    const uint8_t* srcRow = static_cast<const uint8_t*>(mapped.pData);
    UINT copyPitch = handle.width * 4; // BGRA
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

// =============================================================================
// ステージングテクスチャプール
// =============================================================================

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

void D3D11Context::ClearStagingPool() {
    std::lock_guard<std::mutex> lock(m_stagingPoolMutex);
    m_stagingPool.clear();
}

// =============================================================================
// NV12 SRVテクスチャプール（D3D11VAデコーダー用中間テクスチャ）
// =============================================================================

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
    if (FAILED(hr)) {
        LOG_ERROR("Failed to create NV12 SRV texture: {:08X}", static_cast<unsigned int>(hr));
        return nullptr;
    }

    if (m_nv12Pool.size() < kMaxNV12PoolSize) {
        m_nv12Pool.push_back({newTexture, width, height, true});
    }
    return newTexture;
}

void D3D11Context::ReleaseNV12SRVTexture(ID3D11Texture2D* texture) {
    if (!texture) return;
    std::lock_guard<std::mutex> lock(m_nv12PoolMutex);

    for (auto& entry : m_nv12Pool) {
        if (entry.texture.Get() == texture) {
            entry.inUse = false;
            return;
        }
    }
}

void D3D11Context::ClearNV12Pool() {
    std::lock_guard<std::mutex> lock(m_nv12PoolMutex);
    m_nv12Pool.clear();
}

void D3D11Context::Flush() {
    if (m_context) {
        m_context->Flush();
    }
}

// =============================================================================
// デバイス情報
// =============================================================================

std::string D3D11Context::GetAdapterName() const {
    if (!m_adapter) {
        return "Unknown";
    }

    DXGI_ADAPTER_DESC desc;
    if (SUCCEEDED(m_adapter->GetDesc(&desc))) {
        // ワイド文字からマルチバイト文字へ変換
        int size = WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, nullptr, 0, nullptr, nullptr);
        std::string name(size - 1, 0);
        WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name.data(), size, nullptr, nullptr);
        return name;
    }

    return "Unknown";
}

size_t D3D11Context::GetVideoMemorySize() const {
    if (!m_adapter) {
        return 0;
    }

    DXGI_ADAPTER_DESC desc;
    if (SUCCEEDED(m_adapter->GetDesc(&desc))) {
        return desc.DedicatedVideoMemory;
    }

    return 0;
}

D3D_FEATURE_LEVEL D3D11Context::GetFeatureLevel() const {
    return m_featureLevel;
}

} // namespace ytdlpspout
