#include "overlay_panel.hpp"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <iterator>
#include <limits>

#include <imgui.h>

namespace {
const ImVec4 accent{0.27f, 0.78f, 0.91f, 1.0f};
const ImVec4 positive{0.39f, 0.85f, 0.66f, 1.0f};
const ImVec4 caution{0.98f, 0.73f, 0.37f, 1.0f};

float scaled(float value) { return value * ImGui::GetFontSize() / 15.0f; }

struct PresetInfo {
    const char* model;
    const char* description;
    const char* note;
};

PresetInfo describe_preset(DlssPreset preset) {
    switch (preset) {
        case DlssPreset::Default:
            return {"Runtime default", "DLSS chooses the model for the selected quality mode.",
                    "The runtime or NVIDIA driver can override preset requests."};
        case DlssPreset::Latest:
            return {"Automatic selection",
                    "Requests Preset M for Performance; Preset K for DLAA, Quality and Balanced.",
                    "This selection policy does not update the DLSS DLL."};
        case DlssPreset::A:
        case DlssPreset::B:
        case DlssPreset::C:
        case DlssPreset::D:
            return {"Legacy CNN", "Historical Super Resolution model, removed from current DLSS runtimes.",
                    "A compatible older runtime is required; current runtimes may choose a fallback."};
        case DlssPreset::E:
            return {"CNN", "Legacy convolutional model with lower inference cost than Transformer models.",
                    "Deprecated. Removed in 310.5 and restored in 310.6; runtime support varies."};
        case DlssPreset::F:
            return {"CNN", "Legacy convolutional model originally tuned for DLAA and Ultra Performance.",
                    "Deprecated; support depends on the installed runtime."};
        case DlssPreset::J:
            return {"Transformer / generation 1", "Similar to K: less ghosting in some scenes, with more flickering.",
                    "NVIDIA generally recommends K over J."};
        case DlssPreset::K:
            return {"Transformer / generation 1", "DLSS 4 model used by default for DLAA, Quality and Balanced.",
                    "Lower inference cost than generation 2; a useful option on RTX 20/30."};
        case DlssPreset::L:
            return {"Transformer / generation 2", "DLSS 4.5 model tuned for Ultra Performance at 4K.",
                    "Higher GPU cost, especially on RTX 20/30 without native FP8 support."};
        case DlssPreset::M:
            return {"Transformer / generation 2",
                    "DLSS 4.5 model tuned for Performance; also supports other quality modes.",
                    "Higher GPU cost on RTX 20/30; compare the GPU upscale graph with K."};
    }
    return {"Unknown preset", "The runtime determines model availability.", ""};
}

void draw_preset_details(DlssPreset preset) {
    const auto info = describe_preset(preset);
    ImGui::TextColored(accent, "%s", info.model);
    ImGui::TextWrapped("%s", info.description);
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("%s", info.note);
    ImGui::PopStyleColor();
}

void preset_combo(DlssPreset& preset) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("MODEL PRESET");
    ImGui::SetNextItemWidth(-1.0f);
    constexpr std::array labels{"Default",  "Preset A", "Preset B", "Preset C", "Preset D", "Preset E",
                                "Preset F", "Preset J", "Preset K", "Preset L", "Preset M", "Latest"};
    const auto found = std::ranges::find(dlss_presets, preset);
    const auto selected_index = found == dlss_presets.end() ? 0 : std::distance(dlss_presets.begin(), found);
    if (!ImGui::BeginCombo("##preset", labels[selected_index])) return;

    for (std::size_t index = 0; index < dlss_presets.size(); ++index) {
        const bool selected = preset == dlss_presets[index];
        if (ImGui::Selectable(labels[index], selected)) preset = dlss_presets[index];
        if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            ImGui::PushTextWrapPos(ImGui::GetFontSize() * 28.0f);
            ImGui::TextUnformatted(labels[index]);
            ImGui::Separator();
            draw_preset_details(dlss_presets[index]);
            ImGui::PopTextWrapPos();
            ImGui::EndTooltip();
        }
        if (selected) ImGui::SetItemDefaultFocus();
    }
    ImGui::EndCombo();
}

template <typename Value, std::size_t Size>
void enum_combo(const char* label, Value& value, const std::array<Value, Size>& values,
                const std::array<const char*, Size>& names) {
    const auto found = std::ranges::find(values, value);
    int selected = found == values.end() ? 0 : static_cast<int>(std::distance(values.begin(), found));
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", label);
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::PushID(label);
    if (ImGui::Combo("##selection", &selected, names.data(), static_cast<int>(Size))) value = values[selected];
    ImGui::PopID();
}
}  // namespace

