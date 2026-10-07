#pragma once

#include <d3d11.h>
#include <wrl/client.h>

#include "game_types.hpp"

class UpscaleFallback {
public:
    bool initialize(ID3D11Device* device);
    bool render(ID3D11DeviceContext* context, ID3D11ShaderResourceView* color, ID3D11ShaderResourceView* output,
                const RenderViewport& source, const RenderViewport& destination);

private:
    Microsoft::WRL::ComPtr<ID3D11VertexShader> vertex_shader_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> pixel_shader_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler_;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> rasterizer_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> constants_;
};
