#include "hooks.hpp"

#include <MinHook.h>
#include <spdlog/spdlog.h>

bool initialize_hooks() {
    const auto result = MH_Initialize();
    if (result != MH_OK && result != MH_ERROR_ALREADY_INITIALIZED)
        spdlog::error("InitializeHooks: {}", MH_StatusToString(result));
    return result == MH_OK || result == MH_ERROR_ALREADY_INITIALIZED;
}

HookSet::~HookSet() { reset(); }

bool HookSet::create_hook(void* target, void* detour, void** original) {
    if (!target) return false;

    targets_.push_back(target);
    const auto result = MH_CreateHook(target, detour, original);
    if (result != MH_OK) {
        targets_.pop_back();
        spdlog::error("CreateHook at {}: {}", target, MH_StatusToString(result));
        return false;
    }

    return true;
}

bool HookSet::enable() {
    for (auto target : targets_) {
        const auto result = MH_EnableHook(target);
        if (result != MH_OK) {
            spdlog::error("EnableHook at {}: {}", target, MH_StatusToString(result));
            reset();
            return false;
        }
    }

    return true;
}

void HookSet::reset() noexcept {
    // Never disable hooks owned by another injected module.
    for (auto target : targets_) {
        MH_DisableHook(target);
        MH_RemoveHook(target);
    }

    targets_.clear();
}
