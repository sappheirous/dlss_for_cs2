#pragma once

#include <d3d11_1.h>
#include <wrl/client.h>

// D3D11.1 swaps the complete pipeline state, including bindings touched by NGX.
class ContextStateGuard {
public:
    explicit ContextStateGuard(ID3D11DeviceContext* original) {
        struct Cache {
            Microsoft::WRL::ComPtr<ID3D11DeviceContext1> context;
            Microsoft::WRL::ComPtr<ID3D11Device1> device;
            Microsoft::WRL::ComPtr<ID3DDeviceContextState> state;
        };
        static thread_local Cache cache;
        if (cache.context.Get() != original) {
            cache = {};
            if (original->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE ||
                FAILED(original->QueryInterface(IID_PPV_ARGS(&cache.context))))
                return;

            Microsoft::WRL::ComPtr<ID3D11Device> base;
            original->GetDevice(&base);
            if (FAILED(base.As(&cache.device))) return;
        }

        if (!cache.state) {
            if (!cache.device) return;

            const D3D_FEATURE_LEVEL levels[] = {cache.device->GetFeatureLevel()};
            D3D_FEATURE_LEVEL chosen{};
            const auto flags = cache.device->GetCreationFlags() & D3D11_CREATE_DEVICE_SINGLETHREADED
                                   ? D3D11_1_CREATE_DEVICE_CONTEXT_STATE_SINGLETHREADED
                                   : 0;
            if (FAILED(cache.device->CreateDeviceContextState(flags, levels, 1, D3D11_SDK_VERSION,
                                                              __uuidof(ID3D11Device), &chosen, &cache.state)))
                return;
        }

        context_ = cache.context;
        context_->SwapDeviceContextState(cache.state.Get(), &saved_);
    }

    ~ContextStateGuard() {
        if (saved_) {
            context_->ClearState();
            context_->SwapDeviceContextState(saved_.Get(), nullptr);
        }
    }

    ContextStateGuard(const ContextStateGuard&) = delete;
    ContextStateGuard& operator=(const ContextStateGuard&) = delete;
    ContextStateGuard(ContextStateGuard&&) = delete;
    ContextStateGuard& operator=(ContextStateGuard&&) = delete;

    bool ready() const noexcept { return saved_ != nullptr; }

private:
    Microsoft::WRL::ComPtr<ID3D11DeviceContext1> context_;
    Microsoft::WRL::ComPtr<ID3DDeviceContextState> saved_;
};