void OverlayPanel::apply_style(float dpi) {
    ImGui::StyleColorsDark();
    auto& style = ImGui::GetStyle();
    style.WindowPadding = {20, 18};
    style.FramePadding = {10, 8};
    style.ItemSpacing = {12, 10};
    style.CellPadding = {6, 4};
    style.WindowRounding = 12;
    style.ChildRounding = 10;
    style.FrameRounding = 6;
    style.GrabRounding = 6;
    style.PopupRounding = 8;
    style.ScrollbarRounding = 8;
    style.WindowBorderSize = 1;
    style.ChildBorderSize = 1;
    style.FontSizeBase = 15;
    style.FontScaleDpi = dpi;
    style.Colors[ImGuiCol_WindowBg] = {0.055f, 0.071f, 0.10f, 1.0f};
    style.Colors[ImGuiCol_ChildBg] = {0.085f, 0.105f, 0.14f, 1.0f};
    style.Colors[ImGuiCol_PopupBg] = {0.09f, 0.11f, 0.15f, 1.0f};
    style.Colors[ImGuiCol_Border] = {0.17f, 0.21f, 0.27f, 1.0f};
    style.Colors[ImGuiCol_Text] = {0.9f, 0.93f, 0.97f, 1.0f};
    style.Colors[ImGuiCol_TextDisabled] = {0.52f, 0.61f, 0.71f, 1.0f};
    style.Colors[ImGuiCol_TitleBg] = style.Colors[ImGuiCol_WindowBg];
    style.Colors[ImGuiCol_TitleBgActive] = style.Colors[ImGuiCol_WindowBg];
    style.Colors[ImGuiCol_FrameBg] = {0.12f, 0.16f, 0.21f, 1.0f};
    style.Colors[ImGuiCol_FrameBgHovered] = {0.17f, 0.24f, 0.30f, 1.0f};
    style.Colors[ImGuiCol_FrameBgActive] = {0.18f, 0.30f, 0.36f, 1.0f};
    style.Colors[ImGuiCol_Header] = {0.12f, 0.31f, 0.38f, 1.0f};
    style.Colors[ImGuiCol_HeaderHovered] = {0.15f, 0.40f, 0.48f, 1.0f};
    style.Colors[ImGuiCol_HeaderActive] = style.Colors[ImGuiCol_HeaderHovered];
    style.Colors[ImGuiCol_CheckMark] = accent;
    style.ScaleAllSizes(dpi);
}

void OverlayPanel::sample(double frame_ms, std::optional<float> upscale_ms) {
    const auto now = std::chrono::steady_clock::now();
    if (now - sampled_at_ > std::chrono::seconds(1)) {
        count_ = cursor_ = 0;
        sampled_at_ = {};
    }

    if (now - sampled_at_ < std::chrono::milliseconds(100)) return;
    sampled_at_ = now;
    const auto missing = std::numeric_limits<float>::quiet_NaN();
    const bool frame_valid = std::isfinite(frame_ms) && frame_ms > 0.0;
    history_[cursor_] = {frame_valid ? static_cast<float>(frame_ms) : missing,
                         upscale_ms && std::isfinite(*upscale_ms) ? *upscale_ms : missing,
                         frame_valid ? static_cast<float>(1000.0 / frame_ms) : missing};
    cursor_ = (cursor_ + 1) % history_.size();
    count_ = std::min(count_ + 1, history_.size());
}

void OverlayPanel::draw_metric(const char* label, float MetricSample::* metric, const char* unit,
                               const ImVec4& color) const {
    ImGui::PushID(label);
    if (ImGui::BeginChild("card", {0, 0}, ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY)) {
        ImGui::TextDisabled("%s", label);
        const auto latest = count_ ? history_[(cursor_ + history_.size() - 1) % history_.size()].*metric
                                   : std::numeric_limits<float>::quiet_NaN();
        ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.45f);
        if (std::isfinite(latest))
            ImGui::TextColored(color, *unit ? "%.2f %s" : "%.0f%s", latest, unit);
        else
            ImGui::TextColored(color, "--");
        ImGui::PopFont();

        float minimum = std::numeric_limits<float>::max(), maximum = 0.0f;
        const auto start = (cursor_ + history_.size() - count_) % history_.size();
        for (std::size_t index = 0; index < count_; ++index) {
            const auto value = history_[(start + index) % history_.size()].*metric;
            if (!std::isfinite(value)) continue;
            minimum = std::min(minimum, value);
            maximum = std::max(maximum, value);
        }

        const auto origin = ImGui::GetCursorScreenPos();
        const ImVec2 size{ImGui::GetContentRegionAvail().x, scaled(54)};
        ImGui::InvisibleButton("history", size);
        auto* draw = ImGui::GetWindowDrawList();
        const float ceiling = std::max(maximum * 1.15f, 0.1f);
        for (int line = 1; line < 4; ++line) {
            const float y = origin.y + size.y * line / 4.0f;
            draw->AddLine({origin.x, y}, {origin.x + size.x, y}, IM_COL32(42, 53, 68, 255));
        }

        std::optional<ImVec2> previous;
        for (std::size_t index = 0; index < count_; ++index) {
            const auto value = history_[(start + index) % history_.size()].*metric;
            if (!std::isfinite(value)) {
                previous.reset();
                continue;
            }

            const ImVec2 point{origin.x + size.x * static_cast<float>(history_.size() - count_ + index) /
                                              static_cast<float>(history_.size() - 1),
                               origin.y + size.y * (1.0f - value / ceiling)};
            if (previous) draw->AddLine(*previous, point, ImGui::ColorConvertFloat4ToU32(color), scaled(1.5f));
            previous = point;
        }

        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Last 16 seconds / 10 Hz\nScale: 0 - %.2f %s", ceiling, unit);
        if (minimum <= maximum)
            ImGui::TextDisabled("%.1f - %.1f %s", minimum, maximum, unit);
        else
            ImGui::TextDisabled("No samples yet");
    }
    ImGui::EndChild();
    ImGui::PopID();
}

