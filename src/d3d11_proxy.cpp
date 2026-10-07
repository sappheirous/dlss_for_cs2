#include "d3d11_proxy.hpp"

#include <algorithm>
#include <array>

#include <Windows.h>
#include <d3d11.h>

namespace {
// rendersystemdx11.dll imports D3D11CreateDevice from d3d11.dll at RVA 0x1EA528.
// Verified for SHA-256 321af39487adad1b0f0cfc7e0ad519c5b07937a40e16593fdfd12e1f67da8a56.
using D3d11CreateDeviceFunction = decltype(&::D3D11CreateDevice);
using D3d11CreateDeviceAndSwapChainFunction = decltype(&::D3D11CreateDeviceAndSwapChain);

struct D3d11ProxyState {
    INIT_ONCE initialization = INIT_ONCE_STATIC_INIT;
    HMODULE system_module = nullptr;
    D3d11CreateDeviceFunction create_device = nullptr;
    D3d11CreateDeviceAndSwapChainFunction create_device_and_swap_chain = nullptr;
};

D3d11ProxyState proxy_state;

BOOL CALLBACK initialize_system_d3d11(PINIT_ONCE, PVOID context, PVOID*) noexcept {
    auto& state = *static_cast<D3d11ProxyState*>(context);
    std::array<wchar_t, MAX_PATH> path{};
    constexpr wchar_t suffix[] = L"\\d3d11.dll";
    constexpr auto suffix_size = sizeof(suffix) / sizeof(suffix[0]);

    const auto system_directory_size = GetSystemDirectoryW(path.data(), static_cast<UINT>(path.size()));
    if (!system_directory_size || system_directory_size + suffix_size > path.size()) return FALSE;

    std::copy_n(suffix, suffix_size, path.data() + system_directory_size);
    state.system_module = LoadLibraryW(path.data());
    if (!state.system_module) return FALSE;

    state.create_device =
        reinterpret_cast<D3d11CreateDeviceFunction>(GetProcAddress(state.system_module, "D3D11CreateDevice"));
    state.create_device_and_swap_chain = reinterpret_cast<D3d11CreateDeviceAndSwapChainFunction>(
        GetProcAddress(state.system_module, "D3D11CreateDeviceAndSwapChain"));
    if (state.create_device && state.create_device_and_swap_chain) return TRUE;

    FreeLibrary(state.system_module);
    state.system_module = nullptr;
    state.create_device = nullptr;
    state.create_device_and_swap_chain = nullptr;
    return FALSE;
}

bool system_d3d11_ready() noexcept {
    return InitOnceExecuteOnce(&proxy_state.initialization, initialize_system_d3d11, &proxy_state, nullptr) &&
           proxy_state.create_device && proxy_state.create_device_and_swap_chain;
}
}  // namespace

extern "C" HRESULT WINAPI proxy_d3d11_create_device(IDXGIAdapter* adapter, D3D_DRIVER_TYPE driver_type,
                                                    HMODULE software, UINT flags,
                                                    const D3D_FEATURE_LEVEL* feature_levels, UINT feature_levels_count,
                                                    UINT sdk_version, ID3D11Device** device,
                                                    D3D_FEATURE_LEVEL* feature_level,
                                                    ID3D11DeviceContext** immediate_context) {
    if (!system_d3d11_ready()) return E_FAIL;

    return proxy_state.create_device(adapter, driver_type, software, flags, feature_levels, feature_levels_count,
                                     sdk_version, device, feature_level, immediate_context);
}

HRESULT create_d3d11_device_and_swap_chain(IDXGIAdapter* adapter, D3D_DRIVER_TYPE driver_type, HMODULE software,
                                           UINT flags, const D3D_FEATURE_LEVEL* feature_levels,
                                           UINT feature_levels_count, UINT sdk_version,
                                           const DXGI_SWAP_CHAIN_DESC* swap_chain_desc, IDXGISwapChain** swap_chain,
                                           ID3D11Device** device, D3D_FEATURE_LEVEL* feature_level,
                                           ID3D11DeviceContext** immediate_context) noexcept {
    if (!system_d3d11_ready()) return E_FAIL;

    return proxy_state.create_device_and_swap_chain(adapter, driver_type, software, flags, feature_levels,
                                                    feature_levels_count, sdk_version, swap_chain_desc, swap_chain,
                                                    device, feature_level, immediate_context);
}

extern "C" HRESULT WINAPI proxy_d3d11_create_device_and_swap_chain(
    IDXGIAdapter* adapter, D3D_DRIVER_TYPE driver_type, HMODULE software, UINT flags,
    const D3D_FEATURE_LEVEL* feature_levels, UINT feature_levels_count, UINT sdk_version,
    const DXGI_SWAP_CHAIN_DESC* swap_chain_desc, IDXGISwapChain** swap_chain, ID3D11Device** device,
    D3D_FEATURE_LEVEL* feature_level, ID3D11DeviceContext** immediate_context) {
    return create_d3d11_device_and_swap_chain(adapter, driver_type, software, flags, feature_levels,
                                              feature_levels_count, sdk_version, swap_chain_desc, swap_chain, device,
                                              feature_level, immediate_context);
}
