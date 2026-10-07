#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

#include "settings.hpp"

struct ImVec4;

struct OverlayInfo {
    RuntimeStatus status;
    Settings active_settings;
    bool dlss_active = false;
    std::string_view version;
    std::string_view gpu;
    std::uint32_t output_width = 0;
    std::uint32_t output_height = 0;
    std::uint32_t input_width = 0;
    std::uint32_t input_height = 0;
};

class OverlayPanel {
public:
    void sample(double frame_ms, std::optional<float> upscale_ms);
    void draw(Settings& settings, bool& open, const OverlayInfo& info) const;
    static void apply_style(float dpi);

private:
    struct MetricSample {
        float frame_ms;
        float upscale_ms;
        float fps;
    };

    void draw_metric(const char* label, float MetricSample::* metric, const char* unit, const ImVec4& color) const;

    std::array<MetricSample, 160> history_{};
    std::size_t count_ = 0;
    std::size_t cursor_ = 0;
    std::chrono::steady_clock::time_point sampled_at_{};
};
