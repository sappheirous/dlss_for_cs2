#include "input.hpp"

#include <atomic>
#include <exception>
#include <memory>

#include <spdlog/spdlog.h>

#include "game.hpp"
#include "hooks.hpp"
#include "settings.hpp"

namespace {
constexpr int enable_input = 13;
constexpr int poll_input = 15;
constexpr int reset_input = 37;
constexpr int get_analog_delta = 26;
constexpr int get_standard_cursor = 50;
constexpr int set_cursor = 59;
constexpr int set_capture = 65;
constexpr int set_clip = 73;
constexpr int set_relative_mode = 76;
constexpr int default_cursor_index = 1;

std::atomic_bool menu_requested{false};
std::atomic_bool capturing{false};
std::atomic_bool discard_mouse{false};

using PollInput = void(__fastcall*)(void*, bool);
using SetBool = void(__fastcall*)(void*, bool);
using SetWindow = void(__fastcall*)(void*, void*);
using SetCursor = void(__fastcall*)(void*, void*, bool);
using MouseMove = void(__fastcall*)(void*, void*, void*, float, float, bool, std::uint64_t);
using AnalogDelta = float(__fastcall*)(void*, int);

PollInput o_poll{};
SetBool o_enable{};
SetBool o_relative{};
SetWindow o_capture{};
SetWindow o_clip{};
SetCursor o_cursor{};
MouseMove o_mouse_move{};
AnalogDelta o_analog_delta{};

bool(__cdecl* in_main_thread)() = nullptr;
void* input_system = nullptr;

std::atomic_bool desired_enable{true};
std::atomic_bool desired_relative{false};
std::atomic<void*> desired_capture{nullptr};
std::atomic<void*> desired_clip{nullptr};
std::atomic<void*> desired_cursor{nullptr};
void* arrow_cursor = nullptr;

bool block_mouse() noexcept { return menu_requested || capturing || discard_mouse; }

void __fastcall hk_mouse_move(void* self, void* state, void* platform_window, float x, float y, bool report_delta,
                              std::uint64_t timestamp) {
    // Keep absolute coordinates current so leaving/re-entering the window cannot accumulate a jump.
    o_mouse_move(self, state, platform_window, x, y, self == input_system && block_mouse() ? false : report_delta,
                 timestamp);
}

float __fastcall hk_analog_delta(void* self, int code) {
    return self == input_system && block_mouse() ? 0.0f : o_analog_delta(self, code);
}

void __fastcall hk_enable(void* self, bool enabled) {
    desired_enable = enabled;
    o_enable(self, capturing ? false : enabled);
}

void __fastcall hk_relative(void* self, bool enabled) {
    desired_relative = enabled;
    o_relative(self, capturing ? false : enabled);
}

void __fastcall hk_capture(void* self, void* window) {
    desired_capture = window;
    o_capture(self, capturing ? nullptr : window);
}

void __fastcall hk_clip(void* self, void* window) {
    desired_clip = window;
    o_clip(self, capturing ? nullptr : window);
}

void __fastcall hk_cursor(void* self, void* cursor, bool force) {
    desired_cursor = cursor;
    o_cursor(self, capturing ? arrow_cursor : cursor, force);
}

void update_capture() {
    const bool open = menu_requested;
    if (open == capturing.load()) return;
    if (open) {
        desired_enable = field<bool>(input_system, 0x48);
        desired_relative = field<bool>(input_system, 0x54);
        desired_capture = field<void*>(input_system, 0x2640);
        desired_clip = field<void*>(input_system, 0x2648);
        desired_cursor = field<void*>(input_system, 0x2870);

        call_virtual<void>(input_system, reset_input);
        capturing = true;

        o_enable(input_system, false);
        o_relative(input_system, false);
        o_capture(input_system, nullptr);
        o_clip(input_system, nullptr);
        o_cursor(input_system, arrow_cursor, true);

        spdlog::info("Overlay input captured on the main thread");
    } else {
        discard_mouse = true;
        capturing = false;

        o_cursor(input_system, desired_cursor, true);
        o_clip(input_system, desired_clip);
        o_capture(input_system, desired_capture);
        o_relative(input_system, desired_relative);
        o_enable(input_system, desired_enable);

        call_virtual<void>(input_system, reset_input);

        spdlog::info("Game input restored on the main thread");
    }
}

void __fastcall hk_poll(void* self, bool in_game) {
    const bool main_poll = self == input_system && in_main_thread();
    if (main_poll) {
        try {
            static bool poll_reported = false;
            if (!poll_reported) {
                spdlog::info("Input polling hook reached the main thread");
                poll_reported = true;
            }

            update_capture();
            apply_game_settings();
        } catch (const std::exception& error) {
            is_enabled = false;
            store().fail(error.what());
            menu_requested = false;
        }
    }

    o_poll(self, in_game);

    // SDL can queue a warp when relative mode resumes. Drain it with zero deltas before gameplay.
    if (main_poll && !menu_requested && !capturing) discard_mouse = false;
}
}  // namespace

