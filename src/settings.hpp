#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>

enum class DlssPreset : std::uint32_t { Default = 0, A, B, C, D, E, F, J = 10, K, L, M, Latest = 0xFFFFFFFFu };
enum class DlssQuality : int { Performance = 0, Balanced, Quality, Dlaa = 5 };

constexpr std::array dlss_presets{DlssPreset::Default, DlssPreset::A, DlssPreset::B, DlssPreset::C,
                                  DlssPreset::D,       DlssPreset::E, DlssPreset::F, DlssPreset::J,
                                  DlssPreset::K,       DlssPreset::L, DlssPreset::M, DlssPreset::Latest};
constexpr std::array preset_names{"Default", "A", "B", "C", "D", "E", "F", "J", "K", "L", "M", "Latest"};
constexpr std::array dlss_qualities{DlssQuality::Dlaa, DlssQuality::Quality, DlssQuality::Balanced,
                                    DlssQuality::Performance};
constexpr std::array quality_names{"DLAA", "Quality", "Balanced", "Performance"};

constexpr const char* preset_name(DlssPreset preset) noexcept {
    for (std::size_t index = 0; index < dlss_presets.size(); ++index)
        if (dlss_presets[index] == preset) return preset_names[index];

    return "Default";
}

constexpr const char* quality_name(DlssQuality quality) noexcept {
    for (std::size_t index = 0; index < dlss_qualities.size(); ++index)
        if (dlss_qualities[index] == quality) return quality_names[index];

    return "Quality";
}

struct Settings {
    bool enabled = false;
    DlssQuality quality = DlssQuality::Quality;
    DlssPreset preset = DlssPreset::Default;
    float sharpness = 0.0f;

    bool operator==(const Settings&) const = default;
};

struct RuntimeStatus {
    bool integration_ready = false;
    bool input_ready = false;

    bool failed = false;
    std::uint64_t failure_revision = 0;
    std::string last_error;
};

struct SettingsSnapshot {
    Settings value;
    std::uint64_t revision = 0;
};

class SettingsStore {
public:
    void load();
    void submit(const Settings& settings);
    void save_pending();

    SettingsSnapshot requested() const;
    Settings active() const;
    void set_active(const Settings& settings);

    RuntimeStatus status() const;
    bool has_failed() const;
    void set_input_ready();
    void set_integration_ready();

    void fail(std::string message, std::uint32_t result = 0);
    void clear_failure();

private:
    mutable std::mutex mutex_;
    Settings requested_;
    Settings active_;
    RuntimeStatus status_;
    std::uint64_t revision_ = 1;
    std::uint64_t saved_revision_ = 1;
};

SettingsStore& store();
std::filesystem::path data_directory();
constexpr float render_scale(const Settings& settings) noexcept {
    switch (settings.quality) {
        case DlssQuality::Dlaa:
            return 1.0f;
        case DlssQuality::Quality:
            return 2.0f / 3.0f;
        case DlssQuality::Balanced:
            return 0.58f;
        case DlssQuality::Performance:
            return 0.5f;
    }
    return 2.0f / 3.0f;
}
constexpr int game_fsr_mode(const Settings& settings) noexcept { return settings.quality == DlssQuality::Dlaa ? 0 : 1; }

constexpr DlssPreset requested_preset(const Settings& settings) noexcept {
    if (settings.preset != DlssPreset::Latest) return settings.preset;
    // Latest is a UI policy, not an NGX render-preset enum value.
    return settings.quality == DlssQuality::Performance ? DlssPreset::M : DlssPreset::K;
}
