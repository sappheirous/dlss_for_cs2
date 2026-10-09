#include "game.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <memory>
#include <set>
#include <span>
#include <string>
#include <utility>

#include <spdlog/spdlog.h>

#include "dlss.hpp"
#include "hooks.hpp"
#include "ngx.hpp"
#include "settings.hpp"

GameApi game;
std::atomic_bool is_enabled{false};

bool readable(const void* address, std::size_t bytes) noexcept {
    auto cursor = reinterpret_cast<std::uintptr_t>(address);
    if (!cursor || !bytes || cursor > std::numeric_limits<std::uintptr_t>::max() - bytes) return false;

    const auto end = cursor + bytes;
    while (cursor < end) {
        MEMORY_BASIC_INFORMATION region{};
        if (!VirtualQuery(reinterpret_cast<void*>(cursor), &region, sizeof(region)) || region.State != MEM_COMMIT ||
            (region.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
            return false;

        const auto next = reinterpret_cast<std::uintptr_t>(region.BaseAddress) + region.RegionSize;
        if (next <= cursor) return false;
        cursor = next;
    }

    return true;
}

bool is_executable(const void* address) noexcept {
    MEMORY_BASIC_INFORMATION region{};
    if (!VirtualQuery(address, &region, sizeof(region)) || region.State != MEM_COMMIT || (region.Protect & PAGE_GUARD))
        return false;

    const DWORD protection = region.Protect & 0xFF;
    return protection == PAGE_EXECUTE || protection == PAGE_EXECUTE_READ || protection == PAGE_EXECUTE_READWRITE ||
           protection == PAGE_EXECUTE_WRITECOPY;
}

std::vector<std::uint8_t*> find_patterns(const char* module, const char* pattern) {
    auto base = reinterpret_cast<std::uint8_t*>(GetModuleHandleA(module));
    if (!readable(base, sizeof(IMAGE_DOS_HEADER))) return {};

    const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) return {};

    const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (!readable(nt, sizeof(*nt)) || nt->Signature != IMAGE_NT_SIGNATURE) return {};

    std::vector<int> bytes;
    for (auto cursor = pattern; *cursor;) {
        if (*cursor == ' ') {
            ++cursor;
            continue;
        }

        if (*cursor == '?') {
            bytes.push_back(-1);
            while (*cursor == '?') ++cursor;
            continue;
        }

        char* next = nullptr;
        const auto value = std::strtoul(cursor, &next, 16);
        if (next != cursor + 2 || value > 255) return {};
        bytes.push_back(static_cast<int>(value));
        cursor = next;
    }

    if (bytes.empty()) return {};

    std::vector<std::uint8_t*> matches;
    const auto sections = IMAGE_FIRST_SECTION(nt);
    if (!readable(sections, nt->FileHeader.NumberOfSections * sizeof(IMAGE_SECTION_HEADER))) return {};

    for (WORD section = 0; section < nt->FileHeader.NumberOfSections; ++section) {
        const auto& entry = sections[section];
        if (!(entry.Characteristics & IMAGE_SCN_MEM_EXECUTE) || entry.VirtualAddress >= nt->OptionalHeader.SizeOfImage)
            continue;

        const auto size = static_cast<std::size_t>(entry.Misc.VirtualSize);
        auto start = base + entry.VirtualAddress;
        if (size > nt->OptionalHeader.SizeOfImage - entry.VirtualAddress || !readable(start, size)) continue;

        for (std::size_t offset = 0; offset + bytes.size() <= size; ++offset) {
            const auto candidate = std::span(start + offset, bytes.size());
            if (std::ranges::equal(candidate, bytes, [](std::uint8_t actual, int expected) {
                    return expected == -1 || actual == expected;
                }))
                matches.push_back(start + offset);
        }
    }

    return matches;
}

std::uint8_t* find_pattern(const char* module, const char* pattern) {
    const auto matches = find_patterns(module, pattern);
    if (matches.size() != 1) {
        spdlog::error("{} signature matched {} locations; integration disabled", module, matches.size());
        return nullptr;
    }

    return matches.front();
}

std::uint8_t* relative(std::uint8_t* address, int offset) {
    if (!address || !readable(address + offset, sizeof(std::int32_t))) return nullptr;

    std::int32_t displacement = 0;
    std::memcpy(&displacement, address + offset, sizeof(displacement));
    return address + offset + sizeof(displacement) + displacement;
}

namespace {
using AddView = void*(__fastcall*)(void*, const char*, void*, const Frustum*, void*, void*, const RenderViewport*,
                                   const void*, std::uint32_t);
using FinishRenderingViews = void(__fastcall*)(void*, float);
using AddUpscaleLayers = std::uint64_t(__fastcall*)(void*, void*);
using FsrRender = void(__fastcall*)(FsrUpscaleRenderer*, void*, void*, void*);
AddView o_add_view{};
FinishRenderingViews o_finish_rendering_views{};
AddUpscaleLayers o_add_upscale_layers{};
FsrRender o_fsr_render{};

struct ConVarReference {
    std::uint64_t handle;
    ConVar* data;
};
static_assert(sizeof(ConVarReference) == 16);

using SetConVarString = bool(__fastcall*)(ConVarReference*, int, const char*);
SetConVarString set_con_var_string{};

ConVarReference viewport_reference{};
ConVarReference fsr_reference{};
ConVarReference ao_reference{};
std::uint64_t applied_revision = 0;
bool owns_settings = false;
float original_scale = 1.0f;
int original_fsr = 0;
int original_msaa_samples = 0;
bool original_ao = false;
int original_video_ao = 0;
Multisample original_multisample = Multisample::None;

void* engine_services = nullptr;
void* device_manager = nullptr;
int(__fastcall* get_key_value_int)(void*, const char*, int) = nullptr;
void(__fastcall* set_key_value_int)(void*, const char*, int) = nullptr;
bool msaa_change_pending = false;
Multisample pending_multisample = Multisample::None;
std::chrono::steady_clock::time_point msaa_requested_at{};
std::chrono::steady_clock::time_point last_settings_check{};

constexpr int device_config_index = 21;
constexpr int set_device_config_index = 36;
constexpr int video_config_index = 25;
constexpr const char* msaa_setting = "setting.msaa_samples";
constexpr const char* ao_setting = "setting.r_ssao";

enum class VideoSettingResult { Ready, Pending, Failed };

void* find_interface(const char* module_name, const char* name) {
    const auto module = GetModuleHandleA(module_name);
    const auto create = reinterpret_cast<void*(__cdecl*)(const char*, int*)>(GetProcAddress(module, "CreateInterface"));
    return is_executable(reinterpret_cast<void*>(create)) ? create(name, nullptr) : nullptr;
}

bool valid_interface(void* instance, std::initializer_list<int> slots) {
    if (!readable(instance, sizeof(void*))) return false;

    const auto table = *static_cast<void***>(instance);
    return std::ranges::all_of(
        slots, [table](int slot) { return readable(table + slot, sizeof(void*)) && is_executable(table[slot]); });
}

bool read_video_state(void*& values, RenderDeviceConfig& config) {
    values = call_virtual<void*>(device_manager, video_config_index);
    const auto current = call_virtual<const RenderDeviceConfig*>(engine_services, device_config_index);
    if (!readable(values, sizeof(void*)) || !readable(current, sizeof(config)) ||
        std::to_underlying(current->multisample) > std::to_underlying(Multisample::X16))
        return false;

    config = *current;
    return true;
}

VideoSettingResult set_msaa(int samples, Multisample multisample) {
    void* values = nullptr;
    RenderDeviceConfig config{};
    if (!read_video_state(values, config)) return VideoSettingResult::Failed;

    if (get_key_value_int(values, msaa_setting, -1) != samples) set_key_value_int(values, msaa_setting, samples);
    if (get_key_value_int(values, msaa_setting, -1) != samples) return VideoSettingResult::Failed;

    if (config.multisample == multisample && (!msaa_change_pending || pending_multisample == multisample)) {
        msaa_change_pending = false;
        return VideoSettingResult::Ready;
    }

    if (!msaa_change_pending || pending_multisample != multisample) {
        config.multisample = multisample;
        call_virtual<void>(engine_services, set_device_config_index, &config);
        pending_multisample = multisample;
        msaa_change_pending = true;
        msaa_requested_at = std::chrono::steady_clock::now();
        spdlog::info("Requested game MSAA samples {}", samples);
    }

    if (std::chrono::steady_clock::now() - msaa_requested_at > std::chrono::seconds(5)) {
        msaa_change_pending = false;
        return VideoSettingResult::Failed;
    }

    return VideoSettingResult::Pending;
}

ConVarReference find_con_var(const char* name) {
    if (!readable(game.engine_c_var + 0x50, 12)) return {0xFFFF, nullptr};

    auto nodes = *reinterpret_cast<ConVarNode**>(game.engine_c_var + 0x50);
    const auto count = *reinterpret_cast<std::uint16_t*>(game.engine_c_var + 0x5A);
    if (!readable(nodes, static_cast<std::size_t>(count) * sizeof(ConVarNode))) return {0xFFFF, nullptr};
    const auto name_size = std::strlen(name) + 1;

    for (std::uint16_t index = 0; index < count; ++index) {
        auto variable = nodes[index].con_var;
        if (readable(variable, sizeof(ConVar)) && readable(variable->name, name_size) &&
            std::strcmp(variable->name, name) == 0)
            return {index, variable};
    }

    return {0xFFFF, nullptr};
}

bool set_scale(float scale, int fsr) {
    char value[32]{};
    bool scale_parsed = true, fsr_parsed = true;
    if (std::abs(game.viewport_scale->float_value - scale) >= 0.0001f) {
        std::snprintf(value, sizeof(value), "%.9g", static_cast<double>(scale));
        scale_parsed = set_con_var_string(&viewport_reference, -1, value);
    }

    if (game.fsr_upsample->int_value != fsr) {
        std::snprintf(value, sizeof(value), "%d", fsr);
        fsr_parsed = set_con_var_string(&fsr_reference, -1, value);
    }

    return scale_parsed && fsr_parsed && std::abs(game.viewport_scale->float_value - scale) < 0.0001f &&
           game.fsr_upsample->int_value == fsr;
}

bool set_ambient_occlusion(bool value, int video_value) {
    void* values = nullptr;
    RenderDeviceConfig config{};
    if (!ao_reference.data || !read_video_state(values, config)) return false;

    if (get_key_value_int(values, ao_setting, -1) != video_value) set_key_value_int(values, ao_setting, video_value);

    if (ao_reference.data->bool_value != value) {
        char text[16]{};
        std::snprintf(text, sizeof(text), "%d", value);
        if (!set_con_var_string(&ao_reference, -1, text)) return false;
    }

    return ao_reference.data->bool_value == value && get_key_value_int(values, ao_setting, -1) == video_value;
}

bool restore_game_settings() {
    if (!owns_settings) return true;

    const bool scale_restored = set_scale(original_scale, original_fsr);
    const bool ao_restored = set_ambient_occlusion(original_ao, original_video_ao);
    const auto msaa = set_msaa(original_msaa_samples, original_multisample);
    if (!scale_restored || !ao_restored || msaa != VideoSettingResult::Ready) return false;

    owns_settings = false;
    spdlog::info("Restored game scale {}, FSR {}, MSAA samples {} and Ambient Occlusion {}", original_scale,
                 original_fsr, original_msaa_samples, original_ao);
    return true;
}

void* __fastcall hk_add_view(void* scene_system, const char* name, void* view_id, const Frustum* frustum,
                             void* swap_chain, void* world, const RenderViewport* viewport, const void* vis,
                             std::uint32_t priority) {
    Frustum jittered{};
    const bool use_jitter = renderer().build_jittered_frustum(name, frustum, &jittered);
    const auto* render_frustum = use_jitter ? &jittered : frustum;
    auto view = o_add_view(scene_system, name, view_id, render_frustum, swap_chain, world, viewport, vis, priority);
    if (render_frustum)
        renderer().on_add_view(view, name, view_id, frustum, render_frustum->jitter_x, render_frustum->jitter_y);
    return view;
}

void __fastcall hk_finish_rendering_views(void* scene_system, float time) {
    o_finish_rendering_views(scene_system, time);
    renderer().on_finish_rendering_views();
}

std::uint64_t __fastcall hk_add_upscale_layers(void* pipeline, void* context) {
    const auto result = o_add_upscale_layers(pipeline, context);
    renderer().on_add_upscale_layers(static_cast<const UpscaleLayersContext*>(context));
    return result;
}

void __fastcall hk_fsr_render(FsrUpscaleRenderer* upscale_renderer, void* view, void* context, void* layer) {
    // client RVA 0x1210650 only draws the FSR quad; mode 1 has no following RCAS pass (0x11F8070).
    if (renderer().on_upscale_render(upscale_renderer, context, layer, false)) return;

    o_fsr_render(upscale_renderer, view, context, layer);
    renderer().on_upscale_render(upscale_renderer, context, layer, true);
}
}  // namespace

bool initialize_game() {
    spdlog::info("Initializing Source 2 integration");
    const auto fail = [](const std::string& reason) {
        store().fail("Source 2 initialization: " + reason);
        return false;
    };

    if (!bind_ngx()) return fail("required NGX exports are unavailable in rendersystemdx11.dll");

    auto scene =
        relative(find_pattern("client.dll", "48 8B 0D ?? ?? ?? ?? 48 8B 01 FF 90 ?? ?? ?? ?? 48 8B 53 ?? 48 85 D2"), 3);

    std::set<std::uint8_t*> cvar_targets;
    for (auto match : find_patterns("client.dll", "48 8B 0D ?? ?? ?? ?? 4C 8D 43 07 45 33 C9"))
        cvar_targets.insert(relative(match, 3));

    if (!readable(scene, sizeof(void*))) return fail("scene-system pointer location is unavailable or unreadable");
    if (cvar_targets.size() != 1)
        return fail(fmt::format("ConVar signature resolves to {} distinct targets", cvar_targets.size()));
    if (!readable(*cvar_targets.begin(), sizeof(void*))) return fail("ConVar pointer location is unreadable");

    game.scene_system = *reinterpret_cast<void**>(scene);
    game.engine_c_var = *reinterpret_cast<std::uint8_t**>(*cvar_targets.begin());

    game.get_handle_value = reinterpret_cast<decltype(game.get_handle_value)>(
        find_pattern("client.dll", "48 83 EC ?? 8B 44 24 ?? 4C 8D 1D"));
    game.build_frustum = reinterpret_cast<decltype(game.build_frustum)>(
        relative(find_pattern("engine2.dll", "E8 ?? ?? ?? ?? 45 84 F6 74 0C"), 1));

    auto add_upscale = find_pattern("client.dll",
                                    "40 55 53 48 8D 6C 24 ?? 48 81 EC ?? ?? ?? ?? 48 8B 4A 38 45 33 C0 48 81 "
                                    "C1 10 01 00 00 48 8B DA 44 38 82 B8 01 00 00 BA FA AA 68 63");
    auto fsr_render = find_pattern("client.dll",
                                   "48 89 74 24 ?? 55 57 41 57 48 8D AC 24 ?? ?? ?? ?? 48 81 EC ?? ?? ?? ?? "
                                   "83 B9 90 00 00 00 00 4D 8B F9 49 8B F8 48 8B F1 0F 84");
    auto finish = find_pattern(
        "scenesystem.dll", "48 8B C4 48 89 58 10 48 89 70 18 48 89 78 20 48 89 48 08 55 41 54 41 55 41 56 41 57 48 81");
    set_con_var_string = reinterpret_cast<SetConVarString>(
        find_pattern("tier0.dll", "48 89 5C 24 ?? 48 89 74 24 ?? 57 48 83 EC 30 4C 8B 59"));

    if (!readable(game.scene_system, sizeof(void*))) return fail("scene-system instance is unavailable or unreadable");
    if (!readable(game.engine_c_var, 0x60)) return fail("ConVar interface is unavailable or unreadable");

    struct ResolvedTarget {
        const char* name;
        void* address;
    };
    const ResolvedTarget targets[] = {{"GetHandleValue", reinterpret_cast<void*>(game.get_handle_value)},
                                      {"BuildFrustum", reinterpret_cast<void*>(game.build_frustum)},
                                      {"AddUpscaleLayers", add_upscale},
                                      {"FsrRender", fsr_render},
                                      {"FinishRenderingViews", finish},
                                      {"SetConVarString", reinterpret_cast<void*>(set_con_var_string)}};
    for (const auto& target : targets) {
        if (!is_executable(target.address))
            return fail(fmt::format("{} target is unavailable or not executable ({})", target.name, target.address));
    }

    const auto table = *reinterpret_cast<void***>(game.scene_system);
    if (!readable(table, (add_view_index + 1) * sizeof(void*)) || !is_executable(table[add_view_index]))
        return fail(fmt::format("scene-system AddView vtable slot {} is unreadable or not executable", add_view_index));

    viewport_reference = find_con_var("mat_viewportscale");
    fsr_reference = find_con_var("r_csgo_fsr_upsample");
    // client RVAs 0x109200/0xDEE4E0 identify r_ssao/setting.r_ssao; 0x11FAEAB reads its bool at 0x58.
    ao_reference = find_con_var("r_ssao");
    game.viewport_scale = viewport_reference.data;
    game.fsr_upsample = fsr_reference.data;
    if (!game.viewport_scale) return fail("mat_viewportscale ConVar was not found");
    if (!game.fsr_upsample) return fail("r_csgo_fsr_upsample ConVar was not found");
    if (!ao_reference.data) return fail("r_ssao ConVar was not found");

    // client.dll RVA 0x1205F30 copies a 40-byte device config, changes MSAA at 0x24, then calls slot 36.
    // EngineServiceMgr001 slots 21/36 read/queue that config; RenderDeviceMgr001 slot 25 returns video KeyValues.
    engine_services = find_interface("engine2.dll", "EngineServiceMgr001");
    device_manager = find_interface("rendersystemdx11.dll", "RenderDeviceMgr001");
    if (!valid_interface(engine_services, {device_config_index, set_device_config_index}) ||
        !valid_interface(device_manager, {video_config_index}))
        return fail("MSAA video configuration interfaces are unavailable");

    const auto tier0 = GetModuleHandleW(L"tier0.dll");
    get_key_value_int =
        reinterpret_cast<decltype(get_key_value_int)>(GetProcAddress(tier0, "?GetInt@KeyValues@@QEBAHPEBDH@Z"));
    set_key_value_int =
        reinterpret_cast<decltype(set_key_value_int)>(GetProcAddress(tier0, "?SetInt@KeyValues@@QEAAXPEBDH@Z"));
    if (!is_executable(reinterpret_cast<void*>(get_key_value_int)) ||
        !is_executable(reinterpret_cast<void*>(set_key_value_int)))
        return fail("MSAA video configuration accessors are unavailable");

    static auto hooks = std::make_unique<HookSet>();
    if (!hooks->create(table[add_view_index], &hk_add_view, o_add_view) ||
        !hooks->create(finish, &hk_finish_rendering_views, o_finish_rendering_views) ||
        !hooks->create(add_upscale, &hk_add_upscale_layers, o_add_upscale_layers) ||
        !hooks->create(fsr_render, &hk_fsr_render, o_fsr_render) || !hooks->enable()) {
        hooks->reset();
        return fail("could not install scene hooks; see MinHook diagnostics");
    }

    store().set_integration_ready();
    spdlog::info("Source 2 integration ready; ConVar setter {}", reinterpret_cast<void*>(set_con_var_string));
    return true;
}

void apply_game_settings() {
    // Only called by input.cpp after ThreadInMainThread() succeeds.
    const auto status = store().status();
    if (!game.viewport_scale || !status.integration_ready) return;
    if (renderer().display_change_active()) return;

    const auto snapshot = store().requested();
    if (status.failed) {
        is_enabled = false;
        if (!restore_game_settings()) return;
        if (snapshot.revision <= status.failure_revision) return;
    }

    if (snapshot.revision == applied_revision) {
        if (!snapshot.value.enabled || !owns_settings) return;

        const auto now = std::chrono::steady_clock::now();
        if (is_enabled && !msaa_change_pending && now - last_settings_check < std::chrono::milliseconds(100)) return;
        last_settings_check = now;

        void* values = nullptr;
        RenderDeviceConfig config{};
        if (is_enabled && read_video_state(values, config) && config.multisample == Multisample::None &&
            get_key_value_int(values, msaa_setting, -1) == 0 && !ao_reference.data->bool_value &&
            get_key_value_int(values, ao_setting, -1) == 0 &&
            std::abs(game.viewport_scale->float_value - render_scale(snapshot.value)) < 0.0001f &&
            game.fsr_upsample->int_value == game_fsr_mode(snapshot.value))
            return;
        if (!msaa_change_pending)
            spdlog::info("Game changed DLSS-owned scale/FSR/MSAA/AO; reapplying on the main thread");
    }

    const auto active = store().active();
    if (!status.failed && is_enabled && owns_settings && !msaa_change_pending && snapshot.value.enabled &&
        active.quality == snapshot.value.quality && active.preset == snapshot.value.preset &&
        snapshot.revision != applied_revision) {
        store().set_active(snapshot.value);
        applied_revision = snapshot.revision;
        return;
    }

    is_enabled = false;
    if (!snapshot.value.enabled) {
        if (!restore_game_settings()) return;
        store().set_active(snapshot.value);
        applied_revision = snapshot.revision;
        return;
    }

    if (!owns_settings) {
        void* values = nullptr;
        RenderDeviceConfig config{};
        if (!read_video_state(values, config)) {
            store().fail("Cannot read game MSAA configuration");
            applied_revision = snapshot.revision;
            return;
        }

        original_scale = game.viewport_scale->float_value;
        original_fsr = game.fsr_upsample->int_value;
        original_msaa_samples = get_key_value_int(values, msaa_setting, 0);
        original_ao = ao_reference.data->bool_value;
        original_video_ao = get_key_value_int(values, ao_setting, original_ao);
        original_multisample = config.multisample;
        owns_settings = true;
    }

    if (!set_ambient_occlusion(false, 0)) {
        store().fail("Game could not disable Ambient Occlusion; original settings will be restored");
        restore_game_settings();
        applied_revision = snapshot.revision;
        return;
    }

    const auto msaa = set_msaa(0, Multisample::None);
    if (msaa == VideoSettingResult::Pending) return;
    if (msaa == VideoSettingResult::Failed) {
        store().fail("Game could not disable MSAA; original settings will be restored");
        restore_game_settings();
        applied_revision = snapshot.revision;
        return;
    }

    if (!set_scale(render_scale(snapshot.value), game_fsr_mode(snapshot.value))) {
        store().fail("Game rejected render scale; original settings will be restored");
        restore_game_settings();
        applied_revision = snapshot.revision;
        return;
    }

    renderer().prepare_retry();
    store().set_active(snapshot.value);
    store().clear_failure();
    applied_revision = snapshot.revision;
    is_enabled = true;

    spdlog::info("Applied DLSS quality {}, preset {}, game FSR {}", quality_name(snapshot.value.quality),
                 preset_name(snapshot.value.preset), game_fsr_mode(snapshot.value));
}
