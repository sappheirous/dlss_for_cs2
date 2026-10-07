#include "runtime.hpp"

#include <algorithm>
#include <array>
#include <exception>

#include <spdlog/spdlog.h>

#include "dlss.hpp"
#include "game.hpp"
#include "hooks.hpp"
#include "input.hpp"
#include "logging.hpp"
#include "overlay.hpp"
#include "settings.hpp"

namespace {
bool wait_for_modules() {
    constexpr std::array modules{L"client.dll",           L"engine2.dll",     L"scenesystem.dll",
                                 L"rendersystemdx11.dll", L"inputsystem.dll", L"tier0.dll"};
    const auto start = GetTickCount64();

    while (GetTickCount64() - start < 30000) {
        if (std::ranges::all_of(modules, [](const auto module) { return GetModuleHandleW(module) != nullptr; }))
            return true;

        Sleep(100);
    }

    return false;
}

void poll_menu_key(bool& was_down) {
    DWORD foreground_process = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &foreground_process);
    const bool focused = foreground_process == GetCurrentProcessId();
    const bool is_down = (GetAsyncKeyState(VK_INSERT) & 0x8000) != 0;

    if (focused && is_down && !was_down) {
        set_menu_open(!menu_open());
        spdlog::info("Overlay menu {}", menu_open() ? "opened" : "closed");
    }

    static ULONGLONG unfocused_since = 0;
    if (focused || renderer().display_change_active())
        unfocused_since = 0;
    else if (!unfocused_since)
        unfocused_since = GetTickCount64();
    else if (GetTickCount64() - unfocused_since >= 500)
        set_menu_open(false);
    was_down = is_down;
}
}  // namespace

DWORD WINAPI bootstrap(void*) noexcept {
    try {
        initialize_logging();
        store().load();
        spdlog::info("CS2-DLSS starting; Insert toggles menu");

        if (!wait_for_modules()) {
            store().fail("Required DX11 modules did not load within 30 seconds");
            return 0;
        }

        if (!initialize_hooks()) {
            store().fail("Could not initialize MinHook");
            return 0;
        }

        const bool input_ready = initialize_input();
        const bool game_ready = input_ready && initialize_game();
        if (!game_ready && !store().has_failed())
            store().fail("Source 2 integration unavailable; see log for missing interfaces or signatures");

        if (!initialize_overlay()) {
            store().fail("Could not install DXGI overlay hooks");
            return 0;
        }

        spdlog::info("Display-transition hooks enabled");

        // Persistence and disk I/O stay off the game's main and render threads.
        bool insert_was_down = false;
        auto last_disk_update = GetTickCount64();

        for (;;) {
            poll_menu_key(insert_was_down);
            pump_overlay_input();

            if (GetTickCount64() - last_disk_update >= 100) {
                store().save_pending();
                last_disk_update = GetTickCount64();
            }

            Sleep(10);
        }
    } catch (const std::exception& error) {
        OutputDebugStringA(error.what());
        spdlog::error("Bootstrap failed: {}", error.what());
    }

    return 0;
}
