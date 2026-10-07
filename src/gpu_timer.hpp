#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <optional>

#include <d3d11.h>
#include <wrl/client.h>

class GpuTimer {
public:
    class Measurement {
    public:
        Measurement(GpuTimer& timer, ID3D11DeviceContext* context);
        ~Measurement();

        Measurement(const Measurement&) = delete;
        Measurement& operator=(const Measurement&) = delete;
        Measurement(Measurement&&) = delete;
        Measurement& operator=(Measurement&&) = delete;

    private:
        GpuTimer& timer_;
        ID3D11DeviceContext* context_;
        bool recording_;
    };

    Measurement measure(ID3D11DeviceContext* context);
    std::optional<float> milliseconds() const noexcept;
    void reset();

private:
    struct Queries {
        Microsoft::WRL::ComPtr<ID3D11Query> disjoint;
        Microsoft::WRL::ComPtr<ID3D11Query> start;
        Microsoft::WRL::ComPtr<ID3D11Query> end;
        bool pending = false;
    };

    bool begin(ID3D11DeviceContext* context);
    void end(ID3D11DeviceContext* context);
    void collect(ID3D11DeviceContext* context);

    std::array<Queries, 3> queries_;
    Queries* recording_ = nullptr;
    bool unavailable_ = false;
    std::chrono::steady_clock::time_point last_started_{};
    std::atomic<float> milliseconds_{-1.0f};
    std::atomic<std::int64_t> last_sample_ms_{0};
};
