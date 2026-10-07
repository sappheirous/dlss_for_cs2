#include "rcas.hpp"

#include <cstring>

#include "rcas.generated.h"

using Microsoft::WRL::ComPtr;

bool Rcas::prepare(ID3D11Device* device, UINT width, UINT height, DXGI_FORMAT format, bool needs_snapshot) {
    if (!shader_ && FAILED(device->CreateComputeShader(rcas_shader, sizeof(rcas_shader), nullptr, &shader_)))
        return false;

    if (!constants_) {
        D3D11_BUFFER_DESC description{};
        description.ByteWidth = 16;
        description.Usage = D3D11_USAGE_DYNAMIC;
        description.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        description.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(device->CreateBuffer(&description, nullptr, &constants_))) return false;
    }

    if (width != width_ || height != height_ || format != format_) {
        snapshot_view_.Reset();
        snapshot_.Reset();
        result_view_.Reset();
        result_.Reset();
        width_ = width;
        height_ = height;
        format_ = format;
    }

    D3D11_TEXTURE2D_DESC description{};
    description.Width = width;
    description.Height = height;
    description.MipLevels = description.ArraySize = description.SampleDesc.Count = 1;
    description.Format = format;
    description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    if (!result_ && FAILED(device->CreateTexture2D(&description, nullptr, &result_))) return false;
    if (!result_view_ && FAILED(device->CreateUnorderedAccessView(result_.Get(), nullptr, &result_view_))) return false;

    if (needs_snapshot) {
        description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (!snapshot_ && FAILED(device->CreateTexture2D(&description, nullptr, &snapshot_))) return false;
        if (!snapshot_view_ && FAILED(device->CreateShaderResourceView(snapshot_.Get(), nullptr, &snapshot_view_)))
            return false;
    }
    return true;
}

bool Rcas::render(ID3D11DeviceContext* context, ID3D11Texture2D* source, ID3D11ShaderResourceView* source_view,
                  UINT source_mip, ID3D11Texture2D* output, UINT output_mip, const RenderViewport& destination,
                  DXGI_FORMAT format, float sharpness) {
    const auto width = static_cast<UINT>(destination.width);
    const auto height = static_cast<UINT>(destination.height);
    ComPtr<ID3D11Device> device;
    context->GetDevice(&device);
    if (!prepare(device.Get(), width, height, format, !source_view)) return false;

    struct Constants {
        UINT width;
        UINT height;
        float sharpness;
        float padding;
    };
    static_assert(sizeof(Constants) == 16);
    const Constants constants{width, height, sharpness, 0};
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(constants_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) return false;
    std::memcpy(mapped.pData, &constants, sizeof(constants));
    context->Unmap(constants_.Get(), 0);

    // NGX owns its bindings; the surrounding context guard restores the game's state.
    context->ClearState();
    if (!source_view) {
        const D3D11_BOX box{
            static_cast<UINT>(destination.top_left_x),         static_cast<UINT>(destination.top_left_y),          0,
            static_cast<UINT>(destination.top_left_x) + width, static_cast<UINT>(destination.top_left_y) + height, 1};
        context->CopySubresourceRegion(snapshot_.Get(), 0, 0, 0, 0, source, source_mip, &box);
        source_view = snapshot_view_.Get();
    }

    auto buffer = constants_.Get();
    auto target = result_view_.Get();
    context->CSSetShader(shader_.Get(), nullptr, 0);
    context->CSSetConstantBuffers(0, 1, &buffer);
    context->CSSetShaderResources(0, 1, &source_view);
    context->CSSetUnorderedAccessViews(0, 1, &target, nullptr);
    context->Dispatch((width + 7) / 8, (height + 7) / 8, 1);

    ID3D11ShaderResourceView* no_source = nullptr;
    ID3D11UnorderedAccessView* no_target = nullptr;
    context->CSSetShaderResources(0, 1, &no_source);
    context->CSSetUnorderedAccessViews(0, 1, &no_target, nullptr);
    context->CopySubresourceRegion(output, output_mip, static_cast<UINT>(destination.top_left_x),
                                   static_cast<UINT>(destination.top_left_y), 0, result_.Get(), 0, nullptr);
    return true;
}
