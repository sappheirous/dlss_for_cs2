#pragma once

#include <Windows.h>
#include <d3d11.h>

HRESULT create_d3d11_device_and_swap_chain(IDXGIAdapter* adapter, D3D_DRIVER_TYPE driver_type, HMODULE software,
                                           UINT flags, const D3D_FEATURE_LEVEL* feature_levels,
                                           UINT feature_levels_count, UINT sdk_version,
                                           const DXGI_SWAP_CHAIN_DESC* swap_chain_desc, IDXGISwapChain** swap_chain,
                                           ID3D11Device** device, D3D_FEATURE_LEVEL* feature_level,
                                           ID3D11DeviceContext** immediate_context) noexcept;
