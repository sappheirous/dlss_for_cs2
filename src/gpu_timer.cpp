#include "gpu_timer.hpp"

#include <algorithm>

namespace {
std::int64_t steady_milliseconds() noexcept {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
}  // namespace

GpuTimer::Measurement::Measurement(GpuTimer& timer, ID3D11DeviceContext* context)
    : timer_(timer), context_(context), recording_(timer.begin(context)) {}

GpuTimer::Measurement::~Measurement() {
    if (recording_) timer_.end(context_);
}

GpuTimer::Measurement GpuTimer::measure(ID3D11DeviceContext* context) { return Measurement(*this, context); }

std::optional<float> GpuTimer::milliseconds() const noexcept {
    const auto sampled = last_sample_ms_.load();
    const auto value = milliseconds_.load();
    if (!sampled || value < 0.0f || steady_milliseconds() - sampled > 1000) return std::nullopt;

    return value;
}

void GpuTimer::reset() {
    queries_ = {};
    recording_ = nullptr;
    unavailable_ = false;
    last_started_ = {};
    milliseconds_ = -1.0f;
    last_sample_ms_ = 0;
}

bool GpuTimer::begin(ID3D11DeviceContext* context) {
    if (unavailable_ || recording_) return false;
    collect(context);

    const auto now = std::chrono::steady_clock::now();
    if (now - last_started_ < std::chrono::milliseconds(100)) return false;

    const auto free = std::ranges::find(queries_, false, &Queries::pending);
    if (free == queries_.end()) return false;

    if (!free->disjoint) {
        Microsoft::WRL::ComPtr<ID3D11Device> device;
        context->GetDevice(&device);
        const D3D11_QUERY_DESC disjoint{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
        const D3D11_QUERY_DESC timestamp{D3D11_QUERY_TIMESTAMP, 0};
        if (FAILED(device->CreateQuery(&disjoint, &free->disjoint)) ||
            FAILED(device->CreateQuery(&timestamp, &free->start)) ||
            FAILED(device->CreateQuery(&timestamp, &free->end))) {
            unavailable_ = true;
            return false;
        }
    }

    recording_ = &*free;
    last_started_ = now;
    context->Begin(recording_->disjoint.Get());
    context->End(recording_->start.Get());
    return true;
}

void GpuTimer::end(ID3D11DeviceContext* context) {
    context->End(recording_->end.Get());
    context->End(recording_->disjoint.Get());
    recording_->pending = true;
    recording_ = nullptr;
}

void GpuTimer::collect(ID3D11DeviceContext* context) {
    for (auto& query : queries_) {
        if (!query.pending) continue;

        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT clock{};
        UINT64 start = 0, end = 0;
        const auto clock_result =
            context->GetData(query.disjoint.Get(), &clock, sizeof(clock), D3D11_ASYNC_GETDATA_DONOTFLUSH);
        if (clock_result == S_FALSE) continue;
        if (FAILED(clock_result)) {
            query.pending = false;
            continue;
        }

        const auto start_result =
            context->GetData(query.start.Get(), &start, sizeof(start), D3D11_ASYNC_GETDATA_DONOTFLUSH);
        const auto end_result = context->GetData(query.end.Get(), &end, sizeof(end), D3D11_ASYNC_GETDATA_DONOTFLUSH);
        if (start_result == S_FALSE || end_result == S_FALSE) continue;

        query.pending = false;
        if (FAILED(start_result) || FAILED(end_result) || clock.Disjoint || !clock.Frequency || end < start) continue;

        milliseconds_ =
            static_cast<float>(static_cast<double>(end - start) * 1000.0 / static_cast<double>(clock.Frequency));
        last_sample_ms_ = steady_milliseconds();
    }
}