void OverlayPanel::draw(Settings& settings, bool& open, const OverlayInfo& info) const {
    ImGui::SetNextWindowSize({scaled(620), scaled(750)}, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints({scaled(540), scaled(480)}, {FLT_MAX, FLT_MAX});
    if (ImGui::Begin("dlss_for_cs2", &open, ImGuiWindowFlags_NoCollapse)) {
        ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.6f);
        ImGui::TextUnformatted("DLSS control");
        ImGui::PopFont();
        ImGui::TextDisabled("%.*s  /  DX11  /  DLSS %.*s", static_cast<int>(info.gpu.size()), info.gpu.data(),
                            static_cast<int>(info.version.size()), info.version.data());

        const char* state = info.status.failed                                           ? "Error"
                            : !info.status.integration_ready || !info.status.input_ready ? "Unavailable"
                            : settings != info.active_settings                           ? "Applying settings..."
                            : info.dlss_active                                           ? "DLSS active"
                                                                                         : "DLSS disabled";
        ImGui::TextColored(info.status.failed ? caution : info.dlss_active ? positive : accent, "%s", state);
        if (info.input_width && info.input_height)
            ImGui::TextDisabled("Input %u x %u  /  Output %u x %u", info.input_width, info.input_height,
                                info.output_width, info.output_height);
        else
            ImGui::TextDisabled("Input --  /  Output %u x %u", info.output_width, info.output_height);
        ImGui::Spacing();

        if (ImGui::BeginChild("settings", {0, 0}, ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY)) {
            ImGui::BeginDisabled(!info.status.integration_ready || !info.status.input_ready);
            ImGui::Checkbox("Enable DLSS", &settings.enabled);
            if (ImGui::BeginTable("choices", 2, ImGuiTableFlags_SizingStretchSame)) {
                ImGui::TableNextColumn();
                enum_combo("QUALITY MODE", settings.quality, dlss_qualities, quality_names);
                ImGui::TableNextColumn();
                preset_combo(settings.preset);
                ImGui::EndTable();
            }
            ImGui::Spacing();
            ImGui::SetNextItemWidth(-scaled(100));
            ImGui::SliderFloat("Sharpness", &settings.sharpness, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("RCAS after DLSS. 0: off / 1: maximum sharpness.");
            ImGui::EndDisabled();

            ImGui::TextDisabled("Render scale %.1f%% per axis", render_scale(settings) * 100.0f);
            ImGui::TextDisabled("Changes apply automatically. MSAA and AO stay off while active.");
        }
        ImGui::EndChild();

        ImGui::TextDisabled("PERFORMANCE  /  LAST 16 SECONDS");
        if (ImGui::BeginTable("metrics", 3, ImGuiTableFlags_SizingStretchSame)) {
            ImGui::TableNextColumn();
            draw_metric("GPU UPSCALE", &MetricSample::upscale_ms, "ms", accent);
            ImGui::TableNextColumn();
            draw_metric("FRAME TIME", &MetricSample::frame_ms, "ms", caution);
            ImGui::TableNextColumn();
            draw_metric("FPS", &MetricSample::fps, "", positive);
            ImGui::EndTable();
        }
        ImGui::TextDisabled("GPU: NGX evaluation only. Frame time / FPS: Present averages.");

        if (!info.status.last_error.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, caution);
            ImGui::TextWrapped("%s", info.status.last_error.c_str());
            ImGui::PopStyleColor();
        }
        if (!info.status.input_ready) ImGui::TextWrapped("Game input capture is unavailable.");
        ImGui::Separator();
        ImGui::TextDisabled("Preset details describe the request; driver overrides may differ.");
        ImGui::TextDisabled("Insert to close");
    }
    ImGui::End();
}
