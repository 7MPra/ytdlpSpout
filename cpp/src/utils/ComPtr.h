// =============================================================================
// ComPtr.h - COMスマートポインタ
// =============================================================================
//
// 機能:
//   - Microsoft WRL ComPtr のラッパー/エイリアス
//   - DirectX COM オブジェクトの自動解放
//   - 参照カウント管理
//
// 使用例:
//   ComPtr<ID3D11Device> device;
//   D3D11CreateDevice(..., device.GetAddressOf());
//   device->CreateTexture2D(...);
//   // スコープ終了時に自動でRelease()
//
// =============================================================================

#pragma once

// Windows Runtime Library の ComPtr を使用
#include <wrl/client.h>

namespace ytdlpspout {

/// @brief COMオブジェクト用スマートポインタ
/// @tparam T COMインターフェース型
template<typename T>
using ComPtr = Microsoft::WRL::ComPtr<T>;

// =============================================================================
// ComPtr ヘルパー関数
// =============================================================================

/// @brief ComPtrの安全なリセット
/// @tparam T COMインターフェース型
/// @param ptr リセットするComPtr
template<typename T>
inline void SafeReset(ComPtr<T>& ptr) {
    ptr.Reset();
}

/// @brief 2つのComPtrのスワップ
/// @tparam T COMインターフェース型
template<typename T>
inline void Swap(ComPtr<T>& a, ComPtr<T>& b) {
    a.Swap(b);
}

/// @brief ComPtr が有効かどうかをチェック
/// @tparam T COMインターフェース型
/// @param ptr チェックするComPtr
/// @return 有効な場合true
template<typename T>
inline bool IsValid(const ComPtr<T>& ptr) {
    return ptr.Get() != nullptr;
}

/// @brief IUnknownからの安全なキャスト
/// @tparam T 変換先インターフェース型
/// @param unknown 元のIUnknownポインタ
/// @param result 結果を格納するComPtr
/// @return 成功した場合S_OK
template<typename T>
inline HRESULT SafeQueryInterface(IUnknown* unknown, ComPtr<T>& result) {
    if (!unknown) {
        return E_INVALIDARG;
    }
    return unknown->QueryInterface(__uuidof(T), reinterpret_cast<void**>(result.ReleaseAndGetAddressOf()));
}

} // namespace ytdlpspout

// =============================================================================
// 使用例:
// =============================================================================
// 
// #include "ComPtr.h"
// using namespace ytdlpspout;
//
// void Example() {
//     ComPtr<ID3D11Device> device;
//     ComPtr<ID3D11DeviceContext> context;
//     
//     // デバイス作成
//     HRESULT hr = D3D11CreateDevice(
//         nullptr,
//         D3D_DRIVER_TYPE_HARDWARE,
//         nullptr,
//         0,
//         nullptr,
//         0,
//         D3D11_SDK_VERSION,
//         device.GetAddressOf(),
//         nullptr,
//         context.GetAddressOf()
//     );
//     
//     if (SUCCEEDED(hr)) {
//         // device と context は自動的にスコープ終了時に解放される
//     }
// }
// 
// =============================================================================