bool menu_open() noexcept { return menu_requested; }

void set_menu_open(bool open) noexcept { menu_requested = open; }

bool initialize_input() {
    spdlog::info("Initializing InputSystemVersion001");

    const auto module = GetModuleHandleW(L"inputsystem.dll");
    using CreateInterface = void*(__cdecl*)(const char*, int*);
    const auto factory = reinterpret_cast<CreateInterface>(GetProcAddress(module, "CreateInterface"));
    in_main_thread = reinterpret_cast<decltype(in_main_thread)>(
        GetProcAddress(GetModuleHandleW(L"tier0.dll"), "ThreadInMainThread"));

    if (!is_executable(reinterpret_cast<void*>(factory))) {
        store().fail("Input initialization: CreateInterface is missing or not executable");
        return false;
    }

    if (!is_executable(reinterpret_cast<void*>(in_main_thread))) {
        store().fail("Input initialization: tier0 ThreadInMainThread is missing or not executable");
        return false;
    }

    int interface_result = 0;
    input_system = factory("InputSystemVersion001", &interface_result);
    if (!readable(input_system, 0x2878)) {
        store().fail(
            fmt::format("Input initialization: InputSystemVersion001 is unavailable or unreadable "
                        "(result {}, address {})",
                        interface_result, input_system));
        return false;
    }

    auto table = *reinterpret_cast<void***>(input_system);
    if (!readable(table, (set_relative_mode + 1) * sizeof(void*))) {
        store().fail("Input initialization: InputSystemVersion001 vtable is unreadable");
        return false;
    }

    const int slots[] = {enable_input, poll_input, reset_input,       get_standard_cursor, set_cursor,
                         set_capture,  set_clip,   set_relative_mode, get_analog_delta};
    for (int slot : slots) {
        if (!is_executable(table[slot])) {
            store().fail(fmt::format("Input initialization: vtable slot {} is not executable ({})", slot, table[slot]));
            return false;
        }
    }

    const auto mouse_move = find_pattern("inputsystem.dll", "48 89 74 24 ?? 57 48 83 EC 70 F3 0F 10 8A");
    if (!is_executable(mouse_move)) {
        store().fail("Input initialization: SDL mouse-coordinate handler is unavailable or not executable");
        return false;
    }

    // The engine initializes index 0 to null and selects index 1 as its default cursor.
    arrow_cursor = call_virtual<void*>(input_system, get_standard_cursor, default_cursor_index);
    if (!readable(arrow_cursor, sizeof(void*))) {
        store().fail("Input initialization: default cursor (index 1) is unavailable or unreadable");
        return false;
    }

    static auto hooks = std::make_unique<HookSet>();
    if (!hooks->create(table[enable_input], &hk_enable, o_enable) ||
        !hooks->create(mouse_move, &hk_mouse_move, o_mouse_move) ||
        !hooks->create(table[get_analog_delta], &hk_analog_delta, o_analog_delta) ||
        !hooks->create(table[set_relative_mode], &hk_relative, o_relative) ||
        !hooks->create(table[set_capture], &hk_capture, o_capture) ||
        !hooks->create(table[set_clip], &hk_clip, o_clip) || !hooks->create(table[set_cursor], &hk_cursor, o_cursor) ||
        !hooks->create(table[poll_input], &hk_poll, o_poll) || !hooks->enable()) {
        hooks->reset();
        store().fail("Input initialization: could not install input hooks; see MinHook diagnostics");
        return false;
    }

    store().set_input_ready();
    spdlog::info("InputSystemVersion001 hooks ready; interface {}, default cursor {}", input_system, arrow_cursor);
    return true;
}
