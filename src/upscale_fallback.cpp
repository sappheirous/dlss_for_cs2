#include "upscale_fallback.hpp"

#include <algorithm>
#include <array>
#include <cstring>

#include "upscale_fallback_ps.generated.h"
#include "upscale_fallback_vs.generated.h"

using Microsoft::WRL::ComPtr;

bool UpscaleFallback::initialize(ID3D11Device* device) {
    if (vertex_shader_ && pixel_shader_ && sampler_ && rasterizer_ && constants_) return true;

    D3D11_SAMPLER_DESC sampler{};
    sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sampler.MaxLOD = D3D11_FLOAT32_MAX;

    D3D11_RASTERIZER_DESC rasterizer{};
    rasterizer.FillMode = D3D11_FILL_SOLID;
    rasterizer.CullMode = D3D11_CULL_NONE;
    rasterizer.DepthClipEnable = TRUE;

    D3D11_BUFFER_DESC constants{};
    constants.ByteWidth = 32;
    constants.Usage = D3D11_USAGE_DYNAMIC;
    constants.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    constants.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    return SUCCEEDED(device->CreateVertexShader(upscale_fallback_vs_shader, sizeof(upscale_fallback_vs_shader), nullptr,
                                                vertex_shader_.ReleaseAndGetAddressOf())) &&
           SUCCEEDED(device->CreatePixelShader(upscale_fallback_ps_shader, sizeof(upscale_fallback_ps_shader), nullptr,
                                               pixel_shader_.ReleaseAndGetAddressOf())) &&
           SUCCEEDED(device->CreateSamplerState(&sampler, sampler_.ReleaseAndGetAddressOf())) &&
           SUCCEEDED(device->CreateRasterizerState(&rasterizer, rasterizer_.ReleaseAndGetAddressOf())) &&
           SUCCEEDED(device->CreateBuffer(&constants, nullptr, constants_.ReleaseAndGetAddressOf()));
}

bool UpscaleFallback::render(ID3D11DeviceContext* context, ID3D11ShaderResourceView* color,
                             ID3D11ShaderResourceView* output, const RenderViewport& source,
                             const RenderViewport& destination) {
    if (!color || !output) return false;

    D3D11_SHADER_RESOURCE_VIEW_DESC color_view{}, output_view{};
    color->GetDesc(&color_view);
    output->GetDesc(&output_view);
    if (color_view.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2D ||
        output_view.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2D)
        return false;

    ComPtr<ID3D11Resource> color_resource, output_resource;
    color->GetResource(&color_resource);
    output->GetResource(&output_resource);
    if (color_resource == output_resource) return false;

    ComPtr<ID3D11Texture2D> color_texture;
    if (FAILED(color_resource.As(&color_texture))) return false;

    D3D11_TEXTURE2D_DESC color_desc{};
    color_texture->GetDesc(&color_desc);
    const float width = static_cast<float>(std::max(1u, color_desc.Width >> color_view.Texture2D.MostDetailedMip));
    const float height = static_cast<float>(std::max(1u, color_desc.Height >> color_view.Texture2D.MostDetailedMip));

    ComPtr<ID3D11Device> device;
    context->GetDevice(&device);
    D3D11_RENDER_TARGET_VIEW_DESC target_desc{};
    target_desc.Format = output_view.Format;
    target_desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    target_desc.Texture2D.MipSlice = output_view.Texture2D.MostDetailedMip;
    ComPtr<ID3D11RenderTargetView> target;
    if (FAILED(device->CreateRenderTargetView(output_resource.Get(), &target_desc, &target))) return false;

    const std::array<float, 8> constants{source.width / width,
                                         source.height / height,
                                         source.top_left_x / width,
                                         source.top_left_y / height,
                                         (source.top_left_x + 0.5f) / width,
                                         (source.top_left_y + 0.5f) / height,
                                         (source.top_left_x + source.width - 0.5f) / width,
                                         (source.top_left_y + source.height - 0.5f) / height};
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(constants_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) return false;

    std::memcpy(mapped.pData, constants.data(), sizeof(constants));
    context->Unmap(constants_.Get(), 0);

    // NGX may leave bindings behind on failure. This is still the isolated plugin context.
    context->ClearState();
    const D3D11_VIEWPORT viewport{static_cast<float>(destination.top_left_x),
                                  static_cast<float>(destination.top_left_y),
                                  static_cast<float>(destination.width),
                                  static_cast<float>(destination.height),
                                  0.0f,
                                  1.0f};
    auto render_target = target.Get();
    auto buffer = constants_.Get();
    auto sampler = sampler_.Get();
    context->OMSetRenderTargets(1, &render_target, nullptr);
    context->RSSetState(rasterizer_.Get());
    context->RSSetViewports(1, &viewport);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(vertex_shader_.Get(), nullptr, 0);
    context->PSSetShader(pixel_shader_.Get(), nullptr, 0);
    context->PSSetConstantBuffers(0, 1, &buffer);
    context->PSSetShaderResources(0, 1, &color);
    context->PSSetSamplers(0, 1, &sampler);
    context->Draw(3, 0);
    return true;
}
