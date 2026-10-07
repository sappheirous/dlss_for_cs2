#pragma once

#include <vector>

class HookSet {
public:
    HookSet() = default;
    ~HookSet();

    HookSet(const HookSet&) = delete;
    HookSet& operator=(const HookSet&) = delete;
    HookSet(HookSet&&) = delete;
    HookSet& operator=(HookSet&&) = delete;

    template <typename Function>
    bool create(void* target, Function detour, Function& original) {
        return create_hook(target, reinterpret_cast<void*>(detour), reinterpret_cast<void**>(&original));
    }

    bool enable();
    void reset() noexcept;

private:
    bool create_hook(void* target, void* detour, void** original);

    std::vector<void*> targets_;
};

bool initialize_hooks();
