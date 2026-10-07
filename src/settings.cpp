#include "settings.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <SimpleIni.h>
#include <Windows.h>
#include <spdlog/spdlog.h>

namespace {
// FROM_ADDRESS keeps this lookup tied to the loaded DLL, even after it is renamed to d3d11.dll.
int data_directory_module_anchor;
}  // namespace

SettingsStore& store() {
    // The store and its settings state remain alive until the game exits.
    static auto instance = std::make_unique<SettingsStore>();
    return *instance;
}

std::filesystem::path data_directory() {
    HMODULE module = nullptr;
    constexpr DWORD flags = GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT;
    if (!GetModuleHandleExW(flags, reinterpret_cast<LPCWSTR>(&data_directory_module_anchor), &module))
        throw std::runtime_error("Cannot locate the loaded DLL");

    std::vector<wchar_t> module_path(MAX_PATH);
    constexpr std::size_t max_path_size = 32768;
    for (;;) {
        const auto path_size = GetModuleFileNameW(module, module_path.data(), static_cast<DWORD>(module_path.size()));
        if (!path_size) throw std::runtime_error("Cannot read the loaded DLL path");

        if (path_size < module_path.size()) {
            const auto directory = std::filesystem::path(std::wstring(module_path.data(), path_size)).parent_path();
            if (directory.empty()) throw std::runtime_error("Cannot locate the loaded DLL directory");
            return directory;
        }

        if (module_path.size() >= max_path_size) throw std::runtime_error("Loaded DLL path is too long");
        module_path.resize(std::min(module_path.size() * 2, max_path_size));
    }
}

void SettingsStore::load() {
    CSimpleIniA ini;
    ini.SetUnicode();
    const auto path = data_directory() / L"settings.ini";
    Settings loaded;
    if (std::filesystem::exists(path)) {
        if (ini.LoadFile(path.c_str()) < 0)
            spdlog::warn("Cannot read settings.ini; using defaults");
        else {
            loaded.enabled = ini.GetBoolValue("DLSS", "Enabled", false);
            const double sharpness = ini.GetDoubleValue("DLSS", "Sharpness", 0.0);
            loaded.sharpness = std::isfinite(sharpness) ? static_cast<float>(std::clamp(sharpness, 0.0, 1.0)) : 0.0f;
            const auto read_choice = [&ini](const char* key, auto& value, const auto& values, const auto& names) {
                if (const char* text = ini.GetValue("DLSS", key)) {
                    const auto found = std::ranges::find(names, std::string_view(text));
                    if (found != names.end())
                        value = values[static_cast<std::size_t>(found - names.begin())];
                    else
                        spdlog::warn("Invalid {} {}; using default", key, text);
                }
            };
            read_choice("QualityMode", loaded.quality, dlss_qualities, quality_names);
            read_choice("RenderPreset", loaded.preset, dlss_presets, preset_names);
        }
    }

    std::scoped_lock lock(mutex_);
    requested_ = loaded;
}

void SettingsStore::submit(const Settings& settings) {
    std::scoped_lock lock(mutex_);
    requested_ = settings;
    requested_.sharpness = std::isfinite(settings.sharpness) ? std::clamp(settings.sharpness, 0.0f, 1.0f) : 0.0f;
    if (std::ranges::find(dlss_qualities, settings.quality) == dlss_qualities.end())
        requested_.quality = DlssQuality::Quality;
    if (std::ranges::find(dlss_presets, settings.preset) == dlss_presets.end()) requested_.preset = DlssPreset::Default;
    ++revision_;
}

void SettingsStore::save_pending() {
    SettingsSnapshot snapshot;
    {
        std::scoped_lock lock(mutex_);
        if (saved_revision_ == revision_) return;
        snapshot = {requested_, revision_};
    }

    CSimpleIniA ini;
    ini.SetUnicode();
    ini.SetBoolValue("DLSS", "Enabled", snapshot.value.enabled);
    ini.SetDoubleValue("DLSS", "Sharpness", snapshot.value.sharpness);
    ini.SetValue("DLSS", "QualityMode", quality_name(snapshot.value.quality));
    ini.SetValue("DLSS", "RenderPreset", preset_name(snapshot.value.preset));

    const auto directory = data_directory();
    const auto path = directory / L"settings.ini";
    const auto temporary = directory / L"settings.pending.ini";
    if (ini.SaveFile(temporary.c_str()) < 0 ||
        !MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        spdlog::error("Could not save settings.ini");
        return;
    }

    std::scoped_lock lock(mutex_);
    saved_revision_ = snapshot.revision;
}

SettingsSnapshot SettingsStore::requested() const {
    std::scoped_lock lock(mutex_);
    return {requested_, revision_};
}

Settings SettingsStore::active() const {
    std::scoped_lock lock(mutex_);
    return active_;
}

void SettingsStore::set_active(const Settings& settings) {
    std::scoped_lock lock(mutex_);
    active_ = settings;
}

RuntimeStatus SettingsStore::status() const {
    std::scoped_lock lock(mutex_);
    return status_;
}

bool SettingsStore::has_failed() const {
    std::scoped_lock lock(mutex_);
    return status_.failed;
}

void SettingsStore::set_input_ready() {
    std::scoped_lock lock(mutex_);
    status_.input_ready = true;
}

void SettingsStore::set_integration_ready() {
    std::scoped_lock lock(mutex_);
    status_.integration_ready = true;
}

void SettingsStore::fail(std::string message, std::uint32_t result) {
    spdlog::error("{} (NGX 0x{:08X})", message, result);

    std::scoped_lock lock(mutex_);
    status_.failed = true;
    status_.failure_revision = revision_;
    status_.last_error = std::move(message);
}

void SettingsStore::clear_failure() {
    std::scoped_lock lock(mutex_);
    status_.failed = false;
    status_.last_error.clear();
}
