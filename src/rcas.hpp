#pragma once

#include <d3d11.h>
#include <wrl/client.h>

#include "game_types.hpp"

class Rcas {
public:
    bool render(ID3D11DeviceContext* context, ID3D11Texture2D* source, ID3D11ShaderResourceView* source_view,
                UINT source_mip, ID3D11Texture2D* output, UINT output_mip, const RenderViewport& destination,
                DXGI_FORMAT format, float sharpness);

private:
    bool prepare(ID3D11Device* device, UINT width, UINT height, DXGI_FORMAT format, bool needs_snapshot);

    Microsoft::WRL::ComPtr<ID3D11ComputeShader> shader_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> constants_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> snapshot_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> snapshot_view_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> result_;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> result_view_;
    UINT width_ = 0;
    UINT height_ = 0;
    DXGI_FORMAT format_ = DXGI_FORMAT_UNKNOWN;
};
